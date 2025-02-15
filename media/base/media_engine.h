/*
 *  Copyright (c) 2004 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef MEDIA_BASE_MEDIA_ENGINE_H_
#define MEDIA_BASE_MEDIA_ENGINE_H_

#if defined(WEBRTC_MAC) && !defined(WEBRTC_IOS)
#include <CoreAudio/CoreAudio.h>
#endif

#include <memory>
#include <string>
#include <vector>

#include "api/audio_codecs/audio_decoder_factory.h"
#include "api/audio_codecs/audio_encoder_factory.h"
#include "api/crypto/crypto_options.h"
#include "api/rtp_parameters.h"
#include "call/audio_state.h"
#include "media/base/codec.h"
#include "media/base/media_channel.h"
#include "media/base/video_common.h"
#include "rtc_base/platform_file.h"

namespace webrtc {
class AudioDeviceModule;
class AudioMixer;
class AudioProcessing;
class Call;
}  // namespace webrtc

namespace cricket {

webrtc::RTCError CheckRtpParametersValues(
    const webrtc::RtpParameters& new_parameters);

webrtc::RTCError CheckRtpParametersInvalidModificationAndValues(
    const webrtc::RtpParameters& old_parameters,
    const webrtc::RtpParameters& new_parameters);

struct RtpCapabilities {
  RtpCapabilities();
  ~RtpCapabilities();
  std::vector<webrtc::RtpExtension> header_extensions;
};

class VoiceEngineInterface {
 public:
  VoiceEngineInterface() = default;
  virtual ~VoiceEngineInterface() = default;
  RTC_DISALLOW_COPY_AND_ASSIGN(VoiceEngineInterface);

  // Initialization
  // Starts the engine.
  virtual void Init() = 0;

  // TODO(solenberg): Remove once VoE API refactoring is done.
  virtual rtc::scoped_refptr<webrtc::AudioState> GetAudioState() const = 0;

  // MediaChannel creation
  // Creates a voice media channel. Returns NULL on failure.
  virtual VoiceMediaChannel* CreateMediaChannel(
      webrtc::Call* call,
      const MediaConfig& config,
      const AudioOptions& options,
      const webrtc::CryptoOptions& crypto_options) = 0;

  virtual const std::vector<AudioCodec>& send_codecs() const = 0;
  virtual const std::vector<AudioCodec>& recv_codecs() const = 0;
  virtual RtpCapabilities GetCapabilities() const = 0;

  // Starts AEC dump using existing file, a maximum file size in bytes can be
  // specified. Logging is stopped just before the size limit is exceeded.
  // If max_size_bytes is set to a value <= 0, no limit will be used.
  virtual bool StartAecDump(rtc::PlatformFile file, int64_t max_size_bytes) = 0;

  // Stops recording AEC dump.
  virtual void StopAecDump() = 0;
};

class VideoEngineInterface {
 public:
  VideoEngineInterface() = default;
  virtual ~VideoEngineInterface() = default;
  RTC_DISALLOW_COPY_AND_ASSIGN(VideoEngineInterface);

  // Creates a video media channel, paired with the specified voice channel.
  // Returns NULL on failure.
  virtual VideoMediaChannel* CreateMediaChannel(
      webrtc::Call* call,
      const MediaConfig& config,
      const VideoOptions& options,
      const webrtc::CryptoOptions& crypto_options) = 0;

  virtual std::vector<VideoCodec> codecs() const = 0;
  virtual RtpCapabilities GetCapabilities() const = 0;
};

// MediaEngineInterface is an abstraction of a media engine which can be
// subclassed to support different media componentry backends.
// It supports voice and video operations in the same class to facilitate
// proper synchronization between both media types.
class MediaEngineInterface {
 public:
  virtual ~MediaEngineInterface() {}

  // Initialization
  // Starts the engine.
  virtual bool Init() = 0;
  virtual VoiceEngineInterface& voice() = 0;
  virtual VideoEngineInterface& video() = 0;
  virtual const VoiceEngineInterface& voice() const = 0;
  virtual const VideoEngineInterface& video() const = 0;
};

// CompositeMediaEngine constructs a MediaEngine from separate
// voice and video engine classes.
class CompositeMediaEngine : public MediaEngineInterface {
 public:
  CompositeMediaEngine(std::unique_ptr<VoiceEngineInterface> audio_engine,
                       std::unique_ptr<VideoEngineInterface> video_engine);
  ~CompositeMediaEngine() override;
  bool Init() override;

  VoiceEngineInterface& voice() override;
  VideoEngineInterface& video() override;
  const VoiceEngineInterface& voice() const override;
  const VideoEngineInterface& video() const override;

 private:
  std::unique_ptr<VoiceEngineInterface> voice_engine_;
  std::unique_ptr<VideoEngineInterface> video_engine_; // WebRtcVideoEngine
};

enum DataChannelType {
  DCT_NONE = 0,
  DCT_RTP = 1,
  DCT_SCTP = 2,
  DCT_MEDIA_TRANSPORT = 3
};

class DataEngineInterface {
 public:
  virtual ~DataEngineInterface() {}
  virtual DataMediaChannel* CreateChannel(const MediaConfig& config) = 0;
  virtual const std::vector<DataCodec>& data_codecs() = 0;
};

webrtc::RtpParameters CreateRtpParametersWithOneEncoding();
webrtc::RtpParameters CreateRtpParametersWithEncodings(StreamParams sp);

}  // namespace cricket

#endif  // MEDIA_BASE_MEDIA_ENGINE_H_
