/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/rtp_rtcp/source/rtp_rtcp_impl.h"

#include <string.h>
#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <utility>

#include "absl/memory/memory.h"
#include "api/transport/field_trial_based_config.h"
#include "modules/rtp_rtcp/source/rtcp_packet/dlrr.h"
#include "modules/rtp_rtcp/source/rtp_rtcp_config.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"

#ifdef _WIN32
// Disable warning C4355: 'this' : used in base member initializer list.
#pragma warning(disable : 4355)
#endif

namespace webrtc {
namespace {
const int64_t kRtpRtcpMaxIdleTimeProcessMs = 5;
const int64_t kRtpRtcpRttProcessTimeMs = 1000;
const int64_t kRtpRtcpBitrateProcessTimeMs = 10;
const int64_t kDefaultExpectedRetransmissionTimeMs = 125;
constexpr int32_t kDefaultVideoReportInterval = 1000;
constexpr int32_t kDefaultAudioReportInterval = 5000;
}  // namespace

RtpRtcp::Configuration::Configuration() = default;

std::unique_ptr<RtpRtcp> RtpRtcp::Create(const Configuration& configuration) 
{
  RTC_DCHECK(configuration.clock);
	// TODO@chensong 2022-10-19   RTP-RTCP info
  return absl::make_unique<ModuleRtpRtcpImpl>(configuration);
}

RtpRtcp* RtpRtcp::CreateRtpRtcp(const RtpRtcp::Configuration& configuration) {
  // TODO@chensong 2022-10-19 create rtp_rtcp
  if (configuration.clock) 
  {
    return new ModuleRtpRtcpImpl(configuration);
  }
  else 
  {
    // No clock implementation provided, use default clock.
    RtpRtcp::Configuration configuration_copy;
    memcpy(&configuration_copy, &configuration, sizeof(RtpRtcp::Configuration));
    configuration_copy.clock = Clock::GetRealTimeClock();
    return new ModuleRtpRtcpImpl(configuration_copy);
  }
}

ModuleRtpRtcpImpl::ModuleRtpRtcpImpl(const Configuration& configuration)
    : rtcp_sender_(configuration.audio,
                   configuration.clock,
                   configuration.receive_statistics,
                   configuration.rtcp_packet_type_counter_observer,
                   configuration.event_log,
                   configuration.outgoing_transport,
                   configuration.rtcp_report_interval_ms > 0
                       ? configuration.rtcp_report_interval_ms
                       : (configuration.audio ? kDefaultAudioReportInterval
                                              : kDefaultVideoReportInterval)),
      rtcp_receiver_(configuration.clock,
                     configuration.receiver_only,
                     configuration.rtcp_packet_type_counter_observer,
                     configuration.bandwidth_callback,
                     configuration.intra_frame_callback,
                     configuration.rtcp_loss_notification_observer,
                     configuration.transport_feedback_callback,
                     configuration.bitrate_allocation_observer,
                     configuration.rtcp_report_interval_ms > 0
                         ? configuration.rtcp_report_interval_ms
                         : (configuration.audio ? kDefaultAudioReportInterval
                                                : kDefaultVideoReportInterval),
                     this),
      clock_(configuration.clock),
      last_bitrate_process_time_(clock_->TimeInMilliseconds()),
      last_rtt_process_time_(clock_->TimeInMilliseconds()),
      next_process_time_(clock_->TimeInMilliseconds() +
                         kRtpRtcpMaxIdleTimeProcessMs),
      packet_overhead_(28),  // IPV4 UDP.
      nack_last_time_sent_full_ms_(0),
      nack_last_seq_number_sent_(0),
      key_frame_req_method_(kKeyFrameReqPliRtcp),
      remote_bitrate_(configuration.remote_bitrate_estimator),
      ack_observer_(configuration.ack_observer),
      rtt_stats_(configuration.rtt_stats),
      rtt_ms_(0) {
  FieldTrialBasedConfig default_trials;
  if (!configuration.receiver_only) {
    rtp_sender_.reset(new RTPSender(
        configuration.audio, configuration.clock,
        configuration.outgoing_transport, configuration.paced_sender,
        configuration.flexfec_sender
            ? absl::make_optional(configuration.flexfec_sender->ssrc())
            : absl::nullopt,
        configuration.transport_sequence_number_allocator,
        configuration.transport_feedback_callback,
        configuration.send_bitrate_observer,
        configuration.send_side_delay_observer, configuration.event_log,
        configuration.send_packet_observer,
        configuration.retransmission_rate_limiter,
        configuration.overhead_observer,
        configuration.populate_network2_timestamp,
        configuration.frame_encryptor, configuration.require_frame_encryption,
        configuration.extmap_allow_mixed,
        configuration.field_trials ? *configuration.field_trials
                                   : default_trials));

    // Make sure rtcp sender use same timestamp offset as rtp sender.
    rtcp_sender_.SetTimestampOffset(rtp_sender_->TimestampOffset());
  }

  // Set default packet size limit.
  // TODO(nisse): Kind-of duplicates
  // webrtc::VideoSendStream::Config::Rtp::kDefaultMaxPacketSize.
  const size_t kTcpOverIpv4HeaderSize = 40;
  SetMaxRtpPacketSize(IP_PACKET_SIZE - kTcpOverIpv4HeaderSize);
}

ModuleRtpRtcpImpl::~ModuleRtpRtcpImpl() = default;

// Returns the number of milliseconds until the module want a worker thread
// to call Process.
int64_t ModuleRtpRtcpImpl::TimeUntilNextProcess() {
  return std::max<int64_t>(0,
                           next_process_time_ - clock_->TimeInMilliseconds());
}

// Process any pending tasks such as timeouts (non time critical events).
void ModuleRtpRtcpImpl::Process() {
  const int64_t now = clock_->TimeInMilliseconds();
  next_process_time_ = now + kRtpRtcpMaxIdleTimeProcessMs;

  if (rtp_sender_) {
    if (now >= last_bitrate_process_time_ + kRtpRtcpBitrateProcessTimeMs) {
      rtp_sender_->ProcessBitrate();
      last_bitrate_process_time_ = now;
      next_process_time_ =
          std::min(next_process_time_, now + kRtpRtcpBitrateProcessTimeMs);
    }
  }

  bool process_rtt = now >= last_rtt_process_time_ + kRtpRtcpRttProcessTimeMs;
  if (rtcp_sender_.Sending()) {
    // Process RTT if we have received a report block and we haven't
    // processed RTT for at least |kRtpRtcpRttProcessTimeMs| milliseconds.
    if (rtcp_receiver_.LastReceivedReportBlockMs() > last_rtt_process_time_ &&
        process_rtt) {
      std::vector<RTCPReportBlock> receive_blocks;
      rtcp_receiver_.StatisticsReceived(&receive_blocks);
      int64_t max_rtt = 0;
      for (std::vector<RTCPReportBlock>::iterator it = receive_blocks.begin();
           it != receive_blocks.end(); ++it) {
        int64_t rtt = 0;
        rtcp_receiver_.RTT(it->sender_ssrc, &rtt, NULL, NULL, NULL);
        max_rtt = (rtt > max_rtt) ? rtt : max_rtt;
      }
      // Report the rtt.
      if (rtt_stats_ && max_rtt != 0)
        rtt_stats_->OnRttUpdate(max_rtt);
    }

    // Verify receiver reports are delivered and the reported sequence number
    // is increasing.
    if (rtcp_receiver_.RtcpRrTimeout()) {
      RTC_LOG_F(LS_WARNING) << "Timeout: No RTCP RR received.";
    } else if (rtcp_receiver_.RtcpRrSequenceNumberTimeout()) {
      RTC_LOG_F(LS_WARNING) << "Timeout: No increase in RTCP RR extended "
                               "highest sequence number.";
    }

    if (remote_bitrate_ && rtcp_sender_.TMMBR()) {
      unsigned int target_bitrate = 0;
      std::vector<unsigned int> ssrcs;
      if (remote_bitrate_->LatestEstimate(&ssrcs, &target_bitrate)) {
        if (!ssrcs.empty()) {
          target_bitrate = target_bitrate / ssrcs.size();
        }
        rtcp_sender_.SetTargetBitrate(target_bitrate);
      }
    }
  } else {
    // Report rtt from receiver.
    if (process_rtt) {
      int64_t rtt_ms;
      if (rtt_stats_ && rtcp_receiver_.GetAndResetXrRrRtt(&rtt_ms)) {
        rtt_stats_->OnRttUpdate(rtt_ms);
      }
    }
  }

  // Get processed rtt.
  if (process_rtt) {
    last_rtt_process_time_ = now;
    next_process_time_ = std::min(
        next_process_time_, last_rtt_process_time_ + kRtpRtcpRttProcessTimeMs);
    if (rtt_stats_) {
      // Make sure we have a valid RTT before setting.
      int64_t last_rtt = rtt_stats_->LastProcessedRtt();
      if (last_rtt >= 0)
        set_rtt_ms(last_rtt);
    }
  }

  if (rtcp_sender_.TimeToSendRTCPReport())
    rtcp_sender_.SendRTCP(GetFeedbackState(), kRtcpReport);

  if (TMMBR() && rtcp_receiver_.UpdateTmmbrTimers()) {
    rtcp_receiver_.NotifyTmmbrUpdated();
  }
}

void ModuleRtpRtcpImpl::SetRtxSendStatus(int mode) {
  rtp_sender_->SetRtxStatus(mode);
}

int ModuleRtpRtcpImpl::RtxSendStatus() const {
  return rtp_sender_ ? rtp_sender_->RtxStatus() : kRtxOff;
}

void ModuleRtpRtcpImpl::SetRtxSsrc(uint32_t ssrc) {
  rtp_sender_->SetRtxSsrc(ssrc);
}

void ModuleRtpRtcpImpl::SetRtxSendPayloadType(int payload_type,
                                              int associated_payload_type) {
  rtp_sender_->SetRtxPayloadType(payload_type, associated_payload_type);
}

absl::optional<uint32_t> ModuleRtpRtcpImpl::FlexfecSsrc() const {
  if (rtp_sender_)
    return rtp_sender_->FlexfecSsrc();
  return absl::nullopt;
}

void ModuleRtpRtcpImpl::IncomingRtcpPacket(const uint8_t* rtcp_packet,
                                           const size_t length) {
  rtcp_receiver_.IncomingPacket(rtcp_packet, length);
}

void ModuleRtpRtcpImpl::RegisterSendPayloadFrequency(int payload_type,
                                                     int payload_frequency) {
  rtcp_sender_.SetRtpClockRate(payload_type, payload_frequency);
}

int32_t ModuleRtpRtcpImpl::DeRegisterSendPayload(const int8_t payload_type) {
  return 0;
}

uint32_t ModuleRtpRtcpImpl::StartTimestamp() const {
  return rtp_sender_->TimestampOffset();
}

// Configure start timestamp, default is a random number.
// TODO@chensong 2023-05-05 发送ntp包时间戳开始是随机数 
void ModuleRtpRtcpImpl::SetStartTimestamp(const uint32_t timestamp) {
  rtcp_sender_.SetTimestampOffset(timestamp);
  rtp_sender_->SetTimestampOffset(timestamp);
}

uint16_t ModuleRtpRtcpImpl::SequenceNumber() const {
  return rtp_sender_->SequenceNumber();
}

// Set SequenceNumber, default is a random number.
void ModuleRtpRtcpImpl::SetSequenceNumber(const uint16_t seq_num) {
  rtp_sender_->SetSequenceNumber(seq_num);
}

void ModuleRtpRtcpImpl::SetRtpState(const RtpState& rtp_state) {
  rtp_sender_->SetRtpState(rtp_state);
  rtcp_sender_.SetTimestampOffset(rtp_state.start_timestamp);
}

void ModuleRtpRtcpImpl::SetRtxState(const RtpState& rtp_state) {
  rtp_sender_->SetRtxRtpState(rtp_state);
}

RtpState ModuleRtpRtcpImpl::GetRtpState() const {
  return rtp_sender_->GetRtpState();
}

RtpState ModuleRtpRtcpImpl::GetRtxState() const {
  return rtp_sender_->GetRtxRtpState();
}

uint32_t ModuleRtpRtcpImpl::SSRC() const {
  return rtcp_sender_.SSRC();
}

void ModuleRtpRtcpImpl::SetSSRC(const uint32_t ssrc) {
  if (rtp_sender_) {
    rtp_sender_->SetSSRC(ssrc);
  }
  rtcp_sender_.SetSSRC(ssrc);
  SetRtcpReceiverSsrcs(ssrc);
}

void ModuleRtpRtcpImpl::SetRid(const std::string& rid) {
  if (rtp_sender_) {
    rtp_sender_->SetRid(rid);
  }
}

void ModuleRtpRtcpImpl::SetMid(const std::string& mid) {
  if (rtp_sender_) {
    rtp_sender_->SetMid(mid);
  }
  // TODO(bugs.webrtc.org/4050): If we end up supporting the MID SDES item for
  // RTCP, this will need to be passed down to the RTCPSender also.
}

void ModuleRtpRtcpImpl::SetCsrcs(const std::vector<uint32_t>& csrcs) {
  rtcp_sender_.SetCsrcs(csrcs);
  rtp_sender_->SetCsrcs(csrcs);
}

// TODO(pbos): Handle media and RTX streams separately (separate RTCP
// feedbacks).
RTCPSender::FeedbackState ModuleRtpRtcpImpl::GetFeedbackState() {
  RTCPSender::FeedbackState state;
  // This is called also when receiver_only is true. Hence below
  // checks that rtp_sender_ exists.
  if (rtp_sender_) {
    StreamDataCounters rtp_stats;
    StreamDataCounters rtx_stats;
    rtp_sender_->GetDataCounters(&rtp_stats, &rtx_stats);
    state.packets_sent =
        rtp_stats.transmitted.packets + rtx_stats.transmitted.packets;
    state.media_bytes_sent = rtp_stats.transmitted.payload_bytes +
                             rtx_stats.transmitted.payload_bytes;
    state.send_bitrate = rtp_sender_->BitrateSent();
  }
  state.module = this;

  LastReceivedNTP(&state.last_rr_ntp_secs, &state.last_rr_ntp_frac,
                  &state.remote_sr);

  state.last_xr_rtis = rtcp_receiver_.ConsumeReceivedXrReferenceTimeInfo();

  return state;
}

// TODO(nisse): This method shouldn't be called for a receive-only
// stream. Delete rtp_sender_ check as soon as all applications are
// updated.
int32_t ModuleRtpRtcpImpl::SetSendingStatus(const bool sending) {
  if (rtcp_sender_.Sending() != sending) {
    // Sends RTCP BYE when going from true to false
    if (rtcp_sender_.SetSendingStatus(GetFeedbackState(), sending) != 0) {
      RTC_LOG(LS_WARNING) << "Failed to send RTCP BYE";
    }
    if (sending && rtp_sender_) {
      // Update Rtcp receiver config, to track Rtx config changes from
      // the SetRtxStatus and SetRtxSsrc methods.
      SetRtcpReceiverSsrcs(rtp_sender_->SSRC());
    }
  }
  return 0;
}

bool ModuleRtpRtcpImpl::Sending() const {
  return rtcp_sender_.Sending();
}

// TODO(nisse): This method shouldn't be called for a receive-only
// stream. Delete rtp_sender_ check as soon as all applications are
// updated.
void ModuleRtpRtcpImpl::SetSendingMediaStatus(const bool sending) {
  if (rtp_sender_) {
    rtp_sender_->SetSendingMediaStatus(sending);
  } else {
    RTC_DCHECK(!sending);
  }
}

bool ModuleRtpRtcpImpl::SendingMedia() const {
  return rtp_sender_ ? rtp_sender_->SendingMedia() : false;
}

void ModuleRtpRtcpImpl::SetAsPartOfAllocation(bool part_of_allocation) {
  RTC_CHECK(rtp_sender_);
  rtp_sender_->SetAsPartOfAllocation(part_of_allocation);
}

bool ModuleRtpRtcpImpl::OnSendingRtpFrame(uint32_t timestamp,
                                          int64_t capture_time_ms,
                                          int payload_type,
                                          bool force_sender_report) {
	if (!Sending())
	{
		return false;
	}
  // 设置编码时间、capture的时间戳、 类型 payload_type 是编码器的编号？
  rtcp_sender_.SetLastRtpTime(timestamp, capture_time_ms, payload_type);
  // Make sure an RTCP report isn't queued behind a key frame.
  if (rtcp_sender_.TimeToSendRTCPReport(force_sender_report)) 
  {
    rtcp_sender_.SendRTCP(GetFeedbackState(), kRtcpReport);
  }

  return true;
}

bool ModuleRtpRtcpImpl::TimeToSendPacket(uint32_t ssrc,
                                         uint16_t sequence_number,
                                         int64_t capture_time_ms,
                                         bool retransmission,
                                         const PacedPacketInfo& pacing_info) {
  return rtp_sender_->TimeToSendPacket(ssrc, sequence_number, capture_time_ms,
                                       retransmission, pacing_info);
}

size_t ModuleRtpRtcpImpl::TimeToSendPadding(
    size_t bytes,
    const PacedPacketInfo& pacing_info) {
  return rtp_sender_->TimeToSendPadding(bytes, pacing_info);
}

size_t ModuleRtpRtcpImpl::MaxRtpPacketSize() const {
  return rtp_sender_->MaxRtpPacketSize();
}

void ModuleRtpRtcpImpl::SetMaxRtpPacketSize(size_t rtp_packet_size) {
  RTC_DCHECK_LE(rtp_packet_size, IP_PACKET_SIZE)
      << "rtp packet size too large: " << rtp_packet_size;
  RTC_DCHECK_GT(rtp_packet_size, packet_overhead_)
      << "rtp packet size too small: " << rtp_packet_size;

  rtcp_sender_.SetMaxRtpPacketSize(rtp_packet_size);
  if (rtp_sender_)
    rtp_sender_->SetMaxRtpPacketSize(rtp_packet_size);
}

RtcpMode ModuleRtpRtcpImpl::RTCP() const {
  return rtcp_sender_.Status();
}

// Configure RTCP status i.e on/off.
void ModuleRtpRtcpImpl::SetRTCPStatus(const RtcpMode method) {
  rtcp_sender_.SetRTCPStatus(method);
}

int32_t ModuleRtpRtcpImpl::SetCNAME(const char* c_name) {
  return rtcp_sender_.SetCNAME(c_name);
}

int32_t ModuleRtpRtcpImpl::AddMixedCNAME(uint32_t ssrc, const char* c_name) {
  return rtcp_sender_.AddMixedCNAME(ssrc, c_name);
}

int32_t ModuleRtpRtcpImpl::RemoveMixedCNAME(const uint32_t ssrc) {
  return rtcp_sender_.RemoveMixedCNAME(ssrc);
}

int32_t ModuleRtpRtcpImpl::RemoteCNAME(const uint32_t remote_ssrc,
                                       char c_name[RTCP_CNAME_SIZE]) const {
  return rtcp_receiver_.CNAME(remote_ssrc, c_name);
}

int32_t ModuleRtpRtcpImpl::RemoteNTP(uint32_t* received_ntpsecs,
                                     uint32_t* received_ntpfrac,
                                     uint32_t* rtcp_arrival_time_secs,
                                     uint32_t* rtcp_arrival_time_frac,
                                     uint32_t* rtcp_timestamp) const {
  return rtcp_receiver_.NTP(received_ntpsecs, received_ntpfrac,
                            rtcp_arrival_time_secs, rtcp_arrival_time_frac,
                            rtcp_timestamp)
             ? 0
             : -1;
}

// Get RoundTripTime.
int32_t ModuleRtpRtcpImpl::RTT(const uint32_t remote_ssrc,
                               int64_t* rtt,
                               int64_t* avg_rtt,
                               int64_t* min_rtt,
                               int64_t* max_rtt) const {
  int32_t ret = rtcp_receiver_.RTT(remote_ssrc, rtt, avg_rtt, min_rtt, max_rtt);
  if (rtt && *rtt == 0) {
    // Try to get RTT from RtcpRttStats class.
    *rtt = rtt_ms();
  }
  return ret;
}

int64_t ModuleRtpRtcpImpl::ExpectedRetransmissionTimeMs() const 
{
  int64_t expected_retransmission_time_ms = rtt_ms();
  if (expected_retransmission_time_ms > 0) 
  {
    return expected_retransmission_time_ms;
  }
  // No rtt available (|kRtpRtcpRttProcessTimeMs| not yet passed?), so try to
  // poll avg_rtt_ms directly from rtcp receiver.
  if (rtcp_receiver_.RTT(rtcp_receiver_.RemoteSSRC(), nullptr,
                         &expected_retransmission_time_ms, nullptr,
                         nullptr) == 0) 
  {
    return expected_retransmission_time_ms;
  }
  return kDefaultExpectedRetransmissionTimeMs;
}

// Force a send of an RTCP packet.
// Normal SR and RR are triggered via the process function.
int32_t ModuleRtpRtcpImpl::SendRTCP(RTCPPacketType packet_type) {
  return rtcp_sender_.SendRTCP(GetFeedbackState(), packet_type);
}

// Force a send of an RTCP packet.
// Normal SR and RR are triggered via the process function.
int32_t ModuleRtpRtcpImpl::SendCompoundRTCP(
    const std::set<RTCPPacketType>& packet_types) {
  return rtcp_sender_.SendCompoundRTCP(GetFeedbackState(), packet_types);
}

int32_t ModuleRtpRtcpImpl::SetRTCPApplicationSpecificData(
    const uint8_t sub_type,
    const uint32_t name,
    const uint8_t* data,
    const uint16_t length) {
  return rtcp_sender_.SetApplicationSpecificData(sub_type, name, data, length);
}

void ModuleRtpRtcpImpl::SetRtcpXrRrtrStatus(bool enable) {
  rtcp_receiver_.SetRtcpXrRrtrStatus(enable);
  rtcp_sender_.SendRtcpXrReceiverReferenceTime(enable);
}

bool ModuleRtpRtcpImpl::RtcpXrRrtrStatus() const {
  return rtcp_sender_.RtcpXrReceiverReferenceTime();
}

// TODO(asapersson): Replace this method with the one below.
int32_t ModuleRtpRtcpImpl::DataCountersRTP(size_t* bytes_sent,
                                           uint32_t* packets_sent) const {
  StreamDataCounters rtp_stats;
  StreamDataCounters rtx_stats;
  rtp_sender_->GetDataCounters(&rtp_stats, &rtx_stats);

  if (bytes_sent) {
    *bytes_sent = rtp_stats.transmitted.payload_bytes +
                  rtp_stats.transmitted.padding_bytes +
                  rtp_stats.transmitted.header_bytes +
                  rtx_stats.transmitted.payload_bytes +
                  rtx_stats.transmitted.padding_bytes +
                  rtx_stats.transmitted.header_bytes;
  }
  if (packets_sent) {
    *packets_sent =
        rtp_stats.transmitted.packets + rtx_stats.transmitted.packets;
  }
  return 0;
}

void ModuleRtpRtcpImpl::GetSendStreamDataCounters(
    StreamDataCounters* rtp_counters,
    StreamDataCounters* rtx_counters) const {
  rtp_sender_->GetDataCounters(rtp_counters, rtx_counters);
}

void ModuleRtpRtcpImpl::GetRtpPacketLossStats(
    bool outgoing,
    uint32_t ssrc,
    struct RtpPacketLossStats* loss_stats) const {
  if (!loss_stats)
    return;
  const PacketLossStats* stats_source = NULL;
  if (outgoing) {
    if (SSRC() == ssrc) {
      stats_source = &send_loss_stats_;
    }
  } else {
    if (rtcp_receiver_.RemoteSSRC() == ssrc) {
      stats_source = &receive_loss_stats_;
    }
  }
  if (stats_source) {
    loss_stats->single_packet_loss_count = stats_source->GetSingleLossCount();
    loss_stats->multiple_packet_loss_event_count =
        stats_source->GetMultipleLossEventCount();
    loss_stats->multiple_packet_loss_packet_count =
        stats_source->GetMultipleLossPacketCount();
  }
}

// Received RTCP report.
int32_t ModuleRtpRtcpImpl::RemoteRTCPStat(
    std::vector<RTCPReportBlock>* receive_blocks) const {
  return rtcp_receiver_.StatisticsReceived(receive_blocks);
}

// (REMB) Receiver Estimated Max Bitrate.
void ModuleRtpRtcpImpl::SetRemb(int64_t bitrate_bps,
                                std::vector<uint32_t> ssrcs) {
  rtcp_sender_.SetRemb(bitrate_bps, std::move(ssrcs));
}

void ModuleRtpRtcpImpl::UnsetRemb() {
  rtcp_sender_.UnsetRemb();
}

void ModuleRtpRtcpImpl::SetExtmapAllowMixed(bool extmap_allow_mixed) {
  rtp_sender_->SetExtmapAllowMixed(extmap_allow_mixed);
}

int32_t ModuleRtpRtcpImpl::RegisterSendRtpHeaderExtension(
    const RTPExtensionType type,
    const uint8_t id) {
  return rtp_sender_->RegisterRtpHeaderExtension(type, id);
}

bool ModuleRtpRtcpImpl::RegisterRtpHeaderExtension(const std::string& uri,
                                                   int id) {
  return rtp_sender_->RegisterRtpHeaderExtension(uri, id);
}

int32_t ModuleRtpRtcpImpl::DeregisterSendRtpHeaderExtension(
    const RTPExtensionType type) {
  return rtp_sender_->DeregisterRtpHeaderExtension(type);
}

bool ModuleRtpRtcpImpl::HasBweExtensions() const {
  return rtp_sender_->IsRtpHeaderExtensionRegistered(
             kRtpExtensionTransportSequenceNumber) ||
         rtp_sender_->IsRtpHeaderExtensionRegistered(
             kRtpExtensionAbsoluteSendTime) ||
         rtp_sender_->IsRtpHeaderExtensionRegistered(
             kRtpExtensionTransmissionTimeOffset);
}

// (TMMBR) Temporary Max Media Bit Rate.
bool ModuleRtpRtcpImpl::TMMBR() const {
  return rtcp_sender_.TMMBR();
}

void ModuleRtpRtcpImpl::SetTMMBRStatus(const bool enable) {
  rtcp_sender_.SetTMMBRStatus(enable);
}

void ModuleRtpRtcpImpl::SetTmmbn(std::vector<rtcp::TmmbItem> bounding_set) {
  rtcp_sender_.SetTmmbn(std::move(bounding_set));
}

// Send a Negative acknowledgment packet.
int32_t ModuleRtpRtcpImpl::SendNACK(const uint16_t* nack_list,
                                    const uint16_t size) {
  for (int i = 0; i < size; ++i) {
    receive_loss_stats_.AddLostPacket(nack_list[i]);
  }
  uint16_t nack_length = size;
  uint16_t start_id = 0;
  int64_t now_ms = clock_->TimeInMilliseconds();
  if (TimeToSendFullNackList(now_ms)) {
    nack_last_time_sent_full_ms_ = now_ms;
  } else {
    // Only send extended list.
    if (nack_last_seq_number_sent_ == nack_list[size - 1]) {
      // Last sequence number is the same, do not send list.
      return 0;
    }
    // Send new sequence numbers.
    for (int i = 0; i < size; ++i) {
      if (nack_last_seq_number_sent_ == nack_list[i]) {
        start_id = i + 1;
        break;
      }
    }
    nack_length = size - start_id;
  }

  // Our RTCP NACK implementation is limited to kRtcpMaxNackFields sequence
  // numbers per RTCP packet.
  if (nack_length > kRtcpMaxNackFields) {
    nack_length = kRtcpMaxNackFields;
  }
  nack_last_seq_number_sent_ = nack_list[start_id + nack_length - 1];

  return rtcp_sender_.SendRTCP(GetFeedbackState(), kRtcpNack, nack_length,
                               &nack_list[start_id]);
}

void ModuleRtpRtcpImpl::SendNack(const std::vector<uint16_t>& sequence_numbers) 
{
	// TODO@chensong 2023-03-30 发送掉包nack包表给对端发送
  rtcp_sender_.SendRTCP(GetFeedbackState(), kRtcpNack, sequence_numbers.size(), sequence_numbers.data());
}

bool ModuleRtpRtcpImpl::TimeToSendFullNackList(int64_t now) const {
  // Use RTT from RtcpRttStats class if provided.
  int64_t rtt = rtt_ms();
  if (rtt == 0) {
    rtcp_receiver_.RTT(rtcp_receiver_.RemoteSSRC(), NULL, &rtt, NULL, NULL);
  }

  const int64_t kStartUpRttMs = 100;
  int64_t wait_time = 5 + ((rtt * 3) >> 1);  // 5 + RTT * 1.5.
  if (rtt == 0) {
    wait_time = kStartUpRttMs;
  }

  // Send a full NACK list once within every |wait_time|.
  return now - nack_last_time_sent_full_ms_ > wait_time;
}

// Store the sent packets, needed to answer to Negative acknowledgment requests.
void ModuleRtpRtcpImpl::SetStorePacketsStatus(const bool enable,
                                              const uint16_t number_to_store) {
  rtp_sender_->SetStorePacketsStatus(enable, number_to_store);
}

bool ModuleRtpRtcpImpl::StorePackets() const {
  return rtp_sender_->StorePackets();
}

void ModuleRtpRtcpImpl::RegisterRtcpStatisticsCallback(
    RtcpStatisticsCallback* callback) {
  rtcp_receiver_.RegisterRtcpStatisticsCallback(callback);
}

RtcpStatisticsCallback* ModuleRtpRtcpImpl::GetRtcpStatisticsCallback() {
  return rtcp_receiver_.GetRtcpStatisticsCallback();
}

bool ModuleRtpRtcpImpl::SendFeedbackPacket(
    const rtcp::TransportFeedback& packet) {
  return rtcp_sender_.SendFeedbackPacket(packet);
}

int32_t ModuleRtpRtcpImpl::SetKeyFrameRequestMethod(
    const KeyFrameRequestMethod method) {
  key_frame_req_method_ = method;
  return 0;
}

int32_t ModuleRtpRtcpImpl::RequestKeyFrame() {
  switch (key_frame_req_method_) {
    case kKeyFrameReqPliRtcp:
      return SendRTCP(kRtcpPli);
    case kKeyFrameReqFirRtcp:
      return SendRTCP(kRtcpFir);
  }
  return -1;
}

int32_t ModuleRtpRtcpImpl::SendLossNotification(uint16_t last_decoded_seq_num,
                                                uint16_t last_received_seq_num,
                                                bool decodability_flag) {
  return rtcp_sender_.SendLossNotification(
      GetFeedbackState(), last_decoded_seq_num, last_received_seq_num,
      decodability_flag);
}

void ModuleRtpRtcpImpl::SetRemoteSSRC(const uint32_t ssrc) {
  // Inform about the incoming SSRC.
  rtcp_sender_.SetRemoteSSRC(ssrc);
  rtcp_receiver_.SetRemoteSSRC(ssrc);
}

// TODO(nisse): Delete video_rate amd fec_rate arguments.
void ModuleRtpRtcpImpl::BitrateSent(uint32_t* total_rate,
                                    uint32_t* video_rate,
                                    uint32_t* fec_rate,
                                    uint32_t* nack_rate) const {
  *total_rate = rtp_sender_->BitrateSent();
  if (video_rate)
    *video_rate = 0;
  if (fec_rate)
    *fec_rate = 0;
  *nack_rate = rtp_sender_->NackOverheadRate();
}

void ModuleRtpRtcpImpl::OnRequestSendReport() {
  SendRTCP(kRtcpSr);
}

void ModuleRtpRtcpImpl::OnReceivedNack(const std::vector<uint16_t>& nack_sequence_numbers) 
{
  if (!rtp_sender_)
  {
    return;
  }
  // TODO@chensong 2022-12-20 发送包掉包的数据统计模块
  for (uint16_t nack_sequence_number : nack_sequence_numbers)
  {
    send_loss_stats_.AddLostPacket(nack_sequence_number);
  }
  // TODO@chensong 2022-12-20  判断是否符合重新发送seq的数据包
  if (!rtp_sender_->StorePackets() || nack_sequence_numbers.size() == 0) 
  {
    return;
  }
  // Use RTT from RtcpRttStats class if provided.
  int64_t rtt = rtt_ms();
  if (rtt == 0) 
  {
    rtcp_receiver_.RTT(rtcp_receiver_.RemoteSSRC(), NULL, &rtt, NULL, NULL);
  }
  // TODO@chensong 2022-12-20 重新发送包插入到发送队列中去  RTPSender->OnReceivedNack
  rtp_sender_->OnReceivedNack(nack_sequence_numbers, rtt);
}

void ModuleRtpRtcpImpl::OnReceivedRtcpReportBlocks(const ReportBlockList& report_blocks) 
{
	// TODO@chensong 2022-12-20 ack确认机制
  if (ack_observer_) 
  {
    uint32_t ssrc = SSRC();

    for (const RTCPReportBlock& report_block : report_blocks) 
	{
      if (ssrc == report_block.source_ssrc) 
	  {
        ack_observer_->OnReceivedAck(report_block.extended_highest_sequence_number);
      }
    }
  }
}

bool ModuleRtpRtcpImpl::LastReceivedNTP(
    uint32_t* rtcp_arrival_time_secs,  // When we got the last report.
    uint32_t* rtcp_arrival_time_frac,
    uint32_t* remote_sr) const {
  // Remote SR: NTP inside the last received (mid 16 bits from sec and frac).
  uint32_t ntp_secs = 0;
  uint32_t ntp_frac = 0;

  if (!rtcp_receiver_.NTP(&ntp_secs, &ntp_frac, rtcp_arrival_time_secs,
                          rtcp_arrival_time_frac, NULL)) {
    return false;
  }
  *remote_sr =
      ((ntp_secs & 0x0000ffff) << 16) + ((ntp_frac & 0xffff0000) >> 16);
  return true;
}

// Called from RTCPsender.
std::vector<rtcp::TmmbItem> ModuleRtpRtcpImpl::BoundingSet(bool* tmmbr_owner) {
  return rtcp_receiver_.BoundingSet(tmmbr_owner);
}

void ModuleRtpRtcpImpl::SetRtcpReceiverSsrcs(uint32_t main_ssrc) {
  std::set<uint32_t> ssrcs;
  ssrcs.insert(main_ssrc);
  if (RtxSendStatus() != kRtxOff)
    ssrcs.insert(rtp_sender_->RtxSsrc());
  absl::optional<uint32_t> flexfec_ssrc = FlexfecSsrc();
  if (flexfec_ssrc)
    ssrcs.insert(*flexfec_ssrc);
  rtcp_receiver_.SetSsrcs(main_ssrc, ssrcs);
}

void ModuleRtpRtcpImpl::set_rtt_ms(int64_t rtt_ms) {
  rtc::CritScope cs(&critical_section_rtt_);
  rtt_ms_ = rtt_ms;
  if (rtp_sender_)
    rtp_sender_->SetRtt(rtt_ms);
}

int64_t ModuleRtpRtcpImpl::rtt_ms() const {
  rtc::CritScope cs(&critical_section_rtt_);
  return rtt_ms_;
}

void ModuleRtpRtcpImpl::RegisterSendChannelRtpStatisticsCallback(
    StreamDataCountersCallback* callback) {
  rtp_sender_->RegisterRtpStatisticsCallback(callback);
}

StreamDataCountersCallback*
ModuleRtpRtcpImpl::GetSendChannelRtpStatisticsCallback() const {
  return rtp_sender_->GetRtpStatisticsCallback();
}

void ModuleRtpRtcpImpl::SetVideoBitrateAllocation(
    const VideoBitrateAllocation& bitrate) {
  rtcp_sender_.SetVideoBitrateAllocation(bitrate);
}

RTPSender* ModuleRtpRtcpImpl::RtpSender() {
  return rtp_sender_.get();
}

const RTPSender* ModuleRtpRtcpImpl::RtpSender() const {
  return rtp_sender_.get();
}

}  // namespace webrtc
