//
// Created by Laky64 on 02/04/2024.
//

#pragma once
#include <string>
#include <vector>
#include <call/call.h>
#include <pc/dtls_srtp_transport.h>

#include <wrtc/models/media_content.hpp>
#include <wrtc/interfaces/media/channel_manager.hpp>
#include <wrtc/interfaces/media/local_video_adapter.hpp>

namespace wrtc {
    class OutgoingVideoChannel {
        uint32_t _ssrc = 0;
        std::unique_ptr<webrtc::BaseChannel> channel;
        SafeThread& workerThread;
        SafeThread& networkThread;
        std::unique_ptr<webrtc::VideoBitrateAllocatorFactory> bitrateAllocatorFactory;
        LocalVideoAdapter* sink;

    public:
        OutgoingVideoChannel(
            webrtc::Call* call,
            ChannelManager* channelManager,
            webrtc::RtpTransport* rtpTransport,
            const MediaContent& mediaContent,
            SafeThread& workerThread,
            SafeThread& networkThread,
            LocalVideoAdapter* sink,
            // Retinal codec seam: ordered outgoing codec names (e.g. "AV1", "VP9", "H264").
            // Empty keeps the default preference (H.264 first).
            std::vector<std::string> codecPreferences = {}
        );

        ~OutgoingVideoChannel();

        void set_enabled(bool enable) const;

        [[nodiscard]] uint32_t ssrc() const;
    };
} // wrtc