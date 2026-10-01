# Retinal modifications to ntgcalls

This is a **modified version** of [ntgcalls](https://github.com/pytgcalls/ntgcalls).
It is not an official ntgcalls release.

| | |
|---|---|
| Upstream base | ntgcalls v2.2.5, commit `1f4e4baadc77ce74753c037741c21ca95b1ca737` |
| Modified version | `2.2.5+retinal.4` (Android `BuildConfig.VERSION_NAME`; `version.retinal` in `version.properties`) |
| Modified by | Riley Branson, for the Retinal Android application |
| Date of modification | 2026-09-30 (`+retinal.1`: codec preference; `+retinal.2`: outgoing video statistics; `+retinal.3`: opt-in encoder factory without shared EGL context); 2026-10-01 (`+retinal.4`: outgoing video maximum bitrate) |
| Licence | GNU LGPL-3.0, the same as upstream (see `LICENSE`). The modifications are licensed under LGPL-3.0. |

## What was changed

### 1. Generic outgoing video codec preference (2026-09-30)

Upstream always prefers H.264 as the outgoing video codec of a 1:1 (P2P) call
(`OutgoingVideoChannel`'s hard-coded `{H264}` preference). This modification lets
the application supply an **ordered list of codec names** for the outgoing
video stream of a P2P call. The first codec in the list that the peer accepted
during negotiation becomes the send codec.

- The list only reorders codecs the peer already accepted. No codec is added
  to, or removed from, the offer or answer, and Telegram signalling is unchanged.
- An empty list (the default) keeps upstream behaviour exactly: H.264 first.
- No codec policy is hard-coded here. The calling application decides the list.
- When a list is supplied, one log line records the requested order and the
  resulting negotiated order.

Files:

- `android/app/src/main/java/io/github/pytgcalls/NTgCalls.java`: `setOutgoingVideoCodecPreferences(long chatId, List<String> codecs)`
- `android/app/src/main/jni/ntgcalls.cpp`: JNI entry point
- `ntgcalls/include/ntgcalls/ntgcalls.hpp`, `ntgcalls/src/ntgcalls.cpp`: `NTgCalls::setOutgoingVideoCodecPreferences`
- `ntgcalls/include/ntgcalls/instances/p2p_call.hpp`, `ntgcalls/src/instances/p2p_call.cpp`: store and pass to the connection
- `wrtc/include/wrtc/interfaces/native_connection.hpp`, `wrtc/src/interfaces/native_connection.cpp`: store and pass to the outgoing video channel
- `wrtc/include/wrtc/interfaces/media/channels/outgoing_video_channel.hpp`, `wrtc/src/interfaces/media/channels/outgoing_video_channel.cpp`: apply the preference

Changed hunks are marked `Retinal codec seam`.

### 2. Read-only outgoing video statistics (2026-09-30, `+retinal.2`)

Upstream has no way for the application to read a P2P call's outgoing video
statistics. This modification adds one read-only query,
`NTgCalls.getOutgoingVideoStats(chatId)`, returning an `OutgoingVideoStats`
snapshot (or null before the call's media exists). The values are copied from
statistics WebRTC already keeps:

- `webrtc::Call::GetStats()`: send-side bandwidth estimate, pacer delay, RTT;
- the outgoing video channel's `VideoMediaSendInfo` (first sender): negotiated
  send codec and payload type, encoder implementation name, target and media
  bitrate, bytes/packets sent and retransmitted, packets lost, fraction lost,
  RTT, sent frame size and frame rates, quality-limitation reason, QP sum,
  frame and key-frame counts, huge frames, encode time/usage, NACK/FIR/PLI
  counts.

It is observational only: the snapshot is taken on WebRTC's worker thread and
nothing is written back. Congestion control, the degradation preference,
encoder behaviour, negotiation and Telegram signalling are unchanged, and no
policy is added to ntgcalls. The calling application decides what, if
anything, to do with the values.

Files:

- `android/app/src/main/java/io/github/pytgcalls/NTgCalls.java`: `getOutgoingVideoStats(long chatId)`
- `android/app/src/main/java/io/github/pytgcalls/media/OutgoingVideoStats.java` (new): the snapshot class
- `android/app/src/main/jni/ntgcalls.cpp`, `android/app/src/main/jni/utils.hpp`, `android/app/src/main/jni/utils.cpp`: JNI entry point and conversion
- `ntgcalls/include/ntgcalls/ntgcalls.hpp`, `ntgcalls/src/ntgcalls.cpp`: `NTgCalls::getOutgoingVideoStats`
- `ntgcalls/include/ntgcalls/instances/p2p_call.hpp`, `ntgcalls/src/instances/p2p_call.cpp`: forward to the native connection
- `wrtc/include/wrtc/models/outgoing_video_stats.hpp` (new): the snapshot struct
- `wrtc/include/wrtc/interfaces/native_network_interface.hpp`, `wrtc/src/interfaces/native_network_interface.cpp`: take the snapshot on the worker thread
- `wrtc/include/wrtc/interfaces/media/channels/outgoing_video_channel.hpp`, `wrtc/src/interfaces/media/channels/outgoing_video_channel.cpp`: read the send channel's statistics

Changed hunks are marked `Retinal stats seam`.

### 3. Opt-in hardware video encoder factory without shared EGL context (2026-09-30, `+retinal.3`)

Upstream always creates the Android hardware video encoder factory with the
shared EGL context, so every hardware encoder first starts in surface
(texture) mode. An application that only sends I420 frames through
`sendExternalFrame` then gets that encoder released and re-created in
byte-buffer mode on the first frame, after every encoder (re)initialization,
for example on each frame-size change.

`NTgCalls.setVideoEncoderSharedEglContext(false)` lets such an application
create the encoder factory without the shared EGL context, so encoders start
in byte-buffer mode directly.

- Opt-in. The default (`true`) is upstream behaviour, and nothing changes
  unless the application calls it.
- Process-wide, because the encoder factory is created once per process. It
  applies only if called before the first call creates the factory, and
  returns whether it was applied.
- The decoder factory, codec negotiation, bandwidth estimation, the
  degradation preference and frame geometry are unchanged.

Files:

- `android/app/src/main/java/io/github/pytgcalls/NTgCalls.java`: `setVideoEncoderSharedEglContext(boolean enabled)`
- `android/app/src/main/jni/ntgcalls.cpp`: JNI entry point
- `ntgcalls/include/ntgcalls/ntgcalls.hpp`, `ntgcalls/src/ntgcalls.cpp`: `NTgCalls::setVideoEncoderSharedEglContext`
- `wrtc/include/wrtc/video_factory/hardware/android/video_factory.hpp`, `wrtc/src/video_factory/hardware/android/video_factory.cpp`: the setting, applied when the encoder factory is created

Changed hunks are marked `Retinal encoder config`.

### 4. Outgoing video maximum bitrate (2026-10-01, `+retinal.4`)

Upstream sets no maximum for the outgoing video of a P2P call, so WebRTC
applies its resolution-based default ceiling (2.5 Mbps above 960x540, less
below). `NTgCalls.setOutgoingVideoMaxBitrate(chatId, bps)` lets the
application set `encodings[0].max_bitrate_bps` of the outgoing video sender
through `SetRtpSendParameters` (on WebRTC's worker thread). `bps <= 0` clears
it, so WebRTC's defaults apply again.

- It is a ceiling only. WebRTC's bandwidth estimation and bitrate allocation
  still choose the actual bitrate below it.
- Applied immediately during a call, and remembered for a video channel
  created later.
- Without a call to it, behaviour is upstream's.
- Codec negotiation, the degradation preference and the encoder factory are
  unchanged.

Files:

- `android/app/src/main/java/io/github/pytgcalls/NTgCalls.java`: `setOutgoingVideoMaxBitrate(long chatId, int bps)`
- `android/app/src/main/jni/ntgcalls.cpp`: JNI entry point
- `ntgcalls/include/ntgcalls/ntgcalls.hpp`, `ntgcalls/src/ntgcalls.cpp`: `NTgCalls::setOutgoingVideoMaxBitrate`
- `ntgcalls/include/ntgcalls/instances/p2p_call.hpp`, `ntgcalls/src/instances/p2p_call.cpp`: forward to the native connection
- `wrtc/include/wrtc/interfaces/native_network_interface.hpp`, `wrtc/src/interfaces/native_network_interface.cpp`: remember the value and apply it on the worker thread
- `wrtc/src/interfaces/native_connection.cpp`: apply a remembered value when the video channel is created
- `wrtc/include/wrtc/interfaces/media/channels/outgoing_video_channel.hpp`, `wrtc/src/interfaces/media/channels/outgoing_video_channel.cpp`: set `encodings[0].max_bitrate_bps`

Changed hunks are marked `Retinal max bitrate seam`.

### 5. Version identification

- `version.properties`: `version.retinal=4` (`1`, `2`, `3` for `2.2.5+retinal.1`, `.2`, `.3`)
- `android/app/build.gradle`: the Android version name and publication version
  carry the `+retinal.<n>` suffix. (`VERSION_CODE` is unchanged because upstream's
  version-code scheme only understands `-alpha`/`-beta`/`-rc` suffixes.)

## Building

The build is identical to upstream's Android build (`python3 setup.py build_lib --android`,
then `./gradlew assembleRelease` in `android/`), using upstream's pinned
dependencies. They were verified for this build as: WebRTC prebuilt m149.7827.3.0, Boost 1.91.0,
FFmpeg 8.1.1, NDK r28b, libc++ `af4386908c3762433d412689038de6e6333f5921`,
libc++abi `8f11bb1d4438d0239d0dfc1bd9456a9f31629dda`, buildtools
`8a1303aafa0a2efef79976043ea7dfb448a9fb9f`, Chromium clang
`llvmorg-22-init-20115-g2a8be8bd-1`.

Note: upstream's `cmake/FindClang.cmake` downloads Chromium's clang `update.py`
from the unpinned `main` branch and selects the newest clang-22 package at build
time, so the compiler revision must be recorded with each build.
