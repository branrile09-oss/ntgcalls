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
        std::vector<std::string> codecPreferences,
        const int maxFramerate
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
        if (maxFramerate > 0) { // Retinal max framerate seam: before any frame reaches an encoder
            workerThread.BlockingCall([&] {
                applyMaxFramerate(maxFramerate);
            });
        }
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
            if (rtpParameters.encodings.size() > 1) { // Retinal group bitrate seam
                setGroupMaxBitrate(rtpParameters, bps);
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

    // Retinal group bitrate seam. A group call's channel declares three simulcast layers,
    // but the Android hardware encoder produces only the first (its output carries no
    // simulcast index, so WebRTC sends it as layer 0) while being given the sum of every
    // active layer's allocation. A ceiling for the whole outgoing video therefore caps
    // encodings[0] and deactivates the other encodings for bitrate allocation. Their
    // SSRCs stay declared, and WebRTC keeps the RTP modules of one sender active or
    // inactive together, so nothing changes on the wire; deactivating a layer does not
    // re-initialise the encoder. Clearing the ceiling restores the previous `active`
    // values (re-activating a layer re-initialises the encoder once, as upstream would).
    void OutgoingVideoChannel::setGroupMaxBitrate(webrtc::RtpParameters& rtpParameters, const int bps) const {
        auto& encodings = rtpParameters.encodings;
        std::optional<std::vector<bool>> saved = savedEncodingsActive;
        if (bps > 0) {
            if (!saved) {
                saved.emplace();
                for (const auto& encoding : encodings) {
                    saved->push_back(encoding.active);
                }
            }
            for (size_t i = 1; i < encodings.size(); i++) {
                encodings[i].active = false;
            }
            encodings[0].max_bitrate_bps = bps;
        } else {
            if (saved) {
                for (size_t i = 0; i < encodings.size() && i < saved->size(); i++) {
                    encodings[i].active = (*saved)[i];
                }
                saved.reset();
            }
            encodings[0].max_bitrate_bps = std::nullopt;
        }
        const auto result = channel->video_media_send_channel()->SetRtpSendParameters(_ssrc, rtpParameters);
        if (result.ok()) {
            savedEncodingsActive = std::move(saved);
        }
        std::string active;
        for (const auto& encoding : encodings) {
            active += encoding.active ? "1" : "0";
        }
        RTC_LOG(LS_INFO) << "[Retinal group bitrate seam] encodings=" << encodings.size()
                         << " encodings[0].max_bitrate_bps=" << bps << " active=" << active
                         << " result=" << (result.ok() ? "ok" : result.message());
    }

    void OutgoingVideoChannel::setMaxFramerate(const int fps) const {
        workerThread.BlockingCall([&] {
            applyMaxFramerate(fps);
        });
    }

    void OutgoingVideoChannel::applyMaxFramerate(const int fps) const {
        webrtc::RtpParameters rtpParameters = channel->video_media_send_channel()->GetRtpSendParameters(_ssrc);
        if (rtpParameters.encodings.empty()) {
            RTC_LOG(LS_WARNING) << "[Retinal max framerate seam] no encodings";
            return;
        }
        if (rtpParameters.encodings.size() > 1) {
            // Retinal group framerate seam: the encoder runs at the highest max_framerate
            // of any encoding, active or not, so every encoding gets the limit.
            for (auto& encoding : rtpParameters.encodings) {
                encoding.max_framerate = fps > 0 ? std::optional<double>(fps) : std::nullopt;
            }
            const auto result = channel->video_media_send_channel()->SetRtpSendParameters(_ssrc, rtpParameters);
            RTC_LOG(LS_INFO) << "[Retinal group framerate seam] encodings=" << rtpParameters.encodings.size()
                             << " max_framerate=" << fps << " result=" << (result.ok() ? "ok" : result.message());
            return;
        }
        if (fps > 0) {
            rtpParameters.encodings[0].max_framerate = static_cast<double>(fps);
        } else {
            rtpParameters.encodings[0].max_framerate = std::nullopt;
        }
        const auto result = channel->video_media_send_channel()->SetRtpSendParameters(_ssrc, rtpParameters);
        RTC_LOG(LS_INFO) << "[Retinal max framerate seam] encodings[0].max_framerate="
                         << fps << " result=" << (result.ok() ? "ok" : result.message());
    }

    bool OutgoingVideoChannel::getStats(webrtc::VideoMediaSendInfo* info) const {
        return channel && channel->video_media_send_channel()->GetStats(info);
    }
} // wrtc