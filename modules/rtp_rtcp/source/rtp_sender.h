﻿/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef MODULES_RTP_RTCP_SOURCE_RTP_SENDER_H_
#define MODULES_RTP_RTCP_SOURCE_RTP_SENDER_H_

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/string_view.h"
#include "absl/types/optional.h"
#include "api/array_view.h"
#include "api/call/transport.h"
#include "api/transport/webrtc_key_value_config.h"
#include "modules/rtp_rtcp/include/flexfec_sender.h"
#include "modules/rtp_rtcp/include/rtp_header_extension_map.h"
#include "modules/rtp_rtcp/include/rtp_rtcp_defines.h"
#include "modules/rtp_rtcp/source/rtp_packet_history.h"
#include "modules/rtp_rtcp/source/rtp_rtcp_config.h"
#include "rtc_base/constructor_magic.h"
#include "rtc_base/critical_section.h"
#include "rtc_base/deprecation.h"
#include "rtc_base/random.h"
#include "rtc_base/rate_statistics.h"
#include "rtc_base/thread_annotations.h"

namespace webrtc {

class FrameEncryptorInterface;
class OverheadObserver;
class RateLimiter;
class RtcEventLog;
class RtpPacketToSend;

class RTPSender {
 public:
  RTPSender(bool audio,
            Clock* clock,
            Transport* transport,
            RtpPacketSender* paced_sender,
            absl::optional<uint32_t> flexfec_ssrc,
            TransportSequenceNumberAllocator* sequence_number_allocator,
            TransportFeedbackObserver* transport_feedback_callback,
            BitrateStatisticsObserver* bitrate_callback,
            SendSideDelayObserver* send_side_delay_observer,
            RtcEventLog* event_log,
            SendPacketObserver* send_packet_observer,
            RateLimiter* nack_rate_limiter,
            OverheadObserver* overhead_observer,
            bool populate_network2_timestamp,
            FrameEncryptorInterface* frame_encryptor,
            bool require_frame_encryption,
            bool extmap_allow_mixed,
            const WebRtcKeyValueConfig& field_trials);

  ~RTPSender();

  void ProcessBitrate();

  uint16_t ActualSendBitrateKbit() const;

  uint32_t NackOverheadRate() const;

  void SetSendingMediaStatus(bool enabled);
  bool SendingMedia() const;

  void SetAsPartOfAllocation(bool part_of_allocation);

  void GetDataCounters(StreamDataCounters* rtp_stats,
                       StreamDataCounters* rtx_stats) const;

  uint32_t TimestampOffset() const;
  void SetTimestampOffset(uint32_t timestamp);

  void SetSSRC(uint32_t ssrc);

  void SetRid(const std::string& rid);

  void SetMid(const std::string& mid);

  uint16_t SequenceNumber() const;
  void SetSequenceNumber(uint16_t seq);

  void SetCsrcs(const std::vector<uint32_t>& csrcs);

  void SetMaxRtpPacketSize(size_t max_packet_size);

  void SetExtmapAllowMixed(bool extmap_allow_mixed);

  // RTP header extension
  int32_t RegisterRtpHeaderExtension(RTPExtensionType type, uint8_t id);
  bool RegisterRtpHeaderExtension(const std::string& uri, int id);
  bool IsRtpHeaderExtensionRegistered(RTPExtensionType type) const;
  int32_t DeregisterRtpHeaderExtension(RTPExtensionType type);

  bool TimeToSendPacket(uint32_t ssrc,
                        uint16_t sequence_number,
                        int64_t capture_time_ms,
                        bool retransmission,
                        const PacedPacketInfo& pacing_info);
  size_t TimeToSendPadding(size_t bytes, const PacedPacketInfo& pacing_info);

  // NACK.
  // TODO@chensong 2023-04-02 接收对方掉包的sequence_numbers重传
  void OnReceivedNack(const std::vector<uint16_t>& nack_sequence_numbers,
                      int64_t avg_rtt);

  void SetStorePacketsStatus(bool enable, uint16_t number_to_store);

  bool StorePackets() const;

  int32_t ReSendPacket(uint16_t packet_id);

  // RTX.
  void SetRtxStatus(int mode);
  int RtxStatus() const;

  uint32_t RtxSsrc() const;
  void SetRtxSsrc(uint32_t ssrc);

  void SetRtxPayloadType(int payload_type, int associated_payload_type);

  // Size info for header extensions used by FEC packets.
  static rtc::ArrayView<const RtpExtensionSize> FecExtensionSizes();

  // Size info for header extensions used by video packets.
  static rtc::ArrayView<const RtpExtensionSize> VideoExtensionSizes();

  // Create empty packet, fills ssrc, csrcs and reserve place for header
  // extensions RtpSender updates before sending.
  std::unique_ptr<RtpPacketToSend> AllocatePacket() const;
  // Allocate sequence number for provided packet.
  // Save packet's fields to generate padding that doesn't break media stream.
  // Return false if sending was turned off.
  bool AssignSequenceNumber(RtpPacketToSend* packet);

  // Used for padding and FEC packets only.
  size_t RtpHeaderLength() const;
  uint16_t AllocateSequenceNumber(uint16_t packets_to_send);
  // Including RTP headers.
  size_t MaxRtpPacketSize() const;

  uint32_t SSRC() const;

  absl::optional<uint32_t> FlexfecSsrc() const;

  // Sends packet to |transport_| or to the pacer, depending on configuration.
  bool SendToNetwork(std::unique_ptr<RtpPacketToSend> packet,
                     StorageType storage,
                     RtpPacketSender::Priority priority);

  // Called on update of RTP statistics.
  void RegisterRtpStatisticsCallback(StreamDataCountersCallback* callback);
  StreamDataCountersCallback* GetRtpStatisticsCallback() const;

  uint32_t BitrateSent() const;

  void SetRtpState(const RtpState& rtp_state);
  RtpState GetRtpState() const;
  void SetRtxRtpState(const RtpState& rtp_state);
  RtpState GetRtxRtpState() const;

  int64_t LastTimestampTimeMs() const;

  void SetRtt(int64_t rtt_ms);

 private:
  // Maps capture time in milliseconds to send-side delay in milliseconds.
  // Send-side delay is the difference between transmission time and capture
  // time.
  typedef std::map<int64_t, int> SendDelayMap;

  size_t SendPadData(size_t bytes, const PacedPacketInfo& pacing_info);

  bool PrepareAndSendPacket(std::unique_ptr<RtpPacketToSend> packet,
                            bool send_over_rtx,
                            bool is_retransmit,
                            const PacedPacketInfo& pacing_info);

  // Return the number of bytes sent.  Note that both of these functions may
  // return a larger value that their argument.
  size_t TrySendRedundantPayloads(size_t bytes,
                                  const PacedPacketInfo& pacing_info);

  std::unique_ptr<RtpPacketToSend> BuildRtxPacket(
      const RtpPacketToSend& packet);

  // Sends packet on to |transport_|, leaving the RTP module.
  bool SendPacketToNetwork(const RtpPacketToSend& packet,
                           const PacketOptions& options,
                           const PacedPacketInfo& pacing_info);

  void RecomputeMaxSendDelay() RTC_EXCLUSIVE_LOCKS_REQUIRED(statistics_crit_);
  void UpdateDelayStatistics(int64_t capture_time_ms, int64_t now_ms);
  void UpdateOnSendPacket(int packet_id,
                          int64_t capture_time_ms,
                          uint32_t ssrc);

  bool UpdateTransportSequenceNumber(RtpPacketToSend* packet, int* packet_id)
      RTC_EXCLUSIVE_LOCKS_REQUIRED(send_critsect_);

  void UpdateRtpStats(const RtpPacketToSend& packet,
                      bool is_rtx,
                      bool is_retransmit);
  bool IsFecPacket(const RtpPacketToSend& packet) const;

  void AddPacketToTransportFeedback(uint16_t packet_id,
                                    const RtpPacketToSend& packet,
                                    const PacedPacketInfo& pacing_info);

  void UpdateRtpOverhead(const RtpPacketToSend& packet);

  Clock* const clock_;
  const int64_t clock_delta_ms_;
  Random random_ RTC_GUARDED_BY(send_critsect_);

  const bool audio_configured_;

  const absl::optional<uint32_t> flexfec_ssrc_;

  RtpPacketSender* const paced_sender_;
  TransportSequenceNumberAllocator* const transport_sequence_number_allocator_;
  TransportFeedbackObserver* const transport_feedback_observer_;
  rtc::CriticalSection send_critsect_;

  Transport* transport_;
  bool sending_media_ RTC_GUARDED_BY(send_critsect_);
  bool force_part_of_allocation_ RTC_GUARDED_BY(send_critsect_);
  size_t max_packet_size_;

  int8_t last_payload_type_ RTC_GUARDED_BY(send_critsect_);

  RtpHeaderExtensionMap rtp_header_extension_map_
      RTC_GUARDED_BY(send_critsect_);

  RtpPacketHistory packet_history_;
  // TODO(brandtr): Remove |flexfec_packet_history_| when the FlexfecSender
  // is hooked up to the PacedSender.
  RtpPacketHistory flexfec_packet_history_;

  // Statistics
  rtc::CriticalSection statistics_crit_;
  SendDelayMap send_delays_ RTC_GUARDED_BY(statistics_crit_);
  SendDelayMap::const_iterator max_delay_it_ RTC_GUARDED_BY(statistics_crit_);
  int64_t sum_delays_ms_ RTC_GUARDED_BY(statistics_crit_);
  StreamDataCounters rtp_stats_ RTC_GUARDED_BY(statistics_crit_);
  StreamDataCounters rtx_rtp_stats_ RTC_GUARDED_BY(statistics_crit_);
  StreamDataCountersCallback* rtp_stats_callback_
      RTC_GUARDED_BY(statistics_crit_);
  RateStatistics total_bitrate_sent_ RTC_GUARDED_BY(statistics_crit_);
  RateStatistics nack_bitrate_sent_ RTC_GUARDED_BY(statistics_crit_);
  SendSideDelayObserver* const send_side_delay_observer_;
  RtcEventLog* const event_log_;
  SendPacketObserver* const send_packet_observer_;
  BitrateStatisticsObserver* const bitrate_callback_;

  // RTP variables
  uint32_t timestamp_offset_ RTC_GUARDED_BY(send_critsect_);
  bool sequence_number_forced_ RTC_GUARDED_BY(send_critsect_);
  uint16_t sequence_number_ RTC_GUARDED_BY(send_critsect_);
  uint16_t sequence_number_rtx_ RTC_GUARDED_BY(send_critsect_);
  // Must be explicitly set by the application, use of absl::optional
  // only to keep track of correct use.
  absl::optional<uint32_t> ssrc_ RTC_GUARDED_BY(send_critsect_);
  // RID value to send in the RID or RepairedRID header extension.
  std::string rid_ RTC_GUARDED_BY(send_critsect_);
  // MID value to send in the MID header extension.
  std::string mid_ RTC_GUARDED_BY(send_critsect_);
  uint32_t last_rtp_timestamp_ RTC_GUARDED_BY(send_critsect_);
  int64_t capture_time_ms_ RTC_GUARDED_BY(send_critsect_);
  int64_t last_timestamp_time_ms_ RTC_GUARDED_BY(send_critsect_);
  bool media_has_been_sent_ RTC_GUARDED_BY(send_critsect_);
  bool last_packet_marker_bit_ RTC_GUARDED_BY(send_critsect_);
  std::vector<uint32_t> csrcs_ RTC_GUARDED_BY(send_critsect_);
  int rtx_ RTC_GUARDED_BY(send_critsect_);
  absl::optional<uint32_t> ssrc_rtx_ RTC_GUARDED_BY(send_critsect_);
  // Mapping rtx_payload_type_map_[associated] = rtx.
  std::map<int8_t, int8_t> rtx_payload_type_map_ RTC_GUARDED_BY(send_critsect_);
  size_t rtp_overhead_bytes_per_packet_ RTC_GUARDED_BY(send_critsect_);

  RateLimiter* const retransmission_rate_limiter_;
  OverheadObserver* overhead_observer_;
  const bool populate_network2_timestamp_;

  const bool send_side_bwe_with_overhead_;

  RTC_DISALLOW_IMPLICIT_CONSTRUCTORS(RTPSender);
};

}  // namespace webrtc

#endif  // MODULES_RTP_RTCP_SOURCE_RTP_SENDER_H_
