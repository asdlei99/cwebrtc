﻿/*
 *  Copyright 2017 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "pc/rtp_transport.h"

#include <errno.h>
#include <string>
#include <utility>

#include "api/rtp_headers.h"
#include "api/rtp_parameters.h"
#include "media/base/rtp_utils.h"
#include "modules/rtp_rtcp/source/rtp_packet_received.h"
#include "rtc_base/checks.h"
#include "rtc_base/copy_on_write_buffer.h"
#include "rtc_base/logging.h"
#include "rtc_base/third_party/sigslot/sigslot.h"
#include "rtc_base/trace_event.h"

namespace webrtc {

////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////      TODO@chensong  2022-11-29
//
//#if 0
//
//static FILE* out_rtc_rtp_transport_ptr = NULL;
//static void rtc_turn_port_log() {
//  if (!out_rtc_rtp_transport_ptr) {
//    out_rtc_rtp_transport_ptr = ::fopen("./debug/rtp_transport.log", "wb+");
//  }
//}
//
//#define RTC_NORMAL_LOG(format, ...)                              \
//  rtc_turn_port_log();                                       \
//  if (out_rtc_rtp_transport_ptr)	{ 	 			\
//  fprintf(out_rtc_rtp_transport_ptr, format, ##__VA_ARGS__); \
//  fprintf(out_rtc_rtp_transport_ptr, "\n");                  \
//  fflush(out_rtc_rtp_transport_ptr); }
//
//#define RTC_NORMAL_EX_LOG(format, ...) \
//  RTC_NORMAL_LOG("[%s][%d][info]" format, __FUNCTION__, __LINE__, ##__VA_ARGS__)
//
//#endif  // _DEBUG

void RtpTransport::SetRtcpMuxEnabled(bool enable) {
  rtcp_mux_enabled_ = enable;
  MaybeSignalReadyToSend();
}

void RtpTransport::SetRtpPacketTransport(
    rtc::PacketTransportInternal* new_packet_transport) {
  if (new_packet_transport == rtp_packet_transport_) {
    return;
  }
  if (rtp_packet_transport_) {
    rtp_packet_transport_->SignalReadyToSend.disconnect(this);
    rtp_packet_transport_->SignalReadPacket.disconnect(this);
    rtp_packet_transport_->SignalNetworkRouteChanged.disconnect(this);
    rtp_packet_transport_->SignalWritableState.disconnect(this);
    rtp_packet_transport_->SignalSentPacket.disconnect(this);
    // Reset the network route of the old transport.
    SignalNetworkRouteChanged(absl::optional<rtc::NetworkRoute>());
  }
  if (new_packet_transport) {
    new_packet_transport->SignalReadyToSend.connect(
        this, &RtpTransport::OnReadyToSend);
    new_packet_transport->SignalReadPacket.connect(this,
                                                   &RtpTransport::OnReadPacket);
    new_packet_transport->SignalNetworkRouteChanged.connect(
        this, &RtpTransport::OnNetworkRouteChanged);
    new_packet_transport->SignalWritableState.connect(
        this, &RtpTransport::OnWritableState);
    new_packet_transport->SignalSentPacket.connect(this,
                                                   &RtpTransport::OnSentPacket);
    // Set the network route for the new transport.
    SignalNetworkRouteChanged(new_packet_transport->network_route());
  }

  rtp_packet_transport_ = new_packet_transport;
  // Assumes the transport is ready to send if it is writable. If we are wrong,
  // ready to send will be updated the next time we try to send.
  SetReadyToSend(false,
                 rtp_packet_transport_ && rtp_packet_transport_->writable());
}

void RtpTransport::SetRtcpPacketTransport(
    rtc::PacketTransportInternal* new_packet_transport) {
  if (new_packet_transport == rtcp_packet_transport_) {
    return;
  }
  if (rtcp_packet_transport_) {
    rtcp_packet_transport_->SignalReadyToSend.disconnect(this);
    rtcp_packet_transport_->SignalReadPacket.disconnect(this);
    rtcp_packet_transport_->SignalNetworkRouteChanged.disconnect(this);
    rtcp_packet_transport_->SignalWritableState.disconnect(this);
    rtcp_packet_transport_->SignalSentPacket.disconnect(this);
    // Reset the network route of the old transport.
    SignalNetworkRouteChanged(absl::optional<rtc::NetworkRoute>());
  }
  if (new_packet_transport) {
    new_packet_transport->SignalReadyToSend.connect(
        this, &RtpTransport::OnReadyToSend);
    new_packet_transport->SignalReadPacket.connect(this,
                                                   &RtpTransport::OnReadPacket);
    new_packet_transport->SignalNetworkRouteChanged.connect(
        this, &RtpTransport::OnNetworkRouteChanged);
    new_packet_transport->SignalWritableState.connect(
        this, &RtpTransport::OnWritableState);
    new_packet_transport->SignalSentPacket.connect(this,
                                                   &RtpTransport::OnSentPacket);
    // Set the network route for the new transport.
    SignalNetworkRouteChanged(new_packet_transport->network_route());
  }
  rtcp_packet_transport_ = new_packet_transport;

  // Assumes the transport is ready to send if it is writable. If we are wrong,
  // ready to send will be updated the next time we try to send.
  SetReadyToSend(true,
                 rtcp_packet_transport_ && rtcp_packet_transport_->writable());
}

bool RtpTransport::IsWritable(bool rtcp) const {
  rtc::PacketTransportInternal* transport = rtcp && !rtcp_mux_enabled_
                                                ? rtcp_packet_transport_
                                                : rtp_packet_transport_;
  return transport && transport->writable();
}

bool RtpTransport::SendRtpPacket(rtc::CopyOnWriteBuffer* packet,
                                 const rtc::PacketOptions& options,
                                 int flags) {
  return SendPacket(false, packet, options, flags);
}

bool RtpTransport::SendRtcpPacket(rtc::CopyOnWriteBuffer* packet,
                                  const rtc::PacketOptions& options,
                                  int flags) {
  return SendPacket(true, packet, options, flags);
}

bool RtpTransport::SendPacket(bool rtcp,
                              rtc::CopyOnWriteBuffer* packet,
                              const rtc::PacketOptions& options,
                              int flags) {
  rtc::PacketTransportInternal* transport = rtcp && !rtcp_mux_enabled_
                                                ? rtcp_packet_transport_
                                                : rtp_packet_transport_;
#if 0

	RTC_NORMAL_EX_LOG("[transport->SendPacket] [options = %s]",
                webrtc::ToString(options).c_str());
#endif

  int ret = transport->SendPacket(packet->cdata<char>(), packet->size(),
                                  options, flags);
  if (ret != static_cast<int>(packet->size())) {
    if (transport->GetError() == ENOTCONN) {
      RTC_LOG(LS_WARNING) << "Got ENOTCONN from transport.";
      SetReadyToSend(rtcp, false);
    }
    return false;
  }
  return true;
}

void RtpTransport::UpdateRtpHeaderExtensionMap(
    const cricket::RtpHeaderExtensions& header_extensions) {
  header_extension_map_ = RtpHeaderExtensionMap(header_extensions);
}

bool RtpTransport::RegisterRtpDemuxerSink(const RtpDemuxerCriteria& criteria,
                                          RtpPacketSinkInterface* sink) {
  rtp_demuxer_.RemoveSink(sink);
  if (!rtp_demuxer_.AddSink(criteria, sink)) {
    RTC_LOG(LS_ERROR) << "Failed to register the sink for RTP demuxer.";
    return false;
  }
  return true;
}

bool RtpTransport::UnregisterRtpDemuxerSink(RtpPacketSinkInterface* sink) {
  if (!rtp_demuxer_.RemoveSink(sink)) {
    RTC_LOG(LS_ERROR) << "Failed to unregister the sink for RTP demuxer.";
    return false;
  }
  return true;
}

RTCError RtpTransport::SetParameters(const RtpTransportParameters& parameters) {
  if (parameters_.rtcp.mux && !parameters.rtcp.mux) {
    LOG_AND_RETURN_ERROR(RTCErrorType::INVALID_STATE,
                         "Disabling RTCP muxing is not allowed.");
  }

  RtpTransportParameters new_parameters = parameters;

  if (new_parameters.rtcp.cname.empty()) {
    new_parameters.rtcp.cname = parameters_.rtcp.cname;
  }

  parameters_ = new_parameters;
  return RTCError::OK();
}

RtpTransportParameters RtpTransport::GetParameters() const {
  return parameters_;
}

void RtpTransport::DemuxPacket(rtc::CopyOnWriteBuffer packet,
                               int64_t packet_time_us) {
  webrtc::RtpPacketReceived parsed_packet(&header_extension_map_);
  if (!parsed_packet.Parse(std::move(packet))) {
    RTC_LOG(LS_ERROR)
        << "Failed to parse the incoming RTP packet before demuxing. Drop it.";
    return;
  }

  if (packet_time_us != -1) 
  {
	  // TODO@chensong 2022-12-04   packet_time_us 是当前读取网络数据的毫秒数   ， 这边增加500毫秒数 是什么？？？
    parsed_packet.set_arrival_time_ms((packet_time_us + 500) / 1000);
  }
  rtp_demuxer_.OnRtpPacket(parsed_packet);
}

bool RtpTransport::IsTransportWritable() {
  auto rtcp_packet_transport =
      rtcp_mux_enabled_ ? nullptr : rtcp_packet_transport_;
  return rtp_packet_transport_ && rtp_packet_transport_->writable() &&
         (!rtcp_packet_transport || rtcp_packet_transport->writable());
}

void RtpTransport::OnReadyToSend(rtc::PacketTransportInternal* transport) {
  SetReadyToSend(transport == rtcp_packet_transport_, true);
}

void RtpTransport::OnNetworkRouteChanged(
    absl::optional<rtc::NetworkRoute> network_route) {
  SignalNetworkRouteChanged(network_route);
}

void RtpTransport::OnWritableState(
    rtc::PacketTransportInternal* packet_transport) {
  RTC_DCHECK(packet_transport == rtp_packet_transport_ ||
             packet_transport == rtcp_packet_transport_);
  SignalWritableState(IsTransportWritable());
}

void RtpTransport::OnSentPacket(rtc::PacketTransportInternal* packet_transport,
                                const rtc::SentPacket& sent_packet) {
  RTC_DCHECK(packet_transport == rtp_packet_transport_ ||
             packet_transport == rtcp_packet_transport_);

#if 0

  RTC_NORMAL_EX_LOG("[SignalSentPacket][sent_packet = %s]",
                webrtc::ToString(sent_packet).c_str());
#endif  // _DEBUG

  SignalSentPacket(sent_packet);
}

void RtpTransport::OnRtpPacketReceived(rtc::CopyOnWriteBuffer packet,
                                       int64_t packet_time_us) {
  DemuxPacket(packet, packet_time_us);
}

void RtpTransport::OnRtcpPacketReceived(rtc::CopyOnWriteBuffer packet,
                                        int64_t packet_time_us) {
  SignalRtcpPacketReceived(&packet, packet_time_us);
}
/* 
TODO@chensong 2023-04-08 网络数据从底层向上调用流程
[rtc_base/async_udp_socket.cc]	   AsyncUDPSocket::OnReadEvent
[p2p\client/basic_port_allocator.cc]	   AllocationSequence::OnReadPacket
[p2p/base/stun_port.cc]							UDPPort::HandleIncomingPacket
[p2p/base/stun_port.cc]										UDPPort::OnReadPacket
[p2p/base/port.cc]											Connection::OnReadPackets
[p2p/base/p2p_transport_channel.cc]						P2PTransportChannel::OnReadPacket
[pc/rtp_transprot.cc]                                            RtpTransport::OnReadPacket
*/
void RtpTransport::OnReadPacket(rtc::PacketTransportInternal* transport,
                                const char* data,
                                size_t len,
                                const int64_t& packet_time_us,
                                int flags) {
  TRACE_EVENT0("webrtc", "RtpTransport::OnReadPacket");
  // TODO@chensong 2022-10-19   rtp-RTCP   data
  // When using RTCP multiplexing we might get RTCP packets on the RTP
  // transport. We check the RTP payload type to determine if it is RTCP.
  auto array_view = rtc::MakeArrayView(data, len);
  cricket::RtpPacketType packet_type = cricket::InferRtpPacketType(array_view);
  // Filter out the packet that is neither RTP nor RTCP.
  if (packet_type == cricket::RtpPacketType::kUnknown) {
    return;
  }

  // Protect ourselves against crazy data.
  if (!cricket::IsValidRtpPacketSize(packet_type, len)) {
    RTC_LOG(LS_ERROR) << "Dropping incoming "
                      << cricket::RtpPacketTypeToString(packet_type)
                      << " packet: wrong size=" << len;
    return;
  }

  rtc::CopyOnWriteBuffer packet(data, len);
  if (packet_type == cricket::RtpPacketType::kRtcp) {
    OnRtcpPacketReceived(std::move(packet), packet_time_us);
  } else {
    OnRtpPacketReceived(std::move(packet), packet_time_us);
  }
}

void RtpTransport::SetReadyToSend(bool rtcp, bool ready) {
  if (rtcp) {
    rtcp_ready_to_send_ = ready;
  } else {
    rtp_ready_to_send_ = ready;
  }

  MaybeSignalReadyToSend();
}

void RtpTransport::MaybeSignalReadyToSend() {
  bool ready_to_send =
      rtp_ready_to_send_ && (rtcp_ready_to_send_ || rtcp_mux_enabled_);
  if (ready_to_send != ready_to_send_) {
    ready_to_send_ = ready_to_send;
    SignalReadyToSend(ready_to_send);
  }
}

}  // namespace webrtc
