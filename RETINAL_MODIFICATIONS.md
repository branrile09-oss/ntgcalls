# Retinal modifications to ntgcalls

This is a **modified version** of [ntgcalls](https://github.com/pytgcalls/ntgcalls).
It is not an official ntgcalls release.

| | |
|---|---|
| Upstream base | ntgcalls v2.2.5, commit `1f4e4baadc77ce74753c037741c21ca95b1ca737` |
| Modified version | `2.2.5+retinal.1` (Android `BuildConfig.VERSION_NAME`; `version.retinal` in `version.properties`) |
| Modified by | Riley Branson, for the Retinal Android application |
| Date of modification | 2026-09-30 |
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

### 2. Version identification (2026-09-30)

- `version.properties`: `version.retinal=1`
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
