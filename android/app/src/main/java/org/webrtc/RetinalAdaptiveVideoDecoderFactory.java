//
// Retinal adaptive decoder (D-029): hardware decoder factory whose decoders
// follow frame-size changes. See RetinalAdaptiveVideoDecoder.java.
//

package org.webrtc;

import android.media.MediaCodecInfo;
import android.media.MediaCodecInfo.CodecCapabilities;
import android.media.MediaCodecList;
import androidx.annotation.Nullable;

/**
 * Retinal adaptive decoder: WebRTC's hardware decoder factory, with the same
 * codec selection, whose decoders keep running across frame-size changes when
 * the codec supports adaptive playback. Codecs without it get WebRTC's stock
 * decoder.
 */
public class RetinalAdaptiveVideoDecoderFactory extends HardwareVideoDecoderFactory {
  private static final String TAG = "RetinalAdaptiveDecoderFactory";
  // Frames up to this size in either direction (1080p in portrait or landscape)
  // do not restart the decoder.
  private static final int MAX_DIMENSION = 1920;

  private final @Nullable EglBase.Context sharedContext;

  public RetinalAdaptiveVideoDecoderFactory(@Nullable EglBase.Context sharedContext) {
    super(sharedContext);
    this.sharedContext = sharedContext;
  }

  @Nullable
  @Override
  public VideoDecoder createDecoder(VideoCodecInfo codecType) {
    VideoDecoder stock = super.createDecoder(codecType);
    if (stock == null) {
      return null;
    }
    // The stock decoder is only constructed (never initialised); it names the
    // codec WebRTC selected.
    String codecName = stock.getImplementationName();
    VideoCodecMimeType type = VideoCodecMimeType.valueOf(codecType.getName());
    MediaCodecInfo info = findByName(codecName);
    if (info == null) {
      return stock;
    }
    CodecCapabilities capabilities = info.getCapabilitiesForType(type.mimeType());
    if (!capabilities.isFeatureSupported(CodecCapabilities.FEATURE_AdaptivePlayback)) {
      Logging.d(TAG, "[Retinal adaptive decoder] " + codecName + " has no adaptive playback");
      return stock;
    }
    Integer colorFormat =
        MediaCodecUtils.selectColorFormat(MediaCodecUtils.DECODER_COLOR_FORMATS, capabilities);
    if (colorFormat == null) {
      return stock;
    }
    MediaCodecInfo.VideoCapabilities video = capabilities.getVideoCapabilities();
    int maxWidth = MAX_DIMENSION;
    int maxHeight = MAX_DIMENSION;
    if (video != null) {
      maxWidth = Math.min(maxWidth, video.getSupportedWidths().getUpper());
      maxHeight = Math.min(maxHeight, video.getSupportedHeights().getUpper());
    }
    return new RetinalAdaptiveVideoDecoder(new MediaCodecWrapperFactoryImpl(), codecName, type,
        colorFormat, sharedContext, maxWidth, maxHeight);
  }

  private static @Nullable MediaCodecInfo findByName(String name) {
    for (MediaCodecInfo info : new MediaCodecList(MediaCodecList.ALL_CODECS).getCodecInfos()) {
      if (info != null && !info.isEncoder() && info.getName().equals(name)) {
        return info;
      }
    }
    return null;
  }
}
