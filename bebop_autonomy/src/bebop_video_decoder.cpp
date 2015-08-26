#include "bebop_autonomy/bebop_video_decoder.h"
#include <stdexcept>
#include <boost/lexical_cast.hpp>
#include <boost/thread/lock_guard.hpp>
#include <algorithm>

namespace bebop_autonomy
{

const char* VideoDecoder::LOG_TAG = "DEC";

// TODO: Move to util
void VideoDecoder::ThrowOnCondition(const bool cond, const std::string &message)
{
  if (!cond) return;
  throw std::runtime_error(message);
}

VideoDecoder::VideoDecoder()
  : codec_initialized_(false),
    first_iframe_recv_(false),
    format_ctx_ptr_(NULL),
    codec_ctx_ptr_(NULL),
    codec_ptr_(NULL),
    frame_ptr_(NULL),
    frame_rgb_ptr_(NULL),
    img_convert_ctx_ptr_(NULL),
    input_format_ptr_(NULL),
    frame_rgb_raw_ptr_(NULL)
{
  ;
}

bool VideoDecoder::InitCodec(const uint32_t width, const uint32_t height)
{
  if (codec_initialized_)
  {
    // TODO: Maybe re-initialize
    return true;
  }

  try
  {
    ThrowOnCondition(width == 0 || height == 0, std::string("Invalid frame size:") +
                     boost::lexical_cast<std::string>(width) + " x " + boost::lexical_cast<std::string>(height));

    // Very first init
    avcodec_register_all();
    av_register_all();
    av_log_set_level(AV_LOG_QUIET);

    codec_ptr_ = avcodec_find_decoder(CODEC_ID_H264);
    ThrowOnCondition(codec_ptr_ == NULL, "Codec H264 not found!");


    codec_ctx_ptr_ = avcodec_alloc_context3(codec_ptr_);
    codec_ctx_ptr_->pix_fmt = AV_PIX_FMT_YUV420P;
    codec_ctx_ptr_->skip_frame = AVDISCARD_DEFAULT;
    codec_ctx_ptr_->error_concealment = FF_EC_GUESS_MVS | FF_EC_DEBLOCK;
    codec_ctx_ptr_->skip_loop_filter = AVDISCARD_DEFAULT;
    codec_ctx_ptr_->workaround_bugs = AVMEDIA_TYPE_VIDEO;
    codec_ctx_ptr_->codec_id = AV_CODEC_ID_H264;
    codec_ctx_ptr_->skip_idct = AVDISCARD_DEFAULT;
    codec_ctx_ptr_->width = width;
    codec_ctx_ptr_->height = height;

    ThrowOnCondition(
          avcodec_open2(codec_ctx_ptr_, codec_ptr_, NULL) < 0,
          "Can not open the decoder!");



    const uint32_t num_bytes = avpicture_get_size(PIX_FMT_RGB24, codec_ctx_ptr_->width, codec_ctx_ptr_->height);
    {
       frame_ptr_ = avcodec_alloc_frame();
       frame_rgb_ptr_ = avcodec_alloc_frame();

       ThrowOnCondition(!frame_ptr_ || !frame_rgb_ptr_, "Can not allocate memory for frames!");

       frame_rgb_raw_ptr_ = (uint8_t*) av_malloc(num_bytes * sizeof(uint8_t));
       ThrowOnCondition(frame_rgb_raw_ptr_ == NULL,
                        std::string("Can not allocate memory for the buffer: ") +
                        boost::lexical_cast<std::string>(num_bytes));
       ThrowOnCondition(0 == avpicture_fill(
                          (AVPicture*) frame_rgb_ptr_, frame_rgb_raw_ptr_, PIX_FMT_RGB24,
                          codec_ctx_ptr_->width, codec_ctx_ptr_->height),
                        "Failed to initialize the picture data structure.");
    }

    av_init_packet(&packet_);
  }
  catch (const std::runtime_error& e)
  {
    ARSAL_PRINT(ARSAL_PRINT_ERROR, LOG_TAG, "%s", e.what());
    Cleanup();
    return false;
  }

  codec_initialized_ = true;
  first_iframe_recv_ = false;
  ARSAL_PRINT(ARSAL_PRINT_INFO, LOG_TAG, "H264 Codec is initialized!");
  return true;
}

void VideoDecoder::Cleanup()
{
  if (codec_ctx_ptr_)
  {
    avcodec_close(codec_ctx_ptr_);
  }

  if (frame_ptr_)
  {
    av_free(frame_ptr_);
  }

  {
    boost::lock_guard<boost::mutex> lock(frame_rgb_mutex_);
    if (frame_rgb_ptr_)
    {
      av_free(frame_rgb_ptr_);
    }

    if (frame_rgb_raw_ptr_)
    {
      av_free(frame_rgb_raw_ptr_);
    }
  }


  if (img_convert_ctx_ptr_)
  {
    sws_freeContext(img_convert_ctx_ptr_);
  }

  codec_initialized_ = false;
  first_iframe_recv_ = false;
  ARSAL_PRINT(ARSAL_PRINT_INFO, LOG_TAG, "Cleaned up!");
}

VideoDecoder::~VideoDecoder()
{
  Cleanup();
  ARSAL_PRINT(ARSAL_PRINT_INFO, LOG_TAG, "Dstr!");
}

void VideoDecoder::ConvertFrameToRGB()
{
  if (!img_convert_ctx_ptr_)
  {
    img_convert_ctx_ptr_ = sws_getContext(codec_ctx_ptr_->width, codec_ctx_ptr_->height, codec_ctx_ptr_->pix_fmt,
                                          codec_ctx_ptr_->width, codec_ctx_ptr_->height, PIX_FMT_RGB24,
                                          SWS_FAST_BILINEAR, NULL, NULL, NULL);
  }

  {
    boost::lock_guard<boost::mutex> lock(frame_rgb_mutex_);
    sws_scale(img_convert_ctx_ptr_, frame_ptr_->data, frame_ptr_->linesize, 0,
              codec_ctx_ptr_->height, frame_rgb_ptr_->data, frame_rgb_ptr_->linesize);
  }

}

bool VideoDecoder::Decode(const ARCONTROLLER_Frame_t *bebop_frame_ptr_)
{
  if (!codec_initialized_)
  {
    if (!InitCodec(bebop_frame_ptr_->width, bebop_frame_ptr_->height))
    {
      return false;
    }
  }

  // Wait for first IFrame
  if (!first_iframe_recv_)
  {
    if (bebop_frame_ptr_->isIFrame)
    {
      first_iframe_recv_ = true;
    }
    else
    {
      ARSAL_PRINT(ARSAL_PRINT_INFO, LOG_TAG, "Waiting for the first IFrame.");
      return false;
    }
  }

  if (!bebop_frame_ptr_->data || !bebop_frame_ptr_->used)
  {
    ARSAL_PRINT(ARSAL_PRINT_ERROR, LOG_TAG, "Invalid frame data. Skipping.");
    return false;
  }

  packet_.data = bebop_frame_ptr_->data;
  packet_.size = bebop_frame_ptr_->used;

  int32_t frame_finished = 0;
  while (packet_.size > 0)
  {
    const int32_t len = avcodec_decode_video2(codec_ctx_ptr_, frame_ptr_, &frame_finished, &packet_);

    if (len > 0)
    {
      if (frame_finished)
      {
//        ARSAL_PRINT(ARSAL_PRINT_INFO, LOG_TAG, "Res: %d, Frame Finished: %d, isIFrame: %d", len, frame_finished, bebop_frame_ptr_->isIFrame);

        ConvertFrameToRGB();
      }

      if (packet_.data)
      {
        packet_.size -= len;
        packet_.data += len;
      }
    }
  }
  //ARSAL_PRINT(ARSAL_PRINT_INFO, LOG_TAG, "End of Decode");
  return true;
}

void VideoDecoder::CopyDecodedFrame(std::vector<uint8_t>& buffer) const
{
  boost::lock_guard<boost::mutex> lock(frame_rgb_mutex_);
  std::copy(frame_rgb_raw_ptr_,
            frame_rgb_raw_ptr_ + (GetFrameWidth() * GetFrameHeight() * 3),
            buffer.begin());
}
//const uint8_t* VideoDecoder::GetFrameRGBCstPtr() const
//{
//  ARSAL_PRINT(ARSAL_PRINT_ERROR, LOG_TAG, "LOCK");
//  boost::lock_guard<boost::mutex> lock(frame_rgb_mutex_);
//  ARSAL_PRINT(ARSAL_PRINT_ERROR, LOG_TAG, "UNLOCK");
//  return frame_rgb_raw_ptr_;
//}

}  // namespace bebop_autonomy
