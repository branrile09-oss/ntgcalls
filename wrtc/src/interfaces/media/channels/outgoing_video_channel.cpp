//
// Created by Laky64 on 02/04/2024.
//

#include <wrtc/interfaces/media/channels/outgoing_video_channel.hpp>

#include <wrtc/interfaces/native_connection.hpp>
#include <api/video/builtin_video_bitrate_allocator_factory.h>
#include <rtc_base/logging.h>

namespace wrtc {
    OutgoingVideoChannel::OutgoingVideoChannel(
        webrtc::Call* call,
        ChannelManager* channelManager,
        webrtc::RtpTransport* rtpTransport,
        const MediaContent &mediaContent,
        SafeThread& workerThread,
        SafeThread& networkThread,
        LocalVideoAdapter* sink,
        std::vector<std::string> codecPreferences
    ): _ssrc(mediaContent.ssrc), workerThread(workerThread), networkThread(networkThread), sink(sink) {
        webrtc::VideoOptions videoOptions;
        videoOptions.is_screencast = mediaContent.isScreenCast();
        bitrateAllocatorFactory = webrtc::CreateBuiltinVideoBitrateAllocatorFactory();
        channel = channelManager->CreateVideoChannel(
            call,
            webrtc::MediaConfig(),
            std::to_string(_ssrc),
            false,
            NativeNetworkInterface::getDefaultCryptoOptions(),
            videoOptions,
            bitrateAllocatorFactory.get()
        );
        networkThread.BlockingCall([&] {
            channel->SetRtpTransport(rtpTransport);
        });
        std::vector<webrtc::Codec> unsortedCodecs;
        for (const auto &[id, name, clockrate, channels, feedbackTypes, parameters] : mediaContent.payloadTypes) {
            webrtc::Codec codec = webrtc::CreateVideoCodec(static_cast<int>(id), name);
            for (const auto &[fst, snd] : parameters) {
                codec.SetParam(fst, snd);
            }
            for (const auto &[type, subtype] : feedbackTypes) {
                codec.AddFeedbackParam(webrtc::FeedbackParam(type, subtype));
            }
            unsortedCodecs.push_back(std::move(codec));
        }
        // Retinal codec seam: order the negotiated codecs by the caller's preference;
        // the first negotiated codec becomes the send codec. Codecs the peer did
        // not accept are never introduced here, so fallback is automatic.
        const bool hasCallerPreference = !codecPreferences.empty();
        if (!hasCallerPreference) {
            codecPreferences = {webrtc::kH264CodecName};
        }
        std::vector<webrtc::Codec> codecs;
        for (const auto &name : codecPreferences) {
            for (const auto &codec : unsortedCodecs) {
                if (codec.name == name) {
                    codecs.push_back(codec);
                }
            }
        }
        for (const auto &codec : unsortedCodecs) {
            if (std::ranges::find(codecs, codec) == codecs.end()) {
                codecs.push_back(codec);
            }
        }

        if (hasCallerPreference) {
            std::string requested, ordered;
            for (const auto &name : codecPreferences) requested += name + " ";
            for (const auto &codec : codecs) ordered += codec.name + "/" + std::to_string(codec.id) + " ";
            RTC_LOG(LS_INFO) << "[Retinal codec seam] outgoing video preference: " << requested
                             << "| negotiated order: " << ordered;
        }
        auto outgoingVideoDescription = std::make_unique<webrtc::VideoContentDescription>();
        for (const auto &rtpExtension : mediaContent.rtpExtensions) {
            outgoingVideoDescription->AddRtpHeaderExtension(rtpExtension);
        }
        outgoingVideoDescription->set_rtcp_mux(true);
        outgoingVideoDescription->set_rtcp_reduced_size(true);
        outgoingVideoDescription->set_direction(webrtc::RtpTransceiverDirection::kSendOnly);
        outgoingVideoDescription->set_codecs(codecs);
        outgoingVideoDescription->set_bandwidth(-1);
        webrtc::StreamParams videoSendStreamParams;
        for (const auto &[semantics, ssrcs] : mediaContent.ssrcGroups) {
            for (auto ssrc : ssrcs) {
                if (!videoSendStreamParams.has_ssrc(ssrc)) {
                    videoSendStreamParams.ssrcs.push_back(ssrc);
                }
            }
            webrtc::SsrcGroup mappedGroup(semantics, ssrcs);
            videoSendStreamParams.ssrc_groups.push_back(std::move(mappedGroup));
        }
        videoSendStreamParams.cname = "cname";
        outgoingVideoDescription->AddStream(videoSendStreamParams);

        auto incomingVideoDescription = std::make_unique<webrtc::VideoContentDescription>();
        for (const auto &rtpExtension : mediaContent.rtpExtensions) {
            incomingVideoDescription->AddRtpHeaderExtension(webrtc::RtpExtension(rtpExtension.uri, rtpExtension.id));
        }
        incomingVideoDescription->set_rtcp_mux(true);
        incomingVideoDescription->set_rtcp_reduced_size(true);
        incomingVideoDescription->set_direction(webrtc::RtpTransceiverDirection::kRecvOnly);
        incomingVideoDescription->set_codecs(codecs);
        incomingVideoDescription->set_bandwidth(-1);
        workerThread.BlockingCall([&] {
            channel->rtp_transport()->SetActivePayloadTypeDemuxing(false);
            channel->SetLocalContent(outgoingVideoDescription.get(), webrtc::SdpType::kOffer);
            channel->SetRemoteContent(incomingVideoDescription.get(), webrtc::SdpType::kAnswer);
        });
        channel->Enable(true);
        set_enabled(true);
        workerThread.BlockingCall([&] {
            webrtc::RtpParameters rtpParameters = channel->video_media_send_channel()->GetRtpSendParameters(_ssrc);
            rtpParameters.degradation_preference = webrtc::DegradationPreference::MAINTAIN_RESOLUTION;
            channel->video_media_send_channel()->SetRtpSendParameters(_ssrc, rtpParameters);
        });
    }

    OutgoingVideoChannel::~OutgoingVideoChannel() {
        channel->Enable(false);
        networkThread.BlockingCall([&] {
            channel->SetRtpTransport(nullptr);
        });
        workerThread.BlockingCall([&] {
            channel = nullptr;
            bitrateAllocatorFactory = nullptr;
        });
        sink = nullptr;
    }

    void OutgoingVideoChannel::set_enabled(const bool enable) const {
        channel->Enable(enable);
        workerThread.BlockingCall([&] {
            channel->video_media_send_channel()->SetVideoSend(_ssrc, nullptr, enable ? sink:nullptr);
        });
    }

    uint32_t OutgoingVideoChannel::ssrc() const {
        return _ssrc;
    }

    void OutgoingVideoChannel::setMaxBitrate(const int bps) const {
        workerThread.BlockingCall([&] {
            webrtc::RtpParameters rtpParameters = channel->video_media_send_channel()->GetRtpSendParameters(_ssrc);
            if (rtpParameters.encodings.empty()) {
                RTC_LOG(LS_WARNING) << "[Retinal max bitrate seam] no encodings";
                return;
            }
            if (bps > 0) {
                rtpParameters.encodings[0].max_bitrate_bps = bps;
            } else {
                rtpParameters.encodings[0].max_bitrate_bps = std::nullopt;
            }
            const auto result = channel->video_media_send_channel()->SetRtpSendParameters(_ssrc, rtpParameters);
            RTC_LOG(LS_INFO) << "[Retinal max bitrate seam] encodings[0].max_bitrate_bps="
                             << bps << " result=" << (result.ok() ? "ok" : result.message());
        });
    }

    bool OutgoingVideoChannel::getStats(webrtc::VideoMediaSendInfo* info) const {
        return channel && channel->video_media_send_channel()->GetStats(info);
    }
} // wrtc