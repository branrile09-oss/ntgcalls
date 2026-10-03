//
// Retinal encoder handover (D-028).
//

#pragma once

#ifdef IS_ANDROID
#include <memory>

#include <api/environment/environment.h>
#include <api/video_codecs/sdp_video_format.h>
#include <api/video_codecs/video_encoder.h>
#include <api/video_codecs/video_encoder_factory.h>

namespace android {
    // Retinal encoder handover: on a frame-size change WebRTC releases the
    // hardware encoder and starts a new one, which leaves the call without
    // frames for about 230 ms on the S21. Encoders from this factory instead
    // keep the old encoder sending (frames fitted to its size: a wider frame
    // is cropped at the centre, a narrower one gets side bars) while a second
    // encoder starts at the new size on its own thread. Once that encoder is
    // warm, a key frame is requested and the stream switches to it; frames
    // of the old encoder for that frame or later are dropped, so the stream
    // continues without a gap or duplicate. Each inner encoder runs on its
    // own thread (WebRTC's Java encoders are bound to the thread that first
    // uses them). Codec changes and same-size re-initialisations restart the
    // encoder as before; if the new encoder cannot start, the old one is
    // restarted at the new size (stock behaviour).
    class HandoverEncoderFactory final : public webrtc::VideoEncoderFactory {
    public:
        explicit HandoverEncoderFactory(std::unique_ptr<webrtc::VideoEncoderFactory> inner);

        std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override;
        std::vector<webrtc::SdpVideoFormat> GetImplementations() const override;
        using VideoEncoderFactory::QueryCodecSupport;
        CodecSupport QueryCodecSupport(const webrtc::SdpVideoFormat& format, std::optional<std::string> scalabilityMode,
                                       std::optional<webrtc::Resolution> resolution) const override;
        std::unique_ptr<webrtc::VideoEncoder> Create(const webrtc::Environment& env, const webrtc::SdpVideoFormat& format) override;
        std::unique_ptr<EncoderSelectorInterface> GetEncoderSelector() const override;

    private:
        std::unique_ptr<webrtc::VideoEncoderFactory> inner;
    };
} // android
#endif
