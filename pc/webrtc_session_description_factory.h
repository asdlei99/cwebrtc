﻿/*
 *  Copyright 2013 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef PC_WEBRTC_SESSION_DESCRIPTION_FACTORY_H_
#define PC_WEBRTC_SESSION_DESCRIPTION_FACTORY_H_

#include <stdint.h>
#include <memory>
#include <queue>
#include <string>

#include "api/jsep.h"
#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"
#include "p2p/base/transport_description.h"
#include "p2p/base/transport_description_factory.h"
#include "pc/media_session.h"
#include "pc/peer_connection_internal.h"
#include "rtc_base/constructor_magic.h"
#include "rtc_base/message_handler.h"
#include "rtc_base/message_queue.h"
#include "rtc_base/rtc_certificate.h"
#include "rtc_base/rtc_certificate_generator.h"
#include "rtc_base/third_party/sigslot/sigslot.h"
#include "rtc_base/thread.h"
#include "rtc_base/unique_id_generator.h"

namespace webrtc {

// DTLS certificate request callback class.
class WebRtcCertificateGeneratorCallback
    : public rtc::RTCCertificateGeneratorCallback,
      public sigslot::has_slots<> {
 public:
  // |rtc::RTCCertificateGeneratorCallback| overrides.
  void OnSuccess(
      const rtc::scoped_refptr<rtc::RTCCertificate>& certificate) override;
  void OnFailure() override;

  sigslot::signal0<> SignalRequestFailed; //证书创建失败回调函数
  sigslot::signal1<const rtc::scoped_refptr<rtc::RTCCertificate>&>
      SignalCertificateReady; ////证书创建成功回调函数
};

struct CreateSessionDescriptionRequest {
  enum Type {
    kOffer,
    kAnswer,
  };

  CreateSessionDescriptionRequest(Type type,
                                  CreateSessionDescriptionObserver* observer,
                                  const cricket::MediaSessionOptions& options)
      : type(type), observer(observer), options(options) {}

  Type type;
  rtc::scoped_refptr<CreateSessionDescriptionObserver> observer;
  cricket::MediaSessionOptions options;
};

// This class is used to create offer/answer session description. Certificates
// for WebRtcSession/DTLS are either supplied at construction or generated
// asynchronously. It queues the create offer/answer request until the
// certificate generation has completed, i.e. when OnCertificateRequestFailed or
// OnCertificateReady is called.
class WebRtcSessionDescriptionFactory : public rtc::MessageHandler,
                                        public sigslot::has_slots<> {
 public:
  // Can specify either a |cert_generator| or |certificate| to enable DTLS. If
  // a certificate generator is given, starts generating the certificate
  // asynchronously. If a certificate is given, will use that for identifying
  // over DTLS. If neither is specified, DTLS is disabled.
  WebRtcSessionDescriptionFactory(
      rtc::Thread* signaling_thread,
      cricket::ChannelManager* channel_manager,
      PeerConnectionInternal* pc,
      const std::string& session_id,
      std::unique_ptr<rtc::RTCCertificateGeneratorInterface> cert_generator,
      const rtc::scoped_refptr<rtc::RTCCertificate>& certificate,
      rtc::UniqueRandomIdGenerator* ssrc_generator);
  virtual ~WebRtcSessionDescriptionFactory();

  static void CopyCandidatesFromSessionDescription(
      const SessionDescriptionInterface* source_desc,
      const std::string& content_name,
      SessionDescriptionInterface* dest_desc);

  void CreateOffer(
      CreateSessionDescriptionObserver* observer,
      const PeerConnectionInterface::RTCOfferAnswerOptions& options,
      const cricket::MediaSessionOptions& session_options);
  void CreateAnswer(CreateSessionDescriptionObserver* observer,
                    const cricket::MediaSessionOptions& session_options);

  void SetSdesPolicy(cricket::SecurePolicy secure_policy);
  cricket::SecurePolicy SdesPolicy() const;

  void set_enable_encrypted_rtp_header_extensions(bool enable) {
    session_desc_factory_.set_enable_encrypted_rtp_header_extensions(enable);
  }

  void set_is_unified_plan(bool is_unified_plan) {
    session_desc_factory_.set_is_unified_plan(is_unified_plan);
  }

  sigslot::signal1<const rtc::scoped_refptr<rtc::RTCCertificate>&> SignalCertificateReady;

  // For testing.
  bool waiting_for_certificate_for_testing() const {
    return certificate_request_state_ == CERTIFICATE_WAITING;
  }

 private:
  enum CertificateRequestState {
    CERTIFICATE_NOT_NEEDED,
    CERTIFICATE_WAITING,
    CERTIFICATE_SUCCEEDED,
    CERTIFICATE_FAILED,
  };

  // MessageHandler implementation.
  virtual void OnMessage(rtc::Message* msg);

  void InternalCreateOffer(CreateSessionDescriptionRequest request);
  void InternalCreateAnswer(CreateSessionDescriptionRequest request);
  // Posts failure notifications for all pending session description requests.
  void FailPendingRequests(const std::string& reason);
  void PostCreateSessionDescriptionFailed(
      CreateSessionDescriptionObserver* observer,
      const std::string& error);
  void PostCreateSessionDescriptionSucceeded(
      CreateSessionDescriptionObserver* observer,
      std::unique_ptr<SessionDescriptionInterface> description);

  void OnCertificateRequestFailed();
  void SetCertificate(const rtc::scoped_refptr<rtc::RTCCertificate>& certificate);

  std::queue<CreateSessionDescriptionRequest> create_session_description_requests_;
  rtc::Thread* const signaling_thread_;
  cricket::TransportDescriptionFactory transport_desc_factory_;
  cricket::MediaSessionDescriptionFactory session_desc_factory_;
  uint64_t session_version_;
  const std::unique_ptr<rtc::RTCCertificateGeneratorInterface> cert_generator_;
  // TODO(jiayl): remove the dependency on peer connection once bug 2264 is
  // fixed.
  PeerConnectionInternal* const pc_;
  const std::string session_id_;
  CertificateRequestState certificate_request_state_;

  RTC_DISALLOW_COPY_AND_ASSIGN(WebRtcSessionDescriptionFactory);
};
}  // namespace webrtc

#endif  // PC_WEBRTC_SESSION_DESCRIPTION_FACTORY_H_
