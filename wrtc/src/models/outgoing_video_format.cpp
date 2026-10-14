//
// Created by Laky64 on 04/11/24.
//

#include <absl/strings/match.h>
#include <rtc_base/logging.h>
#include <wrtc/models/outgoing_video_format.hpp>

namespace wrtc {
    OutgoingVideoFormat::OutgoingVideoFormat(webrtc::Codec videoCodec_, std::optional<webrtc::Codec> rtxCodec_) :
    videoCodec(std::move(videoCodec_)), rtxCodec(std::move(rtxCodec_)){}

    std::vector<webrtc::Codec> OutgoingVideoFormat::getVideoCodecs(
        [[maybe_unused]] const std::vector<webrtc::SdpVideoFormat>& formats,
        const std::vector<PayloadType>& payloadTypes,
        const bool isGroupConnection
    ) {
        std::vector<webrtc::Codec> codecs;
        // Retinal incoming group-video payload mapping (+retinal.12): the payload
        // table the caller supplies (the group call server's, or a peer's
        // negotiated one) is the only source, used with its codec parameters, RTX
        // associations and RTCP feedback. Upstream numbered a group connection's
        // formats locally (assignPayloadTypes), so one connection mapped a payload
        // type to different codecs for sending and receiving. With no table nothing
        // is invented: no codecs (a group connection's caller never gets here
        // without one, see IncomingVideoPayloadGate).
        if (payloadTypes.empty()) {
            if (isGroupConnection) {
                RTC_LOG(LS_ERROR) << "[Retinal group video] no negotiated video payload types;"
                                  << " not using locally numbered formats";
            }
            return codecs;
        }
        for (const auto &payloadType : payloadTypes) {
            webrtc::Codec codec = webrtc::CreateVideoCodec(payloadType.id, payloadType.name);
            for (const auto & [fst, snd] : payloadType.parameters) {
                codec.SetParam(fst, snd);
            }
            for (const auto & [type, subtype] : payloadType.feedbackTypes) {
                codec.AddFeedbackParam(webrtc::FeedbackParam(type, subtype));
            }
            codecs.push_back(std::move(codec));
        }
        return codecs;
    }

    std::vector<OutgoingVideoFormat> OutgoingVideoFormat::assignPayloadTypes(std::vector<webrtc::SdpVideoFormat> const& formats) {
        if (formats.empty()) {
            return {};
        }

        constexpr int kFirstDynamicPayloadType = 100;

        int payload_type = kFirstDynamicPayloadType;

        std::vector<OutgoingVideoFormat> result;

        const std::vector<std::string> filterCodecNames = {
            webrtc::kVp8CodecName,
            webrtc::kVp9CodecName,
            webrtc::kH264CodecName,
        };

        for (const auto &codecName : filterCodecNames) {
            for (const auto &format : formats) {
                constexpr int kLastDynamicPayloadType = 127;
                if (format.name != codecName) {
                    continue;
                }

                webrtc::Codec codec = webrtc::CreateVideoCodec(format);
                codec.id = payload_type;
                addDefaultFeedbackParams(&codec);

                ++payload_type;
                if (payload_type > kLastDynamicPayloadType) {
                    RTC_LOG(LS_ERROR) << "Out of dynamic payload types, skipping the rest.";
                    break;
                }

                std::optional<webrtc::Codec> rtxCodec;
                if (!absl::EqualsIgnoreCase(codec.name, webrtc::kUlpfecCodecName) && !absl::EqualsIgnoreCase(codec.name, webrtc::kFlexfecCodecName)) {
                    rtxCodec = webrtc::CreateVideoRtxCodec(payload_type, codec.id);

                    ++payload_type;
                    if (payload_type > kLastDynamicPayloadType) {
                        RTC_LOG(LS_ERROR) << "Out of dynamic payload types, skipping the rest.";
                        break;
                    }
                }

                OutgoingVideoFormat resultFormat(codec, rtxCodec);

                result.push_back(std::move(resultFormat));
            }
        }

        return result;
    }

    void OutgoingVideoFormat::addDefaultFeedbackParams(webrtc::Codec* codec) {
        if (codec->name == webrtc::kRedCodecName || codec->name == webrtc::kUlpfecCodecName) {
            return;
        }
        codec->AddFeedbackParam(webrtc::FeedbackParam(webrtc::kRtcpFbParamRemb, webrtc::kParamValueEmpty));
        codec->AddFeedbackParam(webrtc::FeedbackParam(webrtc::kRtcpFbParamTransportCc, webrtc::kParamValueEmpty));
        if (codec->name == webrtc::kFlexfecCodecName) {
            return;
        }
        codec->AddFeedbackParam(webrtc::FeedbackParam(webrtc::kRtcpFbParamCcm, webrtc::kRtcpFbCcmParamFir));
        codec->AddFeedbackParam(webrtc::FeedbackParam(webrtc::kRtcpFbParamNack, webrtc::kParamValueEmpty));
        codec->AddFeedbackParam(webrtc::FeedbackParam(webrtc::kRtcpFbParamNack, webrtc::kRtcpFbNackParamPli));
    }
} // wrtc