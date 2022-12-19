/*
 *  Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <algorithm>

#include "modules/pacing/interval_budget.h"
#include "rtc_base/numerics/safe_conversions.h"

namespace webrtc {
namespace {
/*
����Ĵ���Ŀǰ��Ҫ����������can_build_up_underuse_���ش��£�build_up�����ޡ�
���ǵ��������������������ʱ�����ţ�����ÿ5ms����5ms��budget�ɹ�ʹ�ã����ǲ�����ÿһ��5ms���Ƕ��ܹ���ȫʹ�õ���5ms��budget���������underuse��
��ˣ�can_build_up_underuse_�����������ǽ���Щû�������Ԥ���ۼ��������Թ�����ʹ�á�kWindowMs = 500ms��ζ�����ǿ����ۻ�500ms��ô��û�������Ԥ�㡣

��������غô�����ĳЩʱ�̣����Ƕ�ʱ���ڿɹ����͵�Ԥ����࣬�����ʶ����ϴ��ʱ�����ǿ��Ը���ؽ����ݷ��ͳ�ȥ��
������ȱ���ǣ���ʱ���ڵ����ʿ��Ʋ���ƽ������һЩ�ʹ�����Ӱ�����
*/
constexpr int kWindowMs = 500;
}

IntervalBudget::IntervalBudget(int initial_target_rate_kbps)
    : IntervalBudget(initial_target_rate_kbps, false) {}

IntervalBudget::IntervalBudget(int initial_target_rate_kbps, bool can_build_up_underuse)
    : bytes_remaining_(0)
	, can_build_up_underuse_(can_build_up_underuse) 
{
  set_target_rate_kbps(initial_target_rate_kbps);
}

void IntervalBudget::set_target_rate_kbps(int target_rate_kbps) 
{
  target_rate_kbps_ = target_rate_kbps;
  max_bytes_in_budget_ = (kWindowMs * target_rate_kbps_) / 8;
  bytes_remaining_ = std::min(std::max(-max_bytes_in_budget_, bytes_remaining_), max_bytes_in_budget_);
}

void IntervalBudget::IncreaseBudget(int64_t delta_time_ms) 
{
  // һ����˵��can_build_up_underuse_ ����رգ�����������صĽ��ܼ����һ���ֽ���
  int bytes = rtc::dchecked_cast<int>(target_rate_kbps_ * delta_time_ms / 8);
  if (bytes_remaining_ < 0 || can_build_up_underuse_) 
  {
    // We overused last interval, compensate this interval.
    // ����ϴη��͵Ĺ��ࣨbytes_remaining_ < 0������ô���η��͵������������
    // �������can_build_up_underuse_������������ۻ�֮ǰû�������Ԥ��
    bytes_remaining_ = std::min(bytes_remaining_ + bytes, max_bytes_in_budget_);
  }
  else 
  {
    // If we underused last interval we can't use it this interval.
    // 1�� ����ϴε�budgetû�����꣨bytes_remaining_ >
    // 0�������û������can_build_up_underuse_
    // ������ϴεĲ�����ֱ���������Ԥ�㣬��ʼ�µ�һ��

    // 2�� ���������can_build_up_underuse_��־������ζ��Ҫ�����ϴε�underuse��
    // ����ϴ�û�з����꣬�򱾴���Ҫ������������if�߼�
    bytes_remaining_ = std::min(bytes, max_bytes_in_budget_);
  }
}

void IntervalBudget::UseBudget(size_t bytes) 
{
  bytes_remaining_ = std::max(bytes_remaining_ - static_cast<int>(bytes), -max_bytes_in_budget_);
}

size_t IntervalBudget::bytes_remaining() const 
{
  return static_cast<size_t>(std::max(0, bytes_remaining_));
}

int IntervalBudget::budget_level_percent() const 
{
  if (max_bytes_in_budget_ == 0)
  {
    return 0;
  }
  return rtc::dchecked_cast<int>(int64_t{bytes_remaining_} * 100 / max_bytes_in_budget_);
}

int IntervalBudget::target_rate_kbps() const 
{
  return target_rate_kbps_;
}

}  // namespace webrtc
