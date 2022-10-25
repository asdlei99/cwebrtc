/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/rtp_rtcp/source/rtp_sender.h"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

#include "absl/memory/memory.h"
#include "absl/strings/match.h"
#include "api/array_view.h"
#include "logging/rtc_event_log/events/rtc_event_rtp_packet_outgoing.h"
#include "logging/rtc_event_log/rtc_event_log.h"
#include "modules/rtp_rtcp/include/rtp_cvo.h"
#include "modules/rtp_rtcp/source/byte_io.h"
#include "modules/rtp_rtcp/source/rtp_generic_frame_descriptor_extension.h"
#include "modules/rtp_rtcp/source/rtp_header_extensions.h"
#include "modules/rtp_rtcp/source/rtp_packet_to_send.h"
#include "modules/rtp_rtcp/source/time_util.h"
#include "rtc_base/arraysize.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/numerics/safe_minmax.h"
#include "rtc_base/rate_limiter.h"
#include "rtc_base/time_utils.h"

namespace webrtc {

namespace {
// Max in the RFC 3550 is 255 bytes, we limit it to be modulus 32 for SRTP.
constexpr size_t kMaxPaddingLength = 224;
constexpr size_t kMinAudioPaddingLength = 50;
constexpr int kSendSideDelayWindowMs = 1000;
constexpr size_t kRtpHeaderLength = 12;
constexpr uint16_t kMaxInitRtpSeqNumber = 32767;  // 2^15 -1.
constexpr uint32_t kTimestampTicksPerMs = 90;
constexpr int kBitrateStatisticsWindowMs = 1000;

constexpr size_t kMinFlexfecPacketsToStoreForPacing = 50;

template <typename Extension>
constexpr RtpExtensionSize CreateExtensionSize() {
  return {Extension::kId, Extension::kValueSizeBytes};
}

template <typename Extension>
constexpr RtpExtensionSize CreateMaxExtensionSize() {
  return {Extension::kId, Extension::kMaxValueSizeBytes};
}

// Size info for header extensions that might be used in padding or FEC packets.
constexpr RtpExtensionSize kFecOrPaddingExtensionSizes[] = {
    CreateExtensionSize<AbsoluteSendTime>(),
    CreateExtensionSize<TransmissionOffset>(),
    CreateExtensionSize<TransportSequenceNumber>(),
    CreateExtensionSize<PlayoutDelayLimits>(),
    CreateMaxExtensionSize<RtpMid>(),
};

// Size info for header extensions that might be used in video packets.
constexpr RtpExtensionSize kVideoExtensionSizes[] = {
    CreateExtensionSize<AbsoluteSendTime>(),
    CreateExtensionSize<TransmissionOffset>(),
    CreateExtensionSize<TransportSequenceNumber>(),
    CreateExtensionSize<PlayoutDelayLimits>(),
    CreateExtensionSize<VideoOrientation>(),
    CreateExtensionSize<VideoContentTypeExtension>(),
    CreateExtensionSize<VideoTimingExtension>(),
    CreateMaxExtensionSize<RtpStreamId>(),
    CreateMaxExtensionSize<RepairedRtpStreamId>(),
    CreateMaxExtensionSize<RtpMid>(),
    {RtpGenericFrameDescriptorExtension00::kId,
     RtpGenericFrameDescriptorExtension00::kMaxSizeBytes},
    {RtpGenericFrameDescriptorExtension01::kId,
     RtpGenericFrameDescriptorExtension01::kMaxSizeBytes},
};

}  // namespace

RTPSender::RTPSender(
    bool audio,
    Clock* clock,
    Transport* transport,
    RtpPacketSender* paced_sender,
    absl::optional<uint32_t> flexfec_ssrc,
    TransportSequenceNumberAllocator* sequence_number_allocator,
    TransportFeedbackObserver* transport_feedback_observer,
    BitrateStatisticsObserver* bitrate_callback,
    SendSideDelayObserver* send_side_delay_observer,
    RtcEventLog* event_log,
    SendPacketObserver* send_packet_observer,
    RateLimiter* retransmission_rate_limiter,
    OverheadObserver* overhead_observer,
    bool populate_network2_timestamp,
    FrameEncryptorInterface* frame_encryptor,
    bool require_frame_encryption,
    bool extmap_allow_mixed,
    const WebRtcKeyValueConfig& field_trials)
    : clock_(clock),
      // TODO(holmer): Remove this conversion?
      clock_delta_ms_(clock_->TimeInMilliseconds() - rtc::TimeMillis()),
      random_(clock_->TimeInMicroseconds()),
      audio_configured_(audio),
      flexfec_ssrc_(flexfec_ssrc),
      paced_sender_(paced_sender),
      transport_sequence_number_allocator_(sequence_number_allocator),
      transport_feedback_observer_(transport_feedback_observer),
      transport_(transport),
      sending_media_(true),  // Default to sending media.
      force_part_of_allocation_(false),
      max_packet_size_(IP_PACKET_SIZE - 28),  // Default is IP-v4/UDP.
      last_payload_type_(-1),
      rtp_header_extension_map_(extmap_allow_mixed),
      packet_history_(clock),
      flexfec_packet_history_(clock),
      // Statistics
      send_delays_(),
      max_delay_it_(send_delays_.end()),
      sum_delays_ms_(0),
      rtp_stats_callback_(nullptr),
      total_bitrate_sent_(kBitrateStatisticsWindowMs,
                          RateStatistics::kBpsScale),
      nack_bitrate_sent_(kBitrateStatisticsWindowMs, RateStatistics::kBpsScale),
      send_side_delay_observer_(send_side_delay_observer),
      event_log_(event_log),
      send_packet_observer_(send_packet_observer),
      bitrate_callback_(bitrate_callback),
      // RTP variables
      sequence_number_forced_(false),
      last_rtp_timestamp_(0),
      capture_time_ms_(0),
      last_timestamp_time_ms_(0),
      media_has_been_sent_(false),
      last_packet_marker_bit_(false),
      csrcs_(),
      rtx_(kRtxOff),
      rtp_overhead_bytes_per_packet_(0),
      retransmission_rate_limiter_(retransmission_rate_limiter),
      overhead_observer_(overhead_observer),
      populate_network2_timestamp_(populate_network2_timestamp),
      send_side_bwe_with_overhead_(
          field_trials.Lookup("WebRTC-SendSideBwe-WithOverhead")
              .find("Enabled") == 0) {
  // This random initialization is not intended to be cryptographic strong.
  timestamp_offset_ = random_.Rand<uint32_t>();
  // Random start, 16 bits. Can't be 0.
  sequence_number_rtx_ = random_.Rand(1, kMaxInitRtpSeqNumber);
  sequence_number_ = random_.Rand(1, kMaxInitRtpSeqNumber);

  // Store FlexFEC packets in the packet history data structure, so they can
  // be found when paced.
  if (flexfec_ssrc_) {
    flexfec_packet_history_.SetStorePacketsStatus(
        RtpPacketHistory::StorageMode::kStore,
        kMinFlexfecPacketsToStoreForPacing);
  }
}

RTPSender::~RTPSender() {
  // TODO(tommi): Use a thread checker to ensure the object is created and
  // deleted on the same thread.  At the moment this isn't possible due to
  // voe::ChannelOwner in voice engine.  To reproduce, run:
  // voe_auto_test --automated --gtest_filter=*MixManyChannelsForStressOpus

  // TODO(tommi,holmer): We don't grab locks in the dtor before accessing member
  // variables but we grab them in all other methods. (what's the design?)
  // Start documenting what thread we're on in what method so that it's easier
  // to understand performance attributes and possibly remove locks.
}

rtc::ArrayView<const RtpExtensionSize> RTPSender::FecExtensionSizes() {
  return rtc::MakeArrayView(kFecOrPaddingExtensionSizes,
                            arraysize(kFecOrPaddingExtensionSizes));
}

rtc::ArrayView<const RtpExtensionSize> RTPSender::VideoExtensionSizes() {
  return rtc::MakeArrayView(kVideoExtensionSizes,
                            arraysize(kVideoExtensionSizes));
}

uint16_t RTPSender::ActualSendBitrateKbit() const {
  rtc::CritScope cs(&statistics_crit_);
  return static_cast<uint16_t>(
      total_bitrate_sent_.Rate(clock_->TimeInMilliseconds()).value_or(0) /
      1000);
}

uint32_t RTPSender::NackOverheadRate() const {
  rtc::CritScope cs(&statistics_crit_);
  return nack_bitrate_sent_.Rate(clock_->TimeInMilliseconds()).value_or(0);
}

void RTPSender::SetExtmapAllowMixed(bool extmap_allow_mixed) {
  rtc::CritScope lock(&send_critsect_);
  rtp_header_extension_map_.SetExtmapAllowMixed(extmap_allow_mixed);
}

int32_t RTPSender::RegisterRtpHeaderExtension(RTPExtensionType type,
                                              uint8_t id) {
  rtc::CritScope lock(&send_critsect_);
  return rtp_header_extension_map_.RegisterByType(id, type) ? 0 : -1;
}

bool RTPSender::RegisterRtpHeaderExtension(const std::string& uri, int id) {
  rtc::CritScope lock(&send_critsect_);
  return rtp_header_extension_map_.RegisterByUri(id, uri);
}

bool RTPSender::IsRtpHeaderExtensionRegistered(RTPExtensionType type) const {
  rtc::CritScope lock(&send_critsect_);
  return rtp_header_extension_map_.IsRegistered(type);
}

int32_t RTPSender::DeregisterRtpHeaderExtension(RTPExtensionType type) {
  rtc::CritScope lock(&send_critsect_);
  return rtp_header_extension_map_.Deregister(type);
}

void RTPSender::SetMaxRtpPacketSize(size_t max_packet_size) {
  RTC_DCHECK_GE(max_packet_size, 100);
  RTC_DCHECK_LE(max_packet_size, IP_PACKET_SIZE);
  rtc::CritScope lock(&send_critsect_);
  max_packet_size_ = max_packet_size;
}

size_t RTPSender::MaxRtpPacketSize() const {
  return max_packet_size_;
}

void RTPSender::SetRtxStatus(int mode) {
  rtc::CritScope lock(&send_critsect_);
  rtx_ = mode;
}

int RTPSender::RtxStatus() const {
  rtc::CritScope lock(&send_critsect_);
  return rtx_;
}

void RTPSender::SetRtxSsrc(uint32_t ssrc) {
  rtc::CritScope lock(&send_critsect_);
  ssrc_rtx_.emplace(ssrc);
}

uint32_t RTPSender::RtxSsrc() const {
  rtc::CritScope lock(&send_critsect_);
  RTC_DCHECK(ssrc_rtx_);
  return *ssrc_rtx_;
}

void RTPSender::SetRtxPayloadType(int payload_type,
                                  int associated_payload_type) {
  rtc::CritScope lock(&send_critsect_);
  RTC_DCHECK_LE(payload_type, 127);
  RTC_DCHECK_LE(associated_payload_type, 127);
  if (payload_type < 0) {
    RTC_LOG(LS_ERROR) << "Invalid RTX payload type: " << payload_type << ".";
    return;
  }

  rtx_payload_type_map_[associated_payload_type] = payload_type;
}

size_t RTPSender::TrySendRedundantPayloads(size_t bytes_to_send,
                                           const PacedPacketInfo& pacing_info) {
  {
    rtc::CritScope lock(&send_critsect_);
    if (!sending_media_)
      return 0;
    if ((rtx_ & kRtxRedundantPayloads) == 0)
      return 0;
  }

  int bytes_left = static_cast<int>(bytes_to_send);
  while (bytes_left > 0) {
    std::unique_ptr<RtpPacketToSend> packet =
        packet_history_.GetBestFittingPacket(bytes_left);
    if (!packet)
      break;
    size_t payload_size = packet->payload_size();
    if (!PrepareAndSendPacket(std::move(packet), true, false, pacing_info))
      break;
    bytes_left -= payload_size;
  }
  return bytes_to_send - bytes_left;
}

size_t RTPSender::SendPadData(size_t bytes,
                              const PacedPacketInfo& pacing_info) {
  size_t padding_bytes_in_packet;
  size_t max_payload_size = max_packet_size_ - RtpHeaderLength();

  if (audio_configured_) {
    // Allow smaller padding packets for audio.
    padding_bytes_in_packet = rtc::SafeClamp<size_t>(
        bytes, kMinAudioPaddingLength,
        rtc::SafeMin(max_payload_size, kMaxPaddingLength));
  } else {
    // Always send full padding packets. This is accounted for by the
    // RtpPacketSender, which will make sure we don't send too much padding even
    // if a single packet is larger than requested.
    // We do this to avoid frequently sending small packets on higher bitrates.
    padding_bytes_in_packet =
        rtc::SafeMin<size_t>(max_payload_size, kMaxPaddingLength);
  }
  size_t bytes_sent = 0;
  while (bytes_sent < bytes) {
    int64_t now_ms = clock_->TimeInMilliseconds();
    uint32_t ssrc;
    uint32_t timestamp;
    int64_t capture_time_ms;
    uint16_t sequence_number;
    int payload_type;
    bool over_rtx;
    {
      rtc::CritScope lock(&send_critsect_);
      if (!sending_media_)
        break;
      timestamp = last_rtp_timestamp_;
      capture_time_ms = capture_time_ms_;
      if (rtx_ == kRtxOff) {
        if (last_payload_type_ == -1)
          break;
        // Without RTX we can't send padding in the middle of frames.
        // For audio marker bits doesn't mark the end of a frame and frames
        // are usually a single packet, so for now we don't apply this rule
        // for audio.
        if (!audio_configured_ && !last_packet_marker_bit_) {
          break;
        }
        if (!ssrc_) {
          RTC_LOG(LS_ERROR) << "SSRC unset.";
          return 0;
        }

        RTC_DCHECK(ssrc_);
        ssrc = *ssrc_;

        sequence_number = sequence_number_;
        ++sequence_number_;
        payload_type = last_payload_type_;
        over_rtx = false;
      } else {
        // Without abs-send-time or transport sequence number a media packet
        // must be sent before padding so that the timestamps used for
        // estimation are correct.
        if (!media_has_been_sent_ &&
            !(rtp_header_extension_map_.IsRegistered(AbsoluteSendTime::kId) ||
              (rtp_header_extension_map_.IsRegistered(
                   TransportSequenceNumber::kId) &&
               transport_sequence_number_allocator_))) {
          break;
        }
        // Only change change the timestamp of padding packets sent over RTX.
        // Padding only packets over RTP has to be sent as part of a media
        // frame (and therefore the same timestamp).
        if (last_timestamp_time_ms_ > 0) {
          timestamp +=
              (now_ms - last_timestamp_time_ms_) * kTimestampTicksPerMs;
          capture_time_ms += (now_ms - last_timestamp_time_ms_);
        }
        if (!ssrc_rtx_) {
          RTC_LOG(LS_ERROR) << "RTX SSRC unset.";
          return 0;
        }
        RTC_DCHECK(ssrc_rtx_);
        ssrc = *ssrc_rtx_;
        sequence_number = sequence_number_rtx_;
        ++sequence_number_rtx_;
        payload_type = rtx_payload_type_map_.begin()->second;
        over_rtx = true;
      }
    }

    RtpPacketToSend padding_packet(&rtp_header_extension_map_);
    padding_packet.SetPayloadType(payload_type);
    padding_packet.SetMarker(false);
    padding_packet.SetSequenceNumber(sequence_number);
    padding_packet.SetTimestamp(timestamp);
    padding_packet.SetSsrc(ssrc);

    if (capture_time_ms > 0) {
      padding_packet.SetExtension<TransmissionOffset>(
          (now_ms - capture_time_ms) * kTimestampTicksPerMs);
    }
    padding_packet.SetExtension<AbsoluteSendTime>(
        AbsoluteSendTime::MsTo24Bits(now_ms));
    PacketOptions options;
    // Padding packets are never retransmissions.
    options.is_retransmit = false;
    bool has_transport_seq_num;
    {
      rtc::CritScope lock(&send_critsect_);
      has_transport_seq_num =
          UpdateTransportSequenceNumber(&padding_packet, &options.packet_id);
      options.included_in_allocation =
          has_transport_seq_num || force_part_of_allocation_;
      options.included_in_feedback = has_transport_seq_num;
    }
    padding_packet.SetPadding(padding_bytes_in_packet);
    if (has_transport_seq_num) {
      AddPacketToTransportFeedback(options.packet_id, padding_packet,
                                   pacing_info);
    }

    if (!SendPacketToNetwork(padding_packet, options, pacing_info))
      break;

    bytes_sent += padding_bytes_in_packet;
    UpdateRtpStats(padding_packet, over_rtx, false);
  }

  return bytes_sent;
}

void RTPSender::SetStorePacketsStatus(bool enable, uint16_t number_to_store) {
  RtpPacketHistory::StorageMode mode =
      enable ? RtpPacketHistory::StorageMode::kStore
             : RtpPacketHistory::StorageMode::kDisabled;
  packet_history_.SetStorePacketsStatus(mode, number_to_store);
}

bool RTPSender::StorePackets() const {
  return packet_history_.GetStorageMode() !=
         RtpPacketHistory::StorageMode::kDisabled;
}

int32_t RTPSender::ReSendPacket(uint16_t packet_id) {
  // Try to find packet in RTP packet history. Also verify RTT here, so that we
  // don't retransmit too often.
  absl::optional<RtpPacketHistory::PacketState> stored_packet =
      packet_history_.GetPacketState(packet_id);
  if (!stored_packet) {
    // Packet not found.
    return 0;
  }

  const int32_t packet_size = static_cast<int32_t>(stored_packet->packet_size);

  // Skip retransmission rate check if not configured.
  if (retransmission_rate_limiter_) {
    // Check if we're overusing retransmission bitrate.
    // TODO(sprang): Add histograms for nack success or failure reasons.
    if (!retransmission_rate_limiter_->TryUseRate(packet_size)) {
      return -1;
    }
  }

  if (paced_sender_) 
  {
    // Convert from TickTime to Clock since capture_time_ms is based on
    // TickTime.
    int64_t corrected_capture_tims_ms =
        stored_packet->capture_time_ms + clock_delta_ms_;
    paced_sender_->InsertPacket(
        RtpPacketSender::kNormalPriority, stored_packet->ssrc,
        stored_packet->rtp_sequence_number, corrected_capture_tims_ms,
        stored_packet->packet_size, true);

    return packet_size;
  }

  std::unique_ptr<RtpPacketToSend> packet =
      packet_history_.GetPacketAndSetSendTime(packet_id);
  if (!packet) {
    // Packet could theoretically time out between the first check and this one.
    return 0;
  }

  const bool rtx = (RtxStatus() & kRtxRetransmitted) > 0;
  if (!PrepareAndSendPacket(std::move(packet), rtx, true, PacedPacketInfo()))
    return -1;

  return packet_size;
}

bool RTPSender::SendPacketToNetwork(const RtpPacketToSend& packet,
                                    const PacketOptions& options,
                                    const PacedPacketInfo& pacing_info) {
  int bytes_sent = -1;
  if (transport_) {
    UpdateRtpOverhead(packet);
    bytes_sent = transport_->SendRtp(packet.data(), packet.size(), options)
                     ? static_cast<int>(packet.size())
                     : -1;
    if (event_log_ && bytes_sent > 0) {
      event_log_->Log(absl::make_unique<RtcEventRtpPacketOutgoing>(
          packet, pacing_info.probe_cluster_id));
    }
  }
  // TODO(pwestin): Add a separate bitrate for sent bitrate after pacer.
  if (bytes_sent <= 0) {
    RTC_LOG(LS_WARNING) << "Transport failed to send packet.";
    return false;
  }
  return true;
}

void RTPSender::OnReceivedNack(
    const std::vector<uint16_t>& nack_sequence_numbers,
    int64_t avg_rtt) 
{
  packet_history_.SetRtt(5 + avg_rtt);
  for (uint16_t seq_no : nack_sequence_numbers) 
  {
    const int32_t bytes_sent = ReSendPacket(seq_no);
    if (bytes_sent < 0) 
	{
      // Failed to send one Sequence number. Give up the rest in this nack.
      RTC_LOG(LS_WARNING) << "Failed resending RTP packet " << seq_no
                          << ", Discard rest of packets.";
      break;
    }
  }
}

// Called from pacer when we can send the packet.
bool RTPSender::TimeToSendPacket(uint32_t ssrc,
                                 uint16_t sequence_number,
                                 int64_t capture_time_ms,
                                 bool retransmission,
                                 const PacedPacketInfo& pacing_info) {
  if (!SendingMedia())
    return true;

  std::unique_ptr<RtpPacketToSend> packet;
  if (ssrc == SSRC()) {
    packet = packet_history_.GetPacketAndSetSendTime(sequence_number);
  } else if (ssrc == FlexfecSsrc()) {
    packet = flexfec_packet_history_.GetPacketAndSetSendTime(sequence_number);
  }

  if (!packet) {
    // Packet cannot be found or was resend too recently.
    return true;
  }

  return PrepareAndSendPacket(
      std::move(packet),
      retransmission && (RtxStatus() & kRtxRetransmitted) > 0, retransmission,
      pacing_info);
}

bool RTPSender::PrepareAndSendPacket(std::unique_ptr<RtpPacketToSend> packet,
                                     bool send_over_rtx,
                                     bool is_retransmit,
                                     const PacedPacketInfo& pacing_info) {
  RTC_DCHECK(packet);
  int64_t capture_time_ms = packet->capture_time_ms();
  RtpPacketToSend* packet_to_send = packet.get();

  std::unique_ptr<RtpPacketToSend> packet_rtx;
  if (send_over_rtx) {
    packet_rtx = BuildRtxPacket(*packet);
    if (!packet_rtx)
      return false;
    packet_to_send = packet_rtx.get();
  }

  // Bug webrtc:7859. While FEC is invoked from rtp_sender_video, and not after
  // the pacer, these modifications of the header below are happening after the
  // FEC protection packets are calculated. This will corrupt recovered packets
  // at the same place. It's not an issue for extensions, which are present in
  // all the packets (their content just may be incorrect on recovered packets).
  // In case of VideoTimingExtension, since it's present not in every packet,
  // data after rtp header may be corrupted if these packets are protected by
  // the FEC.
  int64_t now_ms = clock_->TimeInMilliseconds();
  int64_t diff_ms = now_ms - capture_time_ms;
  packet_to_send->SetExtension<TransmissionOffset>(kTimestampTicksPerMs *
                                                   diff_ms);
  packet_to_send->SetExtension<AbsoluteSendTime>(
      AbsoluteSendTime::MsTo24Bits(now_ms));

  if (packet_to_send->HasExtension<VideoTimingExtension>()) {
    if (populate_network2_timestamp_) {
      packet_to_send->set_network2_time_ms(now_ms);
    } else {
      packet_to_send->set_pacer_exit_time_ms(now_ms);
    }
  }

  PacketOptions options;
  // If we are sending over RTX, it also means this is a retransmission.
  // E.g. RTPSender::TrySendRedundantPayloads calls PrepareAndSendPacket with
  // send_over_rtx = true but is_retransmit = false.
  options.is_retransmit = is_retransmit || send_over_rtx;
  bool has_transport_seq_num;
  {
    rtc::CritScope lock(&send_critsect_);
    has_transport_seq_num =
        UpdateTransportSequenceNumber(packet_to_send, &options.packet_id);
    options.included_in_allocation =
        has_transport_seq_num || force_part_of_allocation_;
    options.included_in_feedback = has_transport_seq_num;
  }
  if (has_transport_seq_num) {
    AddPacketToTransportFeedback(options.packet_id, *packet_to_send,
                                 pacing_info);
  }
  options.application_data.assign(packet_to_send->application_data().begin(),
                                  packet_to_send->application_data().end());

  if (!is_retransmit && !send_over_rtx) {
    UpdateDelayStatistics(packet->capture_time_ms(), now_ms);
    UpdateOnSendPacket(options.packet_id, packet->capture_time_ms(),
                       packet->Ssrc());
  }

  if (!SendPacketToNetwork(*packet_to_send, options, pacing_info))
    return false;

  {
    rtc::CritScope lock(&send_critsect_);
    media_has_been_sent_ = true;
  }
  UpdateRtpStats(*packet_to_send, send_over_rtx, is_retransmit);
  return true;
}

void RTPSender::UpdateRtpStats(const RtpPacketToSend& packet,
                               bool is_rtx,
                               bool is_retransmit) {
  int64_t now_ms = clock_->TimeInMilliseconds();

  rtc::CritScope lock(&statistics_crit_);
  StreamDataCounters* counters = is_rtx ? &rtx_rtp_stats_ : &rtp_stats_;

  total_bitrate_sent_.Update(packet.size(), now_ms);

  if (counters->first_packet_time_ms == -1)
    counters->first_packet_time_ms = now_ms;

  if (packet.is_fec())
    counters->fec.AddPacket(packet);

  if (is_retransmit) {
    counters->retransmitted.AddPacket(packet);
    nack_bitrate_sent_.Update(packet.size(), now_ms);
  }
  counters->transmitted.AddPacket(packet);

  if (rtp_stats_callback_)
    rtp_stats_callback_->DataCountersUpdated(*counters, packet.Ssrc());
}

size_t RTPSender::TimeToSendPadding(size_t bytes,
                                    const PacedPacketInfo& pacing_info) {
  if (bytes == 0)
    return 0;
  size_t bytes_sent = TrySendRedundantPayloads(bytes, pacing_info);
  if (bytes_sent < bytes)
    bytes_sent += SendPadData(bytes - bytes_sent, pacing_info);
  return bytes_sent;
}

bool RTPSender::SendToNetwork(std::unique_ptr<RtpPacketToSend> packet,
                              StorageType storage,
                              RtpPacketSender::Priority priority) {
  RTC_DCHECK(packet);
  int64_t now_ms = clock_->TimeInMilliseconds();

  uint32_t ssrc = packet->Ssrc();
  if (paced_sender_) {
    uint16_t seq_no = packet->SequenceNumber();
    // Correct offset between implementations of millisecond time stamps in
    // TickTime and Clock.
    int64_t corrected_time_ms = packet->capture_time_ms() + clock_delta_ms_;
    size_t packet_size =
        send_side_bwe_with_overhead_ ? packet->size() : packet->payload_size();
    if (ssrc == FlexfecSsrc()) {
      // Store FlexFEC packets in the history here, so they can be found
      // when the pacer calls TimeToSendPacket.
      flexfec_packet_history_.PutRtpPacket(std::move(packet), storage,
                                           absl::nullopt);
    } else {
      packet_history_.PutRtpPacket(std::move(packet), storage, absl::nullopt);
    }

    paced_sender_->InsertPacket(priority, ssrc, seq_no, corrected_time_ms,
                                packet_size, false);
    return true;
  }

  PacketOptions options;
  options.is_retransmit = false;

  // |capture_time_ms| <= 0 is considered invalid.
  // TODO(holmer): This should be changed all over Video Engine so that negative
  // time is consider invalid, while 0 is considered a valid time.
  if (packet->capture_time_ms() > 0) {
    packet->SetExtension<TransmissionOffset>(
        kTimestampTicksPerMs * (now_ms - packet->capture_time_ms()));

    if (populate_network2_timestamp_ &&
        packet->HasExtension<VideoTimingExtension>()) {
      packet->set_network2_time_ms(now_ms);
    }
  }
  packet->SetExtension<AbsoluteSendTime>(AbsoluteSendTime::MsTo24Bits(now_ms));

  bool has_transport_seq_num;
  {
    rtc::CritScope lock(&send_critsect_);
    has_transport_seq_num =
        UpdateTransportSequenceNumber(packet.get(), &options.packet_id);
    options.included_in_allocation =
        has_transport_seq_num || force_part_of_allocation_;
    options.included_in_feedback = has_transport_seq_num;
  }
  if (has_transport_seq_num) {
    AddPacketToTransportFeedback(options.packet_id, *packet.get(),
                                 PacedPacketInfo());
  }
  options.application_data.assign(packet->application_data().begin(),
                                  packet->application_data().end());

  UpdateDelayStatistics(packet->capture_time_ms(), now_ms);
  UpdateOnSendPacket(options.packet_id, packet->capture_time_ms(),
                     packet->Ssrc());

  bool sent = SendPacketToNetwork(*packet, options, PacedPacketInfo());

  if (sent) {
    {
      rtc::CritScope lock(&send_critsect_);
      media_has_been_sent_ = true;
    }
    UpdateRtpStats(*packet, false, false);
  }

  // To support retransmissions, we store the media packet as sent in the
  // packet history (even if send failed).
  if (storage == kAllowRetransmission) {
    RTC_DCHECK_EQ(ssrc, SSRC());
    packet_history_.PutRtpPacket(std::move(packet), storage, now_ms);
  }

  return sent;
}

void RTPSender::RecomputeMaxSendDelay() {
  max_delay_it_ = send_delays_.begin();
  for (auto it = send_delays_.begin(); it != send_delays_.end(); ++it) {
    if (it->second >= max_delay_it_->second) {
      max_delay_it_ = it;
    }
  }
}

void RTPSender::UpdateDelayStatistics(int64_t capture_time_ms, int64_t now_ms) {
  if (!send_side_delay_observer_ || capture_time_ms <= 0)
    return;

  uint32_t ssrc;
  int avg_delay_ms = 0;
  int max_delay_ms = 0;
  {
    rtc::CritScope lock(&send_critsect_);
    if (!ssrc_)
      return;
    ssrc = *ssrc_;
  }
  {
    rtc::CritScope cs(&statistics_crit_);
    // Compute the max and average of the recent capture-to-send delays.
    // The time complexity of the current approach depends on the distribution
    // of the delay values. This could be done more efficiently.

    // Remove elements older than kSendSideDelayWindowMs.
    auto lower_bound =
        send_delays_.lower_bound(now_ms - kSendSideDelayWindowMs);
    for (auto it = send_delays_.begin(); it != lower_bound; ++it) {
      if (max_delay_it_ == it) {
        max_delay_it_ = send_delays_.end();
      }
      sum_delays_ms_ -= it->second;
    }
    send_delays_.erase(send_delays_.begin(), lower_bound);
    if (max_delay_it_ == send_delays_.end()) {
      // Removed the previous max. Need to recompute.
      RecomputeMaxSendDelay();
    }

    // Add the new element.
    RTC_DCHECK_GE(now_ms, static_cast<int64_t>(0));
    RTC_DCHECK_LE(now_ms, std::numeric_limits<int64_t>::max() / 2);
    RTC_DCHECK_GE(capture_time_ms, static_cast<int64_t>(0));
    RTC_DCHECK_LE(capture_time_ms, std::numeric_limits<int64_t>::max() / 2);
    int64_t diff_ms = now_ms - capture_time_ms;
    RTC_DCHECK_GE(diff_ms, static_cast<int64_t>(0));
    RTC_DCHECK_LE(diff_ms,
                  static_cast<int64_t>(std::numeric_limits<int>::max()));
    int new_send_delay = rtc::dchecked_cast<int>(now_ms - capture_time_ms);
    SendDelayMap::iterator it;
    bool inserted;
    std::tie(it, inserted) =
        send_delays_.insert(std::make_pair(now_ms, new_send_delay));
    if (!inserted) {
      // TODO(terelius): If we have multiple delay measurements during the same
      // millisecond then we keep the most recent one. It is not clear that this
      // is the right decision, but it preserves an earlier behavior.
      int previous_send_delay = it->second;
      sum_delays_ms_ -= previous_send_delay;
      it->second = new_send_delay;
      if (max_delay_it_ == it && new_send_delay < previous_send_delay) {
        RecomputeMaxSendDelay();
      }
    }
    if (max_delay_it_ == send_delays_.end() ||
        it->second >= max_delay_it_->second) {
      max_delay_it_ = it;
    }
    sum_delays_ms_ += new_send_delay;

    size_t num_delays = send_delays_.size();
    RTC_DCHECK(max_delay_it_ != send_delays_.end());
    max_delay_ms = rtc::dchecked_cast<int>(max_delay_it_->second);
    int64_t avg_ms = (sum_delays_ms_ + num_delays / 2) / num_delays;
    RTC_DCHECK_GE(avg_ms, static_cast<int64_t>(0));
    RTC_DCHECK_LE(avg_ms,
                  static_cast<int64_t>(std::numeric_limits<int>::max()));
    avg_delay_ms =
        rtc::dchecked_cast<int>((sum_delays_ms_ + num_delays / 2) / num_delays);
  }
  send_side_delay_observer_->SendSideDelayUpdated(avg_delay_ms, max_delay_ms,
                                                  ssrc);
}

void RTPSender::UpdateOnSendPacket(int packet_id,
                                   int64_t capture_time_ms,
                                   uint32_t ssrc) {
  if (!send_packet_observer_ || capture_time_ms <= 0 || packet_id == -1)
    return;

  send_packet_observer_->OnSendPacket(packet_id, capture_time_ms, ssrc);
}

void RTPSender::ProcessBitrate() {
  if (!bitrate_callback_)
    return;
  int64_t now_ms = clock_->TimeInMilliseconds();
  uint32_t ssrc;
  {
    rtc::CritScope lock(&send_critsect_);
    if (!ssrc_)
      return;
    ssrc = *ssrc_;
  }

  rtc::CritScope lock(&statistics_crit_);
  bitrate_callback_->Notify(total_bitrate_sent_.Rate(now_ms).value_or(0),
                            nack_bitrate_sent_.Rate(now_ms).value_or(0), ssrc);
}

size_t RTPSender::RtpHeaderLength() const {
  rtc::CritScope lock(&send_critsect_);
  size_t rtp_header_length = kRtpHeaderLength;
  rtp_header_length += sizeof(uint32_t) * csrcs_.size();
  rtp_header_length += RtpHeaderExtensionSize(kFecOrPaddingExtensionSizes,
                                              rtp_header_extension_map_);
  return rtp_header_length;
}

uint16_t RTPSender::AllocateSequenceNumber(uint16_t packets_to_send) {
  rtc::CritScope lock(&send_critsect_);
  uint16_t first_allocated_sequence_number = sequence_number_;
  sequence_number_ += packets_to_send;
  return first_allocated_sequence_number;
}

void RTPSender::GetDataCounters(StreamDataCounters* rtp_stats,
                                StreamDataCounters* rtx_stats) const {
  rtc::CritScope lock(&statistics_crit_);
  *rtp_stats = rtp_stats_;
  *rtx_stats = rtx_rtp_stats_;
}

std::unique_ptr<RtpPacketToSend> RTPSender::AllocatePacket() const {
  rtc::CritScope lock(&send_critsect_);
  // TODO(danilchap): Find better motivator and value for extra capacity.
  // RtpPacketizer might slightly miscalulate needed size,
  // SRTP may benefit from extra space in the buffer and do encryption in place
  // saving reallocation.
  // While sending slightly oversized packet increase chance of dropped packet,
  // it is better than crash on drop packet without trying to send it.
  static constexpr int kExtraCapacity = 16;
  auto packet = absl::make_unique<RtpPacketToSend>(
      &rtp_header_extension_map_, max_packet_size_ + kExtraCapacity);
  RTC_DCHECK(ssrc_);
  packet->SetSsrc(*ssrc_);
  packet->SetCsrcs(csrcs_);
  // Reserve extensions, if registered, RtpSender set in SendToNetwork.
  packet->ReserveExtension<AbsoluteSendTime>();
  packet->ReserveExtension<TransmissionOffset>();
  packet->ReserveExtension<TransportSequenceNumber>();

  if (!mid_.empty()) {
    // This is a no-op if the MID header extension is not registered.
    packet->SetExtension<RtpMid>(mid_);
  }
  if (!rid_.empty()) {
    // This is a no-op if the RID header extension is not registered.
    packet->SetExtension<RtpStreamId>(rid_);
  }
  return packet;
}

bool RTPSender::AssignSequenceNumber(RtpPacketToSend* packet) {
  rtc::CritScope lock(&send_critsect_);
  if (!sending_media_)
    return false;
  RTC_DCHECK(packet->Ssrc() == ssrc_);
  packet->SetSequenceNumber(sequence_number_++);

  // Remember marker bit to determine if padding can be inserted with
  // sequence number following |packet|.
  last_packet_marker_bit_ = packet->Marker();
  // Remember payload type to use in the padding packet if rtx is disabled.
  last_payload_type_ = packet->PayloadType();
  // Save timestamps to generate timestamp field and extensions for the padding.
  last_rtp_timestamp_ = packet->Timestamp();
  last_timestamp_time_ms_ = clock_->TimeInMilliseconds();
  capture_time_ms_ = packet->capture_time_ms();
  return true;
}

bool RTPSender::UpdateTransportSequenceNumber(RtpPacketToSend* packet,
                                              int* packet_id) {
  RTC_DCHECK(packet);
  RTC_DCHECK(packet_id);
  if (!rtp_header_extension_map_.IsRegistered(TransportSequenceNumber::kId))
    return false;

  if (!transport_sequence_number_allocator_)
    return false;

  *packet_id = transport_sequence_number_allocator_->AllocateSequenceNumber();

  if (!packet->SetExtension<TransportSequenceNumber>(*packet_id))
    return false;

  return true;
}

void RTPSender::SetSendingMediaStatus(bool enabled) {
  rtc::CritScope lock(&send_critsect_);
  sending_media_ = enabled;
}

bool RTPSender::SendingMedia() const {
  rtc::CritScope lock(&send_critsect_);
  return sending_media_;
}

void RTPSender::SetAsPartOfAllocation(bool part_of_allocation) {
  rtc::CritScope lock(&send_critsect_);
  force_part_of_allocation_ = part_of_allocation;
}

void RTPSender::SetTimestampOffset(uint32_t timestamp) {
  rtc::CritScope lock(&send_critsect_);
  timestamp_offset_ = timestamp;
}

uint32_t RTPSender::TimestampOffset() const {
  rtc::CritScope lock(&send_critsect_);
  return timestamp_offset_;
}

void RTPSender::SetSSRC(uint32_t ssrc) {
  // This is configured via the API.
  rtc::CritScope lock(&send_critsect_);

  if (ssrc_ == ssrc) {
    return;  // Since it's same ssrc, don't reset anything.
  }
  ssrc_.emplace(ssrc);
  if (!sequence_number_forced_) {
    sequence_number_ = random_.Rand(1, kMaxInitRtpSeqNumber);
  }
}

uint32_t RTPSender::SSRC() const {
  rtc::CritScope lock(&send_critsect_);
  RTC_DCHECK(ssrc_);
  return *ssrc_;
}

void RTPSender::SetRid(const std::string& rid) {
  // RID is used in simulcast scenario when multiple layers share the same mid.
  rtc::CritScope lock(&send_critsect_);
  RTC_DCHECK_LE(rid.length(), RtpStreamId::kMaxValueSizeBytes);
  rid_ = rid;
}

void RTPSender::SetMid(const std::string& mid) {
  // This is configured via the API.
  rtc::CritScope lock(&send_critsect_);
  mid_ = mid;
}

absl::optional<uint32_t> RTPSender::FlexfecSsrc() const {
  return flexfec_ssrc_;
}

void RTPSender::SetCsrcs(const std::vector<uint32_t>& csrcs) {
  RTC_DCHECK_LE(csrcs.size(), kRtpCsrcSize);
  rtc::CritScope lock(&send_critsect_);
  csrcs_ = csrcs;
}

void RTPSender::SetSequenceNumber(uint16_t seq) {
  rtc::CritScope lock(&send_critsect_);
  sequence_number_forced_ = true;
  sequence_number_ = seq;
}

uint16_t RTPSender::SequenceNumber() const {
  rtc::CritScope lock(&send_critsect_);
  return sequence_number_;
}

static void CopyHeaderAndExtensionsToRtxPacket(const RtpPacketToSend& packet,
                                               RtpPacketToSend* rtx_packet) {
  // Set the relevant fixed packet headers. The following are not set:
  // * Payload type - it is replaced in rtx packets.
  // * Sequence number - RTX has a separate sequence numbering.
  // * SSRC - RTX stream has its own SSRC.
  rtx_packet->SetMarker(packet.Marker());
  rtx_packet->SetTimestamp(packet.Timestamp());

  // Set the variable fields in the packet header:
  // * CSRCs - must be set before header extensions.
  // * Header extensions - replace Rid header with RepairedRid header.
  const std::vector<uint32_t> csrcs = packet.Csrcs();
  rtx_packet->SetCsrcs(csrcs);
  for (int extension = kRtpExtensionNone + 1;
       extension < kRtpExtensionNumberOfExtensions; ++extension) {
    RTPExtensionType source_extension =
        static_cast<RTPExtensionType>(extension);
    // Rid header should be replaced with RepairedRid header
    RTPExtensionType destination_extension =
        source_extension == kRtpExtensionRtpStreamId
            ? kRtpExtensionRepairedRtpStreamId
            : source_extension;

    // Empty extensions should be supported, so not checking |source.empty()|.
    if (!packet.HasExtension(source_extension)) {
      continue;
    }

    rtc::ArrayView<const uint8_t> source =
        packet.FindExtension(source_extension);

    rtc::ArrayView<uint8_t> destination =
        rtx_packet->AllocateExtension(destination_extension, source.size());

    // Could happen if any:
    // 1. Extension has 0 length.
    // 2. Extension is not registered in destination.
    // 3. Allocating extension in destination failed.
    if (destination.empty() || source.size() != destination.size()) {
      continue;
    }

    std::memcpy(destination.begin(), source.begin(), destination.size());
  }
}

std::unique_ptr<RtpPacketToSend> RTPSender::BuildRtxPacket(
    const RtpPacketToSend& packet) {
  std::unique_ptr<RtpPacketToSend> rtx_packet;

  // Add original RTP header.
  {
    rtc::CritScope lock(&send_critsect_);
    if (!sending_media_)
      return nullptr;

    RTC_DCHECK(ssrc_rtx_);

    // Replace payload type.
    auto kv = rtx_payload_type_map_.find(packet.PayloadType());
    if (kv == rtx_payload_type_map_.end())
      return nullptr;

    rtx_packet = absl::make_unique<RtpPacketToSend>(&rtp_header_extension_map_,
                                                    max_packet_size_);

    rtx_packet->SetPayloadType(kv->second);

    // Replace sequence number.
    rtx_packet->SetSequenceNumber(sequence_number_rtx_++);

    // Replace SSRC.
    rtx_packet->SetSsrc(*ssrc_rtx_);

    CopyHeaderAndExtensionsToRtxPacket(packet, rtx_packet.get());

    // The spec indicates that it is possible for a sender to stop sending mids
    // once the SSRCs have been bound on the receiver. As a result the source
    // rtp packet might not have the MID header extension set.
    // However, the SSRC of the RTX stream might not have been bound on the
    // receiver. This means that we should include it here.
    // The same argument goes for the Repaired RID extension.
    if (!mid_.empty()) {
      // This is a no-op if the MID header extension is not registered.
      rtx_packet->SetExtension<RtpMid>(mid_);
    }
    if (!rid_.empty()) {
      // This is a no-op if the Repaired-RID header extension is not registered.
      // rtx_packet->SetExtension<RepairedRtpStreamId>(rid_);
    }
  }
  RTC_DCHECK(rtx_packet);

  uint8_t* rtx_payload =
      rtx_packet->AllocatePayload(packet.payload_size() + kRtxHeaderSize);
  if (rtx_payload == nullptr)
    return nullptr;

  // Add OSN (original sequence number).
  ByteWriter<uint16_t>::WriteBigEndian(rtx_payload, packet.SequenceNumber());

  // Add original payload data.
  auto payload = packet.payload();
  memcpy(rtx_payload + kRtxHeaderSize, payload.data(), payload.size());

  // Add original application data.
  rtx_packet->set_application_data(packet.application_data());

  return rtx_packet;
}

void RTPSender::RegisterRtpStatisticsCallback(
    StreamDataCountersCallback* callback) {
  rtc::CritScope cs(&statistics_crit_);
  rtp_stats_callback_ = callback;
}

StreamDataCountersCallback* RTPSender::GetRtpStatisticsCallback() const {
  rtc::CritScope cs(&statistics_crit_);
  return rtp_stats_callback_;
}

uint32_t RTPSender::BitrateSent() const {
  rtc::CritScope cs(&statistics_crit_);
  return total_bitrate_sent_.Rate(clock_->TimeInMilliseconds()).value_or(0);
}

void RTPSender::SetRtpState(const RtpState& rtp_state) {
  rtc::CritScope lock(&send_critsect_);
  sequence_number_ = rtp_state.sequence_number;
  sequence_number_forced_ = true;
  timestamp_offset_ = rtp_state.start_timestamp;
  last_rtp_timestamp_ = rtp_state.timestamp;
  capture_time_ms_ = rtp_state.capture_time_ms;
  last_timestamp_time_ms_ = rtp_state.last_timestamp_time_ms;
  media_has_been_sent_ = rtp_state.media_has_been_sent;
}

RtpState RTPSender::GetRtpState() const {
  rtc::CritScope lock(&send_critsect_);

  RtpState state;
  state.sequence_number = sequence_number_;
  state.start_timestamp = timestamp_offset_;
  state.timestamp = last_rtp_timestamp_;
  state.capture_time_ms = capture_time_ms_;
  state.last_timestamp_time_ms = last_timestamp_time_ms_;
  state.media_has_been_sent = media_has_been_sent_;

  return state;
}

void RTPSender::SetRtxRtpState(const RtpState& rtp_state) {
  rtc::CritScope lock(&send_critsect_);
  sequence_number_rtx_ = rtp_state.sequence_number;
}

RtpState RTPSender::GetRtxRtpState() const {
  rtc::CritScope lock(&send_critsect_);

  RtpState state;
  state.sequence_number = sequence_number_rtx_;
  state.start_timestamp = timestamp_offset_;

  return state;
}

void RTPSender::AddPacketToTransportFeedback(
    uint16_t packet_id,
    const RtpPacketToSend& packet,
    const PacedPacketInfo& pacing_info) {
  size_t packet_size = packet.payload_size() + packet.padding_size();
  if (send_side_bwe_with_overhead_) {
    packet_size = packet.size();
  }

  if (transport_feedback_observer_) {
    transport_feedback_observer_->AddPacket(SSRC(), packet_id, packet_size,
                                            pacing_info);
  }
}

void RTPSender::UpdateRtpOverhead(const RtpPacketToSend& packet) {
  if (!overhead_observer_)
    return;
  size_t overhead_bytes_per_packet;
  {
    rtc::CritScope lock(&send_critsect_);
    if (rtp_overhead_bytes_per_packet_ == packet.headers_size()) {
      return;
    }
    rtp_overhead_bytes_per_packet_ = packet.headers_size();
    overhead_bytes_per_packet = rtp_overhead_bytes_per_packet_;
  }
  overhead_observer_->OnOverheadChanged(overhead_bytes_per_packet);
}

int64_t RTPSender::LastTimestampTimeMs() const {
  rtc::CritScope lock(&send_critsect_);
  return last_timestamp_time_ms_;
}

void RTPSender::SetRtt(int64_t rtt_ms) {
  packet_history_.SetRtt(rtt_ms);
  flexfec_packet_history_.SetRtt(rtt_ms);
}
}  // namespace webrtc
