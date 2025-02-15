﻿/*
 *  Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/congestion_controller/goog_cc/trendline_estimator.h"

#include <math.h>

#include <algorithm>

#include "absl/types/optional.h"
#include "modules/remote_bitrate_estimator/include/bwe_defines.h"
#include "modules/remote_bitrate_estimator/test/bwe_test_logging.h"
#include "rtc_base/checks.h"
#include "rtc_base/numerics/safe_minmax.h"

namespace webrtc {

namespace {
// TODO@chensong 2022-11-30 线性回归函数最小二乘法
/*
 TODO@chensong 2022-11-30 
 时间作为                     : x
 平滑延迟值smoothed_delay作为  : y


 x/y
*/
absl::optional<double> LinearFitSlope(const std::deque<std::pair<double, double>>& points) 
{
  RTC_DCHECK(points.size() >= 2);
  // Compute the "center of mass".
  double sum_x = 0;
  double sum_y = 0;
  for (const auto& point : points) 
  {
    sum_x += point.first;
    sum_y += point.second;
  }
  double x_avg = sum_x / points.size();
  double y_avg = sum_y / points.size();
  // TODO@chensong 2022-11-30 直线方程y=bx+a的斜率b按如下公式计算:
  // Compute the slope k = \sum (x_i-x_avg)(y_i-y_avg) / \sum (x_i-x_avg)^2
  double numerator = 0;
  double denominator = 0;
  for (const auto& point : points) 
  {
    numerator += (point.first - x_avg) * (point.second - y_avg);
    denominator += (point.first - x_avg) * (point.first - x_avg);
  }
  if (denominator == 0) 
  {
    return absl::nullopt;
  }
  return numerator / denominator;
}

constexpr double kMaxAdaptOffsetMs = 15.0;
constexpr double kOverUsingTimeThreshold = 10;
constexpr int kMinNumDeltas = 60;
constexpr int kDeltaCounterMax = 1000;

}  // namespace

TrendlineEstimator::TrendlineEstimator(
    size_t window_size,
    double smoothing_coef,
    double threshold_gain,
    NetworkStatePredictor* network_state_predictor)
    : window_size_(window_size),
      smoothing_coef_(smoothing_coef),
      threshold_gain_(threshold_gain),
      num_of_deltas_(0),
      first_arrival_time_ms_(-1),
      accumulated_delay_(0),
      smoothed_delay_(0),
      delay_hist_(),
      k_up_(0.0087),
      k_down_(0.039),
      overusing_time_threshold_(kOverUsingTimeThreshold),
      threshold_(12.5),
      prev_modified_trend_(NAN),
      last_update_ms_(-1),
      prev_trend_(0.0),
      time_over_using_(-1),
      overuse_counter_(0),
      hypothesis_(BandwidthUsage::kBwNormal),
      hypothesis_predicted_(BandwidthUsage::kBwNormal),
      network_state_predictor_(network_state_predictor) {}

TrendlineEstimator::~TrendlineEstimator() {}

void TrendlineEstimator::Update(double recv_delta_ms, double send_delta_ms, int64_t send_time_ms, int64_t arrival_time_ms, bool calculated_deltas) 
{
  if (calculated_deltas) 
  {
    const double delta_ms = recv_delta_ms - send_delta_ms;
    ++num_of_deltas_;
    num_of_deltas_ = std::min(num_of_deltas_, kDeltaCounterMax /* 1000 */);
    if (first_arrival_time_ms_ == -1) 
	{
      first_arrival_time_ms_ = arrival_time_ms;
    }

    // Exponential backoff filter.
    accumulated_delay_ += delta_ms;
    BWE_TEST_LOGGING_PLOT(1, "accumulated_delay_ms", arrival_time_ms, accumulated_delay_);
    /*
    TODO@chensong 2022-11-30
        到达时间滤波器(arrival-time filter)
   为减少网络波动影响，使用中会将最近1000个
   包组传输时延进行叠加，计算出一个平滑延迟值 smoothed_delay。 WebRTC
   使用了线性回归进行时延梯度趋势预测，通过最小二乘法求拟合直线的斜率，根据斜率判断增长趋势
   
   平滑延迟公式 = 平滑系数 * 平滑延迟 + (1 - 平滑系数) * 累积的延迟
   */
    smoothed_delay_ = smoothing_coef_ * smoothed_delay_ + (1 - smoothing_coef_ /*0.9*/) * accumulated_delay_;
    BWE_TEST_LOGGING_PLOT(1, "smoothed_delay_ms", arrival_time_ms,
                          smoothed_delay_);

    // Simple linear regression. ==>>> 简单线性回归
    delay_hist_.push_back(std::make_pair(static_cast<double>(arrival_time_ms - first_arrival_time_ms_), smoothed_delay_));
    if (delay_hist_.size() > window_size_)
	{
      delay_hist_.pop_front();
    }
    double trend = prev_trend_;
    if (delay_hist_.size() == window_size_ /*20*/) 
	{
      // Update trend_ if it is possible to fit a line to the data. The delay
      // trend can be seen as an estimate of (send_rate - capacity)/capacity.
      // 0 < trend < 1   ->  the delay increases, queues are filling up		==> 1、延时增大，路由buffer 正在被填充。
      //   trend == 0    ->  the delay does not change						==> 2、延时没有发生变化。
      //   trend < 0     ->  the delay decreases, queues are being emptied	==> 3、延时开始降低，路由buffer正在排空。
      trend = LinearFitSlope(delay_hist_).value_or(trend);
    }

    BWE_TEST_LOGGING_PLOT(1, "trendline_slope", arrival_time_ms, trend);

    Detect(trend, send_delta_ms, arrival_time_ms);
  }
  if (network_state_predictor_)
  {
    hypothesis_predicted_ = network_state_predictor_->Update(send_time_ms, arrival_time_ms, hypothesis_);
  }
}

BandwidthUsage TrendlineEstimator::State() const {
  return network_state_predictor_ ? hypothesis_predicted_ : hypothesis_;
}

void TrendlineEstimator::Detect(double trend, double ts_delta, int64_t now_ms) 
{
  if (num_of_deltas_ < 2) 
  {
    hypothesis_ = BandwidthUsage::kBwNormal;
    return;
  }
  //TODO@chensong 2022-11-30 过载检测器(over-use detector)
  //实际使用中，由于trend 是一个非常小的值，会乘以包组数量和增益系数进行放大得到modified_trend
  const double modified_trend = std::min(num_of_deltas_, kMinNumDeltas) * trend * threshold_gain_ /*增益系数 =  4.0*/;
  prev_modified_trend_ = modified_trend;
  BWE_TEST_LOGGING_PLOT(1, "T", now_ms, modified_trend);
  BWE_TEST_LOGGING_PLOT(1, "threshold", now_ms, threshold_);
  if (modified_trend > threshold_) //持续时间超过100ms并且 trend值持续变大，认为此时处于 overuse 状态。
  {
    if (time_over_using_ == -1) 
	{
      // Initialize the timer. Assume that we've been
      // over-using half of the time since the previous
      // sample.
      time_over_using_ = ts_delta / 2;
    }
	else 
	{
      // Increment timer
      time_over_using_ += ts_delta;
    }
    overuse_counter_++;
    if (time_over_using_ > overusing_time_threshold_ && overuse_counter_ > 1) 
	{
      if (trend >= prev_trend_) 
	  {
        time_over_using_ = 0;
        overuse_counter_ = 0;
        hypothesis_ = BandwidthUsage::kBwOverusing;
      }
    }
  } 
  else if (modified_trend < -threshold_) //认为此时处于underuse 状态
  {
    time_over_using_ = -1;
    overuse_counter_ = 0;
    hypothesis_ = BandwidthUsage::kBwUnderusing;
  }
  else  //-threshold < modifed_trend < threshold  认为此时处于normal 状态。
  {
    time_over_using_ = -1;
    overuse_counter_ = 0;
    hypothesis_ = BandwidthUsage::kBwNormal;
  }
  prev_trend_ = trend;
  UpdateThreshold(modified_trend, now_ms);
}

void TrendlineEstimator::UpdateThreshold(double modified_trend, int64_t now_ms)
{
  if (last_update_ms_ == -1)
  {
    last_update_ms_ = now_ms;
  }

  if (fabs(modified_trend) > threshold_ + kMaxAdaptOffsetMs) 
  {
    // Avoid adapting the threshold to big latency spikes, caused e.g.,
    // by a sudden capacity drop.
    last_update_ms_ = now_ms;
    return;
  }

  const double k = fabs(modified_trend) < threshold_ ? k_down_ : k_up_;
  const int64_t kMaxTimeDeltaMs = 100;
  int64_t time_delta_ms = std::min(now_ms - last_update_ms_, kMaxTimeDeltaMs);
  threshold_ += k * (fabs(modified_trend) - threshold_) * time_delta_ms;
  threshold_ = rtc::SafeClamp(threshold_, 6.f, 600.f);
  last_update_ms_ = now_ms;
}

}  // namespace webrtc
