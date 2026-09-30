//
// Created by Laky64 on 15/09/24.
//

#ifdef IS_ANDROID
#include <wrtc/video_factory/hardware/android/video_factory.hpp>
#include <sdk/android/native_api/codecs/wrapper.h>
#include <sdk/android/native_api/jni/class_loader.h>
#include <sdk/android/native_api/jni/scoped_java_ref.h>
#include <mutex>
#include <rtc_base/logging.h>

namespace android {
    // Retinal encoder config
    namespace {
        std::mutex encoderConfigMutex;
        bool encoderSharedEglContext = true;
        bool encoderFactoryCreated = false;
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
        {
            std::lock_guard lock(encoderConfigMutex);
            encoderFactoryCreated = true;
            useSharedEglContext = encoderSharedEglContext;
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
        return webrtc::JavaToNativeVideoEncoderFactory(env, factoryObject.obj());
    }

    std::unique_ptr<webrtc::VideoDecoderFactory> CreateVideoDecoderFactory(JNIEnv* env) {
        const webrtc::ScopedJavaLocalRef<jclass> javaVideoCapturerModule = webrtc::GetClass(env, "io/github/pytgcalls/devices/JavaVideoCapturerModule");
        // ReSharper disable once CppLocalVariableMayBeConst
        jmethodID getEglContext = env->GetStaticMethodID(javaVideoCapturerModule.obj(), "getSharedEGLContext", "()Lorg/webrtc/EglBase$Context;");
        const auto eglContext = env->CallStaticObjectMethod(javaVideoCapturerModule.obj(), getEglContext);

        const webrtc::ScopedJavaLocalRef<jclass> factoryClass = webrtc::GetClass(env, "org/webrtc/DefaultVideoDecoderFactory");
        // ReSharper disable once CppLocalVariableMayBeConst
        jmethodID factoryConstructor = env->GetMethodID(factoryClass.obj(), "<init>", "(Lorg/webrtc/EglBase$Context;)V");
        const auto factoryObject = webrtc::ScopedJavaLocalRef<>::Adopt(
            env,
            env->NewObject(factoryClass.obj(), factoryConstructor, eglContext)
        );
        return webrtc::JavaToNativeVideoDecoderFactory(env, factoryObject.obj());
    }
} // android
#endif