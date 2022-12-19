/*
 *  Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef MODULES_PACING_INTERVAL_BUDGET_H_
#define MODULES_PACING_INTERVAL_BUDGET_H_

#include <stddef.h>
#include <stdint.h>

namespace webrtc {

// TODO(tschumim): Reflector IntervalBudget so that we can set a under- and
// over-use budget in ms.
class IntervalBudget {
 public:
  explicit IntervalBudget(int initial_target_rate_kbps);
  IntervalBudget(int initial_target_rate_kbps, bool can_build_up_underuse);
  // ����Ŀ�귢������
  void set_target_rate_kbps(int target_rate_kbps);

  // TODO(tschumim): Unify IncreaseBudget and UseBudget to one function.
  // ʱ�����ź�����budget
  void IncreaseBudget(int64_t delta_time_ms);
  // �������ݺ����budget
  void UseBudget(size_t bytes);
  // ʣ��budget
  size_t bytes_remaining() const;
  // ʣ��budgetռ��ǰ��������������
  int budget_level_percent() const;
  // Ŀ�귢������
  int target_rate_kbps() const;

 private:
  // ���õ�Ŀ�����ʣ�����������ʿ������ݷ���
  int target_rate_kbps_;
  // �����ڣ�500ms����Ӧ������ֽ���=���ڴ�С*target_rate_kbps_/8
  int max_bytes_in_budget_;
  // ʣ��ɷ����ֽ��������Ʒ�Χ:[-max_bytes_in_budget_, max_bytes_in_budget_]
  int bytes_remaining_;
  // �ϸ�����underuse���������Ƿ���Խ����ϸ����ڵ�ʣ����
  bool can_build_up_underuse_;
};

}  // namespace webrtc

#endif  // MODULES_PACING_INTERVAL_BUDGET_H_
