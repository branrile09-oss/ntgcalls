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

    // Retinal AV1 hardware config: once called, AV1 is hardware-only. With
    // encode, AV1 is offered and created only through the Android hardware
    // encoder factory (never libaom); with decode, it is accepted and created
    // only through the hardware decoder factory (never dav1d or a platform
    // software decoder). A false direction has no AV1 at all. Other codecs
    // keep the stock factories. Never called: upstream behaviour.
    // Process-wide: applies only before the codec factories are first
    // created; returns whether the setting was applied.
    bool setAv1HardwareCapabilities(bool encode, bool decode);

    // Retinal AV1 hardware config: the AV1 formats the hardware decoder
    // factory offers for 1:1 incoming video; empty unless hardware AV1 decode
    // was enabled and a hardware AV1 decoder exists.
    std::vector<webrtc::SdpVideoFormat> av1IncomingVideoFormats();

    // Retinal encoder handover: with enabled, encoders created by the video
    // encoder factory hand over to a second encoder on a frame-size change
    // instead of restarting (see HandoverEncoderFactory). Process-wide:
    // applies only before the encoder factory is first created; returns
    // whether the setting was applied. Default false: upstream behaviour.
    bool setVideoEncoderHandover(bool enabled);

    // Retinal adaptive decoder: when enabled, hardware decoders come from
    // org.webrtc.RetinalAdaptiveVideoDecoderFactory (WebRTC's hardware codec
    // selection; decoders with adaptive playback keep running across frame-size
    // changes). Software fallbacks are unchanged. Process-wide; applies only
    // before the decoder factory is first created; returns whether the setting
    // was applied. Default false: upstream behaviour.
    bool setVideoDecoderAdaptivePlayback(bool enabled);

    std::unique_ptr<webrtc::VideoEncoderFactory> CreateVideoEncoderFactory(JNIEnv* env);

    std::unique_ptr<webrtc::VideoDecoderFactory> CreateVideoDecoderFactory(JNIEnv* env);
} // android

#endif