//
// Retinal adaptive decoder (D-029): DefaultVideoDecoderFactory combination
// with the adaptive hardware factory. See RetinalAdaptiveVideoDecoder.java.
//

package org.webrtc;

import androidx.annotation.Nullable;
import java.util.Arrays;
import java.util.LinkedHashSet;

/**
 * Retinal adaptive decoder: WebRTC's DefaultVideoDecoderFactory(EglBase.Context)
 * combination (hardware with software and platform-software fallbacks), with
 * the hardware side replaced by {@link RetinalAdaptiveVideoDecoderFactory}.
 */
public class RetinalDefaultVideoDecoderFactory implements VideoDecoderFactory {
  private final VideoDecoderFactory hardwareVideoDecoderFactory;
  private final VideoDecoderFactory softwareVideoDecoderFactory = new SoftwareVideoDecoderFactory();
  private final VideoDecoderFactory platformSoftwareVideoDecoderFactory;

  public RetinalDefaultVideoDecoderFactory(@Nullable EglBase.Context eglContext) {
    this.hardwareVideoDecoderFactory = new RetinalAdaptiveVideoDecoderFactory(eglContext);
    this.platformSoftwareVideoDecoderFactory = new PlatformSoftwareVideoDecoderFactory(eglContext);
  }

  @Override
  public @Nullable VideoDecoder createDecoder(VideoCodecInfo codecType) {
    VideoDecoder softwareDecoder = softwareVideoDecoderFactory.createDecoder(codecType);
    final VideoDecoder hardwareDecoder = hardwareVideoDecoderFactory.createDecoder(codecType);
    if (softwareDecoder == null) {
      softwareDecoder = platformSoftwareVideoDecoderFactory.createDecoder(codecType);
    }
    if (hardwareDecoder != null && softwareDecoder != null) {
      return new VideoDecoderFallback(
          /* fallback= */ softwareDecoder, /* primary= */ hardwareDecoder);
    }
    return hardwareDecoder != null ? hardwareDecoder : softwareDecoder;
  }

  @Override
  public VideoCodecInfo[] getSupportedCodecs() {
    LinkedHashSet<VideoCodecInfo> supportedCodecInfos = new LinkedHashSet<VideoCodecInfo>();
    supportedCodecInfos.addAll(Arrays.asList(softwareVideoDecoderFactory.getSupportedCodecs()));
    supportedCodecInfos.addAll(Arrays.asList(hardwareVideoDecoderFactory.getSupportedCodecs()));
    supportedCodecInfos.addAll(
        Arrays.asList(platformSoftwareVideoDecoderFactory.getSupportedCodecs()));
    return supportedCodecInfos.toArray(new VideoCodecInfo[supportedCodecInfos.size()]);
  }
}
