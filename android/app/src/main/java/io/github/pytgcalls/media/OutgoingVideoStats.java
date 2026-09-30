package io.github.pytgcalls.media;

/**
 * Retinal stats seam (Retinal modification of ntgcalls, LGPL-3.0, 2026-09-30):
 * a read-only snapshot of a P2P call's outgoing video, from WebRTC's call and
 * send-channel statistics. -1 means "not reported". Cumulative counters
 * (bytes, packets, frames, qpSum) grow for the life of the send stream.
 */
public class OutgoingVideoStats {
    /** WebRTC monotonic clock (ms) when sampled. */
    public final long timestampMs;
    /** Send-side bandwidth estimate. */
    public final long sendBandwidthBps;
    public final long pacerDelayMs;
    public final long rttMs;

    /** False until the outgoing video sender exists; the fields below are then defaults. */
    public final boolean hasSender;
    /** Negotiated send codec, e.g. "VP9", "H264"; null if not reported. */
    public final String codecName;
    public final int codecPayloadType;
    /** Encoder in use, e.g. a MediaCodec or libvpx name; null if not reported. */
    public final String encoderImplementation;
    /** -1 unknown, 0 no, 1 yes. */
    public final int powerEfficientEncoder;
    public final long targetBitrateBps;
    public final long mediaBitrateBps;
    public final long bytesSent;
    public final long retransmittedBytesSent;
    public final long packetsSent;
    public final long packetsLost;
    public final float fractionLost;
    public final long senderRttMs;
    public final int frameWidth;
    public final int frameHeight;
    public final double framerateInput;
    public final int framerateSent;
    /** "none", "cpu", "bandwidth" or "other". */
    public final String qualityLimitationReason;
    public final long qualityLimitationResolutionChanges;
    public final long qpSum;
    public final long framesEncoded;
    public final long keyFramesEncoded;
    public final long framesSent;
    public final long hugeFramesSent;
    public final long totalEncodedBytesTarget;
    public final int avgEncodeMs;
    public final int encodeUsagePercent;
    public final long nacksReceived;
    public final long firsReceived;
    public final long plisReceived;

    public OutgoingVideoStats(
        long timestampMs, long sendBandwidthBps, long pacerDelayMs, long rttMs,
        boolean hasSender, String codecName, int codecPayloadType,
        String encoderImplementation, int powerEfficientEncoder,
        long targetBitrateBps, long mediaBitrateBps, long bytesSent, long retransmittedBytesSent,
        long packetsSent, long packetsLost, float fractionLost, long senderRttMs,
        int frameWidth, int frameHeight, double framerateInput, int framerateSent,
        String qualityLimitationReason, long qualityLimitationResolutionChanges,
        long qpSum, long framesEncoded, long keyFramesEncoded, long framesSent,
        long hugeFramesSent, long totalEncodedBytesTarget, int avgEncodeMs, int encodeUsagePercent,
        long nacksReceived, long firsReceived, long plisReceived
    ) {
        this.timestampMs = timestampMs;
        this.sendBandwidthBps = sendBandwidthBps;
        this.pacerDelayMs = pacerDelayMs;
        this.rttMs = rttMs;
        this.hasSender = hasSender;
        this.codecName = codecName;
        this.codecPayloadType = codecPayloadType;
        this.encoderImplementation = encoderImplementation;
        this.powerEfficientEncoder = powerEfficientEncoder;
        this.targetBitrateBps = targetBitrateBps;
        this.mediaBitrateBps = mediaBitrateBps;
        this.bytesSent = bytesSent;
        this.retransmittedBytesSent = retransmittedBytesSent;
        this.packetsSent = packetsSent;
        this.packetsLost = packetsLost;
        this.fractionLost = fractionLost;
        this.senderRttMs = senderRttMs;
        this.frameWidth = frameWidth;
        this.frameHeight = frameHeight;
        this.framerateInput = framerateInput;
        this.framerateSent = framerateSent;
        this.qualityLimitationReason = qualityLimitationReason;
        this.qualityLimitationResolutionChanges = qualityLimitationResolutionChanges;
        this.qpSum = qpSum;
        this.framesEncoded = framesEncoded;
        this.keyFramesEncoded = keyFramesEncoded;
        this.framesSent = framesSent;
        this.hugeFramesSent = hugeFramesSent;
        this.totalEncodedBytesTarget = totalEncodedBytesTarget;
        this.avgEncodeMs = avgEncodeMs;
        this.encodeUsagePercent = encodeUsagePercent;
        this.nacksReceived = nacksReceived;
        this.firsReceived = firsReceived;
        this.plisReceived = plisReceived;
    }
}
