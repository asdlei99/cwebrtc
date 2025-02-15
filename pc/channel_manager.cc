﻿/*
 *  Copyright 2004 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "pc/channel_manager.h"

#include <utility>

#include "absl/algorithm/container.h"
#include "absl/memory/memory.h"
#include "absl/strings/match.h"
#include "media/base/media_constants.h"
#include "rtc_base/checks.h"
#include "rtc_base/location.h"
#include "rtc_base/logging.h"
#include "rtc_base/thread_checker.h"
#include "rtc_base/trace_event.h"

namespace cricket {

ChannelManager::ChannelManager(
    std::unique_ptr<MediaEngineInterface> media_engine,
    std::unique_ptr<DataEngineInterface> data_engine,
    rtc::Thread* worker_thread,
    rtc::Thread* network_thread)
    : media_engine_(std::move(media_engine)),
      data_engine_(std::move(data_engine)),
      main_thread_(rtc::Thread::Current()),
      worker_thread_(worker_thread),
      network_thread_(network_thread) {
  RTC_DCHECK(data_engine_);
  RTC_DCHECK(worker_thread_);
  RTC_DCHECK(network_thread_);
}

ChannelManager::~ChannelManager() {
  if (initialized_) {
    Terminate();
  }
  // The media engine needs to be deleted on the worker thread for thread safe
  // destruction,
  worker_thread_->Invoke<void>(RTC_FROM_HERE, [&] { media_engine_.reset(); });
}

bool ChannelManager::SetVideoRtxEnabled(bool enable) 
{
  // To be safe, this call is only allowed before initialization. Apps like
  // Flute only have a singleton ChannelManager and we don't want this flag to
  // be toggled between calls or when there's concurrent calls. We expect apps
  // to enable this at startup and retain that setting for the lifetime of the
  // app.
  if (!initialized_) 
  {
    enable_rtx_ = enable;
    return true;
  }
  else 
  {
    RTC_LOG(LS_WARNING) << "Cannot toggle rtx after initialization!";
    return false;
  }
}

void ChannelManager::GetSupportedAudioSendCodecs(std::vector<AudioCodec>* codecs) const 
{
  if (!media_engine_) 
  {
    return;
  }
  *codecs = media_engine_->voice().send_codecs();
}

void ChannelManager::GetSupportedAudioReceiveCodecs( std::vector<AudioCodec>* codecs) const 
{
  if (!media_engine_) 
  {
    return;
  }
  *codecs = media_engine_->voice().recv_codecs();
}

void ChannelManager::GetSupportedAudioRtpHeaderExtensions(RtpHeaderExtensions* ext) const 
{
  if (!media_engine_) 
  {
    return;
  }
  *ext = media_engine_->voice().GetCapabilities().header_extensions;
}

void ChannelManager::GetSupportedVideoCodecs( std::vector<VideoCodec>* codecs) const 
{
  if (!media_engine_) 
  {
    return;
  }
  codecs->clear();
  // TODO@chensong 2022-09-27 获取视频的编解码器的信息
  std::vector<VideoCodec> video_codecs = media_engine_->video().codecs();
  for (const auto& video_codec : video_codecs) 
  {
    if (!enable_rtx_ && absl::EqualsIgnoreCase(kRtxCodecName, video_codec.name)) 
	{
      continue;
    }
    codecs->push_back(video_codec);
  }
}

void ChannelManager::GetSupportedVideoRtpHeaderExtensions( RtpHeaderExtensions* ext) const 
{
  if (!media_engine_) 
  {
    return;
  }
  *ext = media_engine_->video().GetCapabilities().header_extensions;
}

void ChannelManager::GetSupportedDataCodecs( std::vector<DataCodec>* codecs) const 
{
  *codecs = data_engine_->data_codecs();
}
/************************************************************************/
/*TODO@chensong 2022-10-06  工作线程执行的 媒体引擎的初始化的接口Init                                                               */
/************************************************************************/
bool ChannelManager::Init() 
{
  RTC_DCHECK(!initialized_);
  if (initialized_) 
  {
    return false;
  }
  RTC_DCHECK(network_thread_);
  RTC_DCHECK(worker_thread_);
  //TODO@chensong 20220321 正常情况工作线程与网络线程 所以是会设置线程
  if (!network_thread_->IsCurrent()) 
  {
    // Do not allow invoking calls to other threads on the network thread.
	 // 不允许调用网络线程上的其他线程。
    network_thread_->Invoke<void>(RTC_FROM_HERE, [&] { network_thread_->DisallowBlockingCalls(); });
  }
  // 媒体信息初始化 非常重要的哈 ^_^
  if (media_engine_)
  {
    initialized_ = worker_thread_->Invoke<bool>(RTC_FROM_HERE, [&] { return media_engine_->Init(); });
    RTC_DCHECK(initialized_);
  } 
  else 
  {
    initialized_ = true;
  }
  return initialized_;
}

void ChannelManager::Terminate() 
{
  RTC_DCHECK(initialized_);
  if (!initialized_) {
    return;
  }
  // Need to destroy the channels on the worker thread.
  worker_thread_->Invoke<void>(RTC_FROM_HERE, [&] {
    video_channels_.clear();
    voice_channels_.clear();
    data_channels_.clear();
  });
  initialized_ = false;
}

VoiceChannel* ChannelManager::CreateVoiceChannel(
    webrtc::Call* call,
    const cricket::MediaConfig& media_config,
    webrtc::RtpTransportInternal* rtp_transport,
    webrtc::MediaTransportInterface* media_transport,
    rtc::Thread* signaling_thread,
    const std::string& content_name,
    bool srtp_required,
    const webrtc::CryptoOptions& crypto_options,
    rtc::UniqueRandomIdGenerator* ssrc_generator,
    const AudioOptions& options)
{
  if (!worker_thread_->IsCurrent()) 
  {
    return worker_thread_->Invoke<VoiceChannel*>(RTC_FROM_HERE, [&] {
      return CreateVoiceChannel(
          call, media_config, rtp_transport, media_transport, signaling_thread,
          content_name, srtp_required, crypto_options, ssrc_generator, options);
    });
  }

  RTC_DCHECK_RUN_ON(worker_thread_);
  RTC_DCHECK(initialized_);
  RTC_DCHECK(call);
  if (!media_engine_) 
  {
    return nullptr;
  }

  VoiceMediaChannel* media_channel = media_engine_->voice().CreateMediaChannel(
      call, media_config, options, crypto_options);
  if (!media_channel) 
  {
    return nullptr;
  }

  auto voice_channel = absl::make_unique<VoiceChannel>(
      worker_thread_, network_thread_, signaling_thread,
      absl::WrapUnique(media_channel), content_name, srtp_required,
      crypto_options, ssrc_generator);

  voice_channel->Init_w(rtp_transport, media_transport);

  VoiceChannel* voice_channel_ptr = voice_channel.get();
  voice_channels_.push_back(std::move(voice_channel));
  return voice_channel_ptr;
}

void ChannelManager::DestroyVoiceChannel(VoiceChannel* voice_channel) 
{
  TRACE_EVENT0("webrtc", "ChannelManager::DestroyVoiceChannel");
  if (!voice_channel) 
  {
    return;
  }
  if (!worker_thread_->IsCurrent()) 
  {
    worker_thread_->Invoke<void>(RTC_FROM_HERE,
                                 [&] { DestroyVoiceChannel(voice_channel); });
    return;
  }

  RTC_DCHECK(initialized_);

  auto it = absl::c_find_if(voice_channels_,
                            [&](const std::unique_ptr<VoiceChannel>& p) {
                              return p.get() == voice_channel;
                            });
  RTC_DCHECK(it != voice_channels_.end());
  if (it == voice_channels_.end()) 
  {
    return;
  }

  voice_channels_.erase(it);
}

VideoChannel* ChannelManager::CreateVideoChannel(
    webrtc::Call* call,
    const cricket::MediaConfig& media_config,
    webrtc::RtpTransportInternal* rtp_transport,
    webrtc::MediaTransportInterface* media_transport,
    rtc::Thread* signaling_thread,
    const std::string& content_name,
    bool srtp_required,
    const webrtc::CryptoOptions& crypto_options,
    rtc::UniqueRandomIdGenerator* ssrc_generator,
    const VideoOptions& options) 
{
  if (!worker_thread_->IsCurrent()) 
  {
    return worker_thread_->Invoke<VideoChannel*>(RTC_FROM_HERE, [&] {
      return CreateVideoChannel(
          call, media_config, rtp_transport, media_transport, signaling_thread,
          content_name, srtp_required, crypto_options, ssrc_generator, options);
    });
  }

  RTC_DCHECK_RUN_ON(worker_thread_);
  RTC_DCHECK(initialized_);
  RTC_DCHECK(call);
  if (!media_engine_)
  {
    return nullptr;
  }
  // TODO@chensong 2022-07-25 创建视频通道
  VideoMediaChannel* media_channel = media_engine_->video().CreateMediaChannel(
      call, media_config, options, crypto_options);
  if (!media_channel) {
    return nullptr;
  }

  auto video_channel = absl::make_unique<VideoChannel>(
      worker_thread_, network_thread_, signaling_thread,
      absl::WrapUnique(media_channel), content_name, srtp_required,
      crypto_options, ssrc_generator);

  video_channel->Init_w(rtp_transport, media_transport);

  VideoChannel* video_channel_ptr = video_channel.get();
  video_channels_.push_back(std::move(video_channel));
  return video_channel_ptr;
}

void ChannelManager::DestroyVideoChannel(VideoChannel* video_channel) {
  TRACE_EVENT0("webrtc", "ChannelManager::DestroyVideoChannel");
  if (!video_channel) {
    return;
  }
  if (!worker_thread_->IsCurrent()) {
    worker_thread_->Invoke<void>(RTC_FROM_HERE,
                                 [&] { DestroyVideoChannel(video_channel); });
    return;
  }

  RTC_DCHECK(initialized_);

  auto it = absl::c_find_if(video_channels_,
                            [&](const std::unique_ptr<VideoChannel>& p) {
                              return p.get() == video_channel;
                            });
  RTC_DCHECK(it != video_channels_.end());
  if (it == video_channels_.end()) {
    return;
  }

  video_channels_.erase(it);
}

RtpDataChannel* ChannelManager::CreateRtpDataChannel(
    const cricket::MediaConfig& media_config,
    webrtc::RtpTransportInternal* rtp_transport,
    rtc::Thread* signaling_thread,
    const std::string& content_name,
    bool srtp_required,
    const webrtc::CryptoOptions& crypto_options,
    rtc::UniqueRandomIdGenerator* ssrc_generator) {
  if (!worker_thread_->IsCurrent()) {
    return worker_thread_->Invoke<RtpDataChannel*>(RTC_FROM_HERE, [&] {
      return CreateRtpDataChannel(media_config, rtp_transport, signaling_thread,
                                  content_name, srtp_required, crypto_options,
                                  ssrc_generator);
    });
  }

  // This is ok to alloc from a thread other than the worker thread.
  RTC_DCHECK(initialized_);
  DataMediaChannel* media_channel = data_engine_->CreateChannel(media_config);
  if (!media_channel) {
    RTC_LOG(LS_WARNING) << "Failed to create RTP data channel.";
    return nullptr;
  }

  auto data_channel = absl::make_unique<RtpDataChannel>(
      worker_thread_, network_thread_, signaling_thread,
      absl::WrapUnique(media_channel), content_name, srtp_required,
      crypto_options, ssrc_generator);
  data_channel->Init_w(rtp_transport);

  RtpDataChannel* data_channel_ptr = data_channel.get();
  data_channels_.push_back(std::move(data_channel));
  return data_channel_ptr;
}

void ChannelManager::DestroyRtpDataChannel(RtpDataChannel* data_channel) {
  TRACE_EVENT0("webrtc", "ChannelManager::DestroyRtpDataChannel");
  if (!data_channel) {
    return;
  }
  if (!worker_thread_->IsCurrent()) {
    worker_thread_->Invoke<void>(
        RTC_FROM_HERE, [&] { return DestroyRtpDataChannel(data_channel); });
    return;
  }

  RTC_DCHECK(initialized_);

  auto it = absl::c_find_if(data_channels_,
                            [&](const std::unique_ptr<RtpDataChannel>& p) {
                              return p.get() == data_channel;
                            });
  RTC_DCHECK(it != data_channels_.end());
  if (it == data_channels_.end()) {
    return;
  }

  data_channels_.erase(it);
}

bool ChannelManager::StartAecDump(rtc::PlatformFile file,
                                  int64_t max_size_bytes) {
  return worker_thread_->Invoke<bool>(RTC_FROM_HERE, [&] {
    return media_engine_->voice().StartAecDump(file, max_size_bytes);
  });
}

void ChannelManager::StopAecDump() {
  worker_thread_->Invoke<void>(RTC_FROM_HERE,
                               [&] { media_engine_->voice().StopAecDump(); });
}

}  // namespace cricket
