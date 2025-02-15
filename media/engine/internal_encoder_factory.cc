﻿/*
 *  Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "media/engine/internal_encoder_factory.h"

#include <string>

#include "absl/strings/match.h"
#include "api/video_codecs/sdp_video_format.h"
#include "media/base/codec.h"
#include "media/base/media_constants.h"
#include "modules/video_coding/codecs/h264/include/h264.h"
#include "modules/video_coding/codecs/vp8/include/vp8.h"
#include "modules/video_coding/codecs/vp9/include/vp9.h"
#include "rtc_base/logging.h"

namespace webrtc {

std::vector<SdpVideoFormat> InternalEncoderFactory::GetSupportedFormats() const 
{
  // TODO@chensong 20220928 支持视频编解码器 VP8、VP9、H264
  std::vector<SdpVideoFormat> supported_codecs;
  for (const webrtc::SdpVideoFormat& format : webrtc::SupportedH264Codecs()) 
  {
    // cricket::VideoCodec codec(kH264CodecName);
    //// TODO(magjed): Move setting these parameters into webrtc::H264Encoder
    //// instead.
    // codec.SetParam(kH264FmtpProfileLevelId,
    // kH264ProfileLevelConstrainedBaseline);
    // codec.SetParam(kH264FmtpLevelAsymmetryAllowed, "1");

    supported_codecs.push_back(format);
  }
  supported_codecs.push_back(SdpVideoFormat(cricket::kVp8CodecName));
  for (const webrtc::SdpVideoFormat& format : webrtc::SupportedVP9Codecs())
  {
    supported_codecs.push_back(format);
  }

  return supported_codecs;
}

VideoEncoderFactory::CodecInfo InternalEncoderFactory::QueryVideoEncoder( const SdpVideoFormat& format) const 
{
  CodecInfo info;
  info.is_hardware_accelerated = false;
  info.has_internal_source = false;
  return info;
}
// TODO@chensong 20211020   根据类型 创建VP8、VP9、H264编码器
std::unique_ptr<VideoEncoder> InternalEncoderFactory::CreateVideoEncoder( const SdpVideoFormat& format) 
{
  if (absl::EqualsIgnoreCase(format.name, cricket::kVp8CodecName)) {
    return VP8Encoder::Create();
  }
  if (absl::EqualsIgnoreCase(format.name, cricket::kVp9CodecName)) 
  {
    return VP9Encoder::Create(cricket::VideoCodec(format));
  }
  if (absl::EqualsIgnoreCase(format.name, cricket::kH264CodecName))
  {
    return H264Encoder::Create(cricket::VideoCodec(format));
  }
  RTC_LOG(LS_ERROR) << "Trying to created encoder of unsupported format "
                    << format.name;
  return nullptr;
}

}  // namespace webrtc
