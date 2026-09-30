//
// Created by Laky64 on 15/09/24.
//

#pragma once
#ifdef IS_ANDROID
#include <sdk/android/native_api/jni/jvm.h>
#include <api/video_codecs/video_encoder_factory.h>
#include <api/video_codecs/video_decoder_factory.h>

namespace android {
    // Retinal encoder config: false builds the hardware video encoder factory
    // without the shared EGL context, so encoders take byte-buffer (I420)
    // input from the start instead of starting in surface mode and being
    // re-created on the first I420 frame. Default true (upstream behaviour).
    // Process-wide: applies only before the encoder factory is first created;
    // returns whether the setting was applied.
    bool setVideoEncoderSharedEglContext(bool enabled);

    std::unique_ptr<webrtc::VideoEncoderFactory> CreateVideoEncoderFactory(JNIEnv* env);

    std::unique_ptr<webrtc::VideoDecoderFactory> CreateVideoDecoderFactory(JNIEnv* env);
} // android

#endif