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
            std::vector<std::string> codecPreferences = {},
            // Retinal max framerate seam: encodings[0].max_framerate, set before the
            // channel is enabled (so before the first encoder exists). 0 = WebRTC default.
            int maxFramerate = 0
        );

        ~OutgoingVideoChannel();

        void set_enabled(bool enable) const;

        [[nodiscard]] uint32_t ssrc() const;

        // Retinal stats seam: read-only; call on the worker thread.
        bool getStats(webrtc::VideoMediaSendInfo* info) const;

        // Retinal max bitrate seam: sets encodings[0].max_bitrate_bps (a ceiling for
        // WebRTC's own allocation); bps <= 0 clears it (WebRTC defaults). Worker thread.
        void setMaxBitrate(int bps) const;

        // Retinal max framerate seam: sets encodings[0].max_framerate; fps <= 0 clears it
        // (WebRTC default). WebRTC does not re-initialise a running encoder for it: the
        // next encoder it creates uses it. Worker thread.
        void setMaxFramerate(int fps) const;

    private:
        // Retinal max framerate seam; on the worker thread.
        void applyMaxFramerate(int fps) const;
    };
} // wrtc