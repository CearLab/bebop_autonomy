#ifndef BEBOP_VIDEO_DECODER_H
#define BEBOP_VIDEO_DECODER_H

extern "C"
{
  #include "libARSAL/ARSAL_Print.h"
  #include "libARController/ARCONTROLLER_Error.h"
  #include "libARController/ARCONTROLLER_Frame.h"
  #include <libavcodec/avcodec.h>
  #include <libavformat/avformat.h>
  #include <libavformat/avio.h>
  #include <libswscale/swscale.h>
}

#include <boost/thread/mutex.hpp>
#include <string>
#include <vector>

namespace bebop_autonomy
{

class VideoDecoder
{
private:
  static const char* LOG_TAG;

  bool codec_initialized_;
  bool first_iframe_recv_;
  AVFormatContext* format_ctx_ptr_;
  AVCodecContext* codec_ctx_ptr_;
  AVCodec* codec_ptr_;
  AVFrame* frame_ptr_;
  AVFrame* frame_rgb_ptr_;
  AVPacket packet_;
  SwsContext* img_convert_ctx_ptr_;
  AVInputFormat* input_format_ptr_;
  uint8_t *frame_rgb_raw_ptr_;

  mutable boost::mutex frame_rgb_mutex_;

  static void ThrowOnCondition(const bool cond, const std::string& message);
  bool InitCodec(const uint32_t width, const uint32_t height);
  void Cleanup();

  void ConvertFrameToRGB();

public:
  VideoDecoder();
  ~VideoDecoder();

  bool Decode(const ARCONTROLLER_Frame_t* bebop_frame_ptr_);
  inline uint32_t GetFrameWidth() const {return codec_initialized_ ? codec_ctx_ptr_->width : 0;}
  inline uint32_t GetFrameHeight() const {return codec_initialized_ ? codec_ctx_ptr_->height : 0;}

  // Thread-safe access to decoded frame as raw RGB24 data
  void CopyDecodedFrame(std::vector<uint8_t>& buffer) const;
//  const uint8_t* GetFrameRGBCstPtr() const;
};

}  // namespace bebop_autonomy

#endif  // BEBOP_VIDEO_DECODER_H
