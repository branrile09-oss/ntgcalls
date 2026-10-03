//
// Created by Laky64 on 15/09/24.
//

#ifdef IS_ANDROID
#include <wrtc/video_factory/hardware/android/video_factory.hpp>
#include <wrtc/video_factory/hardware/android/handover_encoder.hpp>
#include <sdk/android/native_api/codecs/wrapper.h>
#include <sdk/android/native_api/jni/class_loader.h>
#include <sdk/android/native_api/jni/scoped_java_ref.h>
#include <mutex>
#include <absl/strings/match.h>
#include <media/base/media_constants.h>
#include <rtc_base/logging.h>

namespace android {
    // Retinal encoder config
    namespace {
        std::mutex encoderConfigMutex;
        bool encoderSharedEglContext = true;
        bool encoderFactoryCreated = false;

        // Retinal AV1 hardware config (guarded by encoderConfigMutex).
        bool av1Configured = false;
        bool av1HardwareEncode = false;
        bool av1HardwareDecode = false;
        bool decoderFactoryCreated = false;
        std::vector<webrtc::SdpVideoFormat> av1IncomingFormats;

        // Retinal encoder handover (guarded by encoderConfigMutex).
        bool encoderHandover = false;

        // Retinal adaptive decoder (guarded by encoderConfigMutex).
        bool decoderAdaptivePlayback = false;

        std::unique_ptr<webrtc::VideoEncoderFactory> withHandover(std::unique_ptr<webrtc::VideoEncoderFactory> factory, const bool enabled) {
            if (!enabled) return factory;
            RTC_LOG(LS_INFO) << "[Retinal handover] encoder handover on frame-size changes enabled";
            return std::make_unique<HandoverEncoderFactory>(std::move(factory));
        }

        bool isAv1(const webrtc::SdpVideoFormat& format) {
            return absl::EqualsIgnoreCase(format.name, webrtc::kAv1CodecName);
        }

        std::vector<webrtc::SdpVideoFormat> withoutAv1(std::vector<webrtc::SdpVideoFormat> formats) {
            std::erase_if(formats, isAv1);
            return formats;
        }

        std::vector<webrtc::SdpVideoFormat> onlyAv1(std::vector<webrtc::SdpVideoFormat> formats) {
            std::erase_if(formats, [](const webrtc::SdpVideoFormat& f) { return !isAv1(f); });
            return formats;
        }

        // Retinal AV1 hardware config: every codec except AV1 comes from the
        // stock factory (hardware with its software fallback, unchanged);
        // AV1 comes only from the hardware factory, or not at all.
        class Av1HardwareOnlyEncoderFactory final : public webrtc::VideoEncoderFactory {
        public:
            Av1HardwareOnlyEncoderFactory(
                std::unique_ptr<VideoEncoderFactory> stock,
                std::unique_ptr<VideoEncoderFactory> av1Hardware
            ): stock(std::move(stock)), av1Hardware(std::move(av1Hardware)) {}

            std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
                auto formats = withoutAv1(stock->GetSupportedFormats());
                if (av1Hardware) {
                    for (auto& f : onlyAv1(av1Hardware->GetSupportedFormats())) formats.push_back(std::move(f));
                }
                return formats;
            }

            std::vector<webrtc::SdpVideoFormat> GetImplementations() const override {
                auto formats = withoutAv1(stock->GetImplementations());
                if (av1Hardware) {
                    for (auto& f : onlyAv1(av1Hardware->GetImplementations())) formats.push_back(std::move(f));
                }
                return formats;
            }

            using VideoEncoderFactory::QueryCodecSupport;
            CodecSupport QueryCodecSupport(
                const webrtc::SdpVideoFormat& format,
                std::optional<std::string> scalabilityMode,
                std::optional<webrtc::Resolution> resolution
            ) const override {
                if (isAv1(format)) {
                    return av1Hardware ? av1Hardware->QueryCodecSupport(format, scalabilityMode, resolution) : CodecSupport{};
                }
                return stock->QueryCodecSupport(format, scalabilityMode, resolution);
            }

            std::unique_ptr<webrtc::VideoEncoder> Create(const webrtc::Environment& env, const webrtc::SdpVideoFormat& format) override {
                if (isAv1(format)) {
                    // No software AV1: without a hardware encoder there is no AV1 encoder.
                    return av1Hardware ? av1Hardware->Create(env, format) : nullptr;
                }
                return stock->Create(env, format);
            }

            std::unique_ptr<EncoderSelectorInterface> GetEncoderSelector() const override {
                return stock->GetEncoderSelector();
            }

        private:
            std::unique_ptr<VideoEncoderFactory> stock;
            std::unique_ptr<VideoEncoderFactory> av1Hardware;
        };

        class Av1HardwareOnlyDecoderFactory final : public webrtc::VideoDecoderFactory {
        public:
            Av1HardwareOnlyDecoderFactory(
                std::unique_ptr<VideoDecoderFactory> stock,
                std::unique_ptr<VideoDecoderFactory> av1Hardware
            ): stock(std::move(stock)), av1Hardware(std::move(av1Hardware)) {}

            std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
                auto formats = withoutAv1(stock->GetSupportedFormats());
                if (av1Hardware) {
                    for (auto& f : onlyAv1(av1Hardware->GetSupportedFormats())) formats.push_back(std::move(f));
                }
                return formats;
            }

            using VideoDecoderFactory::QueryCodecSupport;
            CodecSupport QueryCodecSupport(
                const webrtc::SdpVideoFormat& format,
                const bool referenceScaling,
                std::optional<webrtc::Resolution> resolution
            ) const override {
                if (isAv1(format)) {
                    return av1Hardware ? av1Hardware->QueryCodecSupport(format, referenceScaling, resolution) : CodecSupport{};
                }
                return stock->QueryCodecSupport(format, referenceScaling, resolution);
            }

            std::unique_ptr<webrtc::VideoDecoder> Create(const webrtc::Environment& env, const webrtc::SdpVideoFormat& format) override {
                if (isAv1(format)) {
                    // No software AV1: without a hardware decoder there is no AV1 decoder.
                    return av1Hardware ? av1Hardware->Create(env, format) : nullptr;
                }
                return stock->Create(env, format);
            }

        private:
            std::unique_ptr<VideoDecoderFactory> stock;
            std::unique_ptr<VideoDecoderFactory> av1Hardware;
        };
    }

    bool setAv1HardwareCapabilities(const bool encode, const bool decode) {
        std::lock_guard lock(encoderConfigMutex);
        if (encoderFactoryCreated || decoderFactoryCreated) {
            return false;
        }
        av1Configured = true;
        av1HardwareEncode = encode;
        av1HardwareDecode = decode;
        RTC_LOG(LS_INFO) << "[Retinal AV1] hardware-only AV1: encode=" << encode << " decode=" << decode;
        return true;
    }

    std::vector<webrtc::SdpVideoFormat> av1IncomingVideoFormats() {
        std::lock_guard lock(encoderConfigMutex);
        return av1IncomingFormats;
    }

    bool setVideoEncoderHandover(const bool enabled) {
        std::lock_guard lock(encoderConfigMutex);
        if (encoderFactoryCreated) {
            return false;
        }
        encoderHandover = enabled;
        return true;
    }

    bool setVideoDecoderAdaptivePlayback(const bool enabled) {
        std::lock_guard lock(encoderConfigMutex);
        if (decoderFactoryCreated) {
            return false;
        }
        decoderAdaptivePlayback = enabled;
        return true;
    }

    bool setVideoEncoderSharedEglContext(const bool enabled) {
        std::lock_guard lock(encoderConfigMutex);
        if (encoderFactoryCreated) {
            return false;
        }
        encoderSharedEglContext = enabled;
        return true;
    }

    std::unique_ptr<webrtc::VideoEncoderFactory> CreateVideoEncoderFactory(JNIEnv* env) {
        bool useSharedEglContext;
        bool av1Wrap;
        bool av1Encode;
        bool handover;
        {
            std::lock_guard lock(encoderConfigMutex);
            encoderFactoryCreated = true;
            handover = encoderHandover;
            useSharedEglContext = encoderSharedEglContext;
            av1Wrap = av1Configured;
            av1Encode = av1HardwareEncode;
        }
        jobject eglContext = nullptr;
        if (useSharedEglContext) {
            const webrtc::ScopedJavaLocalRef<jclass> javaVideoCapturerModule = webrtc::GetClass(env, "io/github/pytgcalls/devices/JavaVideoCapturerModule");
            // ReSharper disable once CppLocalVariableMayBeConst
            jmethodID getEglContext = env->GetStaticMethodID(javaVideoCapturerModule.obj(), "getSharedEGLContext", "()Lorg/webrtc/EglBase$Context;");
            eglContext = env->CallStaticObjectMethod(javaVideoCapturerModule.obj(), getEglContext);
        } else {
            RTC_LOG(LS_INFO) << "[Retinal encoder config] hardware video encoder factory without shared EGL context (byte-buffer input)";
        }

        const webrtc::ScopedJavaLocalRef<jclass> factoryClass = webrtc::GetClass(env, "org/webrtc/DefaultVideoEncoderFactory");
        // ReSharper disable once CppLocalVariableMayBeConst
        jmethodID factoryConstructor = env->GetMethodID(factoryClass.obj(), "<init>", "(Lorg/webrtc/EglBase$Context;ZZ)V");
        const auto factoryObject = webrtc::ScopedJavaLocalRef<>::Adopt(
            env,
            env->NewObject(factoryClass.obj(), factoryConstructor, eglContext, false, true)
        );
        auto stock = webrtc::JavaToNativeVideoEncoderFactory(env, factoryObject.obj());
        if (!av1Wrap) {
            return withHandover(std::move(stock), handover);
        }
        std::unique_ptr<webrtc::VideoEncoderFactory> av1Hardware;
        if (av1Encode) {
            // The same hardware factory DefaultVideoEncoderFactory wraps, without its software side.
            const webrtc::ScopedJavaLocalRef<jclass> hardwareClass = webrtc::GetClass(env, "org/webrtc/HardwareVideoEncoderFactory");
            // ReSharper disable once CppLocalVariableMayBeConst
            jmethodID hardwareConstructor = env->GetMethodID(hardwareClass.obj(), "<init>", "(Lorg/webrtc/EglBase$Context;ZZ)V");
            const auto hardwareObject = webrtc::ScopedJavaLocalRef<>::Adopt(
                env,
                env->NewObject(hardwareClass.obj(), hardwareConstructor, eglContext, false, true)
            );
            av1Hardware = webrtc::JavaToNativeVideoEncoderFactory(env, hardwareObject.obj());
        }
        auto factory = std::make_unique<Av1HardwareOnlyEncoderFactory>(std::move(stock), std::move(av1Hardware));
        const auto av1 = onlyAv1(factory->GetSupportedFormats());
        RTC_LOG(LS_INFO) << "[Retinal AV1] encoder factory: AV1 " << (av1.empty() ? "not offered" : "hardware only");
        return withHandover(std::move(factory), handover);
    }

    std::unique_ptr<webrtc::VideoDecoderFactory> CreateVideoDecoderFactory(JNIEnv* env) {
        bool av1Wrap;
        bool av1Decode;
        bool adaptive;
        {
            std::lock_guard lock(encoderConfigMutex);
            decoderFactoryCreated = true;
            av1Wrap = av1Configured;
            av1Decode = av1HardwareDecode;
            adaptive = decoderAdaptivePlayback;
        }
        if (adaptive) {
            RTC_LOG(LS_INFO) << "[Retinal adaptive decoder] hardware decoders follow frame-size changes";
        }
        const webrtc::ScopedJavaLocalRef<jclass> javaVideoCapturerModule = webrtc::GetClass(env, "io/github/pytgcalls/devices/JavaVideoCapturerModule");
        // ReSharper disable once CppLocalVariableMayBeConst
        jmethodID getEglContext = env->GetStaticMethodID(javaVideoCapturerModule.obj(), "getSharedEGLContext", "()Lorg/webrtc/EglBase$Context;");
        const auto eglContext = env->CallStaticObjectMethod(javaVideoCapturerModule.obj(), getEglContext);

        // Retinal adaptive decoder: the same combination with the adaptive hardware factory.
        const webrtc::ScopedJavaLocalRef<jclass> factoryClass = webrtc::GetClass(env, adaptive ? "org/webrtc/RetinalDefaultVideoDecoderFactory" : "org/webrtc/DefaultVideoDecoderFactory");
        // ReSharper disable once CppLocalVariableMayBeConst
        jmethodID factoryConstructor = env->GetMethodID(factoryClass.obj(), "<init>", "(Lorg/webrtc/EglBase$Context;)V");
        const auto factoryObject = webrtc::ScopedJavaLocalRef<>::Adopt(
            env,
            env->NewObject(factoryClass.obj(), factoryConstructor, eglContext)
        );
        auto stock = webrtc::JavaToNativeVideoDecoderFactory(env, factoryObject.obj());
        if (!av1Wrap) {
            return stock;
        }
        std::unique_ptr<webrtc::VideoDecoderFactory> av1Hardware;
        if (av1Decode) {
            // The same hardware factory DefaultVideoDecoderFactory wraps, without
            // its software (dav1d) and platform software sides.
            const webrtc::ScopedJavaLocalRef<jclass> hardwareClass = webrtc::GetClass(env, adaptive ? "org/webrtc/RetinalAdaptiveVideoDecoderFactory" : "org/webrtc/HardwareVideoDecoderFactory");
            // ReSharper disable once CppLocalVariableMayBeConst
            jmethodID hardwareConstructor = env->GetMethodID(hardwareClass.obj(), "<init>", "(Lorg/webrtc/EglBase$Context;)V");
            const auto hardwareObject = webrtc::ScopedJavaLocalRef<>::Adopt(
                env,
                env->NewObject(hardwareClass.obj(), hardwareConstructor, eglContext)
            );
            av1Hardware = webrtc::JavaToNativeVideoDecoderFactory(env, hardwareObject.obj());
        }
        auto factory = std::make_unique<Av1HardwareOnlyDecoderFactory>(std::move(stock), std::move(av1Hardware));
        auto av1 = onlyAv1(factory->GetSupportedFormats());
        RTC_LOG(LS_INFO) << "[Retinal AV1] decoder factory: AV1 " << (av1.empty() ? "not accepted" : "hardware only");
        {
            std::lock_guard lock(encoderConfigMutex);
            av1IncomingFormats = std::move(av1);
        }
        return factory;
    }
} // android
#endif