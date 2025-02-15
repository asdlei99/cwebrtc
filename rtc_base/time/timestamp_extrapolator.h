﻿/*
 *  Copyright (c) 2011 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.


 FrameBuffer每获得一个可解码帧，都要更新其渲染时间，渲染时间通过TimestampExtrapolator类获得。
 TimestampExtrapolator是一个卡尔曼滤波器，其输入为输入帧的RTP时间戳，
 TimestampExtrapolator会根据输入帧的RTP时间戳计算出该帧的期望接收时间，该时间是经过平滑的。

 视频帧的期望渲染时间 = 帧平滑时间(就是帧的期望接收时间) + 实际延迟(actual delay,
 由current_delay_ms_、min_playout_delay_ms_和max_playout_delay_ms_计算出来)。


 */

#ifndef RTC_BASE_TIME_TIMESTAMP_EXTRAPOLATOR_H_
#define RTC_BASE_TIME_TIMESTAMP_EXTRAPOLATOR_H_

#include <stdint.h>

#include "rtc_base/synchronization/rw_lock_wrapper.h"

namespace webrtc {

class TimestampExtrapolator {
 public:
  explicit TimestampExtrapolator(int64_t start_ms);
  ~TimestampExtrapolator();
  void Update(int64_t tMs, uint32_t ts90khz);
  int64_t ExtrapolateLocalTime(uint32_t timestamp90khz);
  void Reset(int64_t start_ms);

 private:
  void CheckForWrapArounds(uint32_t ts90khz);
  bool DelayChangeDetection(double error);
  RWLockWrapper* _rwLock;
  double _w[2];
  double _pP[2][2];
  int64_t _startMs;
  int64_t _prevMs;
  uint32_t _firstTimestamp;
  int32_t _wrapArounds;
  int64_t _prevUnwrappedTimestamp;
  int64_t _prevWrapTimestamp;
  const double _lambda;
  bool _firstAfterReset;
  uint32_t _packetCount;
  const uint32_t _startUpFilterDelayInPackets;

  double _detectorAccumulatorPos;
  double _detectorAccumulatorNeg;
  const double _alarmThreshold;
  const double _accDrift;
  const double _accMaxError;
  const double _pP11;
};

}  // namespace webrtc

#endif  // RTC_BASE_TIME_TIMESTAMP_EXTRAPOLATOR_H_
