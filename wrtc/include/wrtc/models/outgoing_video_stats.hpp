//
// Retinal stats seam (Retinal modification of ntgcalls, LGPL-3.0, 2026-09-30).
//

#pragma once
#include <cstdint>
#include <string>

namespace wrtc {

    // Read-only snapshot of the outgoing video of a P2P call, copied from
    // webrtc::Call::GetStats() and the outgoing video channel's
    // VideoMediaSendInfo. Nothing here changes how the call behaves.
    // -1 means "not reported".
    struct OutgoingVideoStats {
        int64_t timestampMs = 0; // webrtc::TimeMillis() when sampled

        // webrtc::Call::Stats
        int64_t sendBandwidthBps = 0; // send-side bandwidth estimate
        int64_t pacerDelayMs = 0;
        int64_t rttMs = -1;

        // First (only, in P2P) outgoing video sender; the fields below keep
        // their defaults when hasSender is false.
        bool hasSender = false;
        std::string codecName;
        int codecPayloadType = -1;
        std::string encoderImplementation;
        int powerEfficientEncoder = -1; // -1 unknown, 0 no, 1 yes
        int64_t targetBitrateBps = -1;
        int64_t mediaBitrateBps = 0; // VideoSenderInfo::nominal_bitrate
        int64_t bytesSent = 0; // payload + header + padding
        int64_t retransmittedBytesSent = 0;
        int64_t packetsSent = 0;
        int64_t packetsLost = 0;
        float fractionLost = 0;
        int64_t senderRttMs = -1;
        int frameWidth = 0;
        int frameHeight = 0;
        double framerateInput = 0;
        int framerateSent = 0;
        std::string qualityLimitationReason; // none | cpu | bandwidth | other
        int64_t qualityLimitationResolutionChanges = 0;
        int64_t qpSum = -1;
        int64_t framesEncoded = 0;
        int64_t keyFramesEncoded = 0;
        int64_t framesSent = 0;
        int64_t hugeFramesSent = 0;
        int64_t totalEncodedBytesTarget = 0;
        int avgEncodeMs = 0;
        int encodeUsagePercent = 0;
        int64_t nacksReceived = 0;
        int64_t firsReceived = 0;
        int64_t plisReceived = 0;
    };

} // wrtc
