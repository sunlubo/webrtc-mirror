/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 *
 */

#include "webrtc/modules/bitrate_controller/bitrate_controller_impl.h"

#include <algorithm>
#include <utility>

#include "webrtc/modules/rtp_rtcp/interface/rtp_rtcp_defines.h"

namespace webrtc {

class BitrateControllerImpl::RtcpBandwidthObserverImpl
    : public RtcpBandwidthObserver {
 public:
  explicit RtcpBandwidthObserverImpl(BitrateControllerImpl* owner)
      : owner_(owner) {
  }
  virtual ~RtcpBandwidthObserverImpl() {
  }
  // Received RTCP REMB or TMMBR.
  virtual void OnReceivedEstimatedBitrate(uint32_t bitrate) OVERRIDE {
    owner_->OnReceivedEstimatedBitrate(bitrate);
  }
  // Received RTCP receiver block.
  virtual void OnReceivedRtcpReceiverReport(
      const ReportBlockList& report_blocks,
      int64_t rtt,
      int64_t now_ms) OVERRIDE {
    if (report_blocks.empty())
      return;

    int fraction_lost_aggregate = 0;
    int total_number_of_packets = 0;

    // Compute the a weighted average of the fraction loss from all report
    // blocks.
    for (ReportBlockList::const_iterator it = report_blocks.begin();
        it != report_blocks.end(); ++it) {
      std::map<uint32_t, uint32_t>::iterator seq_num_it =
          ssrc_to_last_received_extended_high_seq_num_.find(it->sourceSSRC);

      int number_of_packets = 0;
      if (seq_num_it != ssrc_to_last_received_extended_high_seq_num_.end())
        number_of_packets = it->extendedHighSeqNum -
            seq_num_it->second;

      fraction_lost_aggregate += number_of_packets * it->fractionLost;
      total_number_of_packets += number_of_packets;

      // Update last received for this SSRC.
      ssrc_to_last_received_extended_high_seq_num_[it->sourceSSRC] =
          it->extendedHighSeqNum;
    }
    if (total_number_of_packets == 0)
      fraction_lost_aggregate = 0;
    else
      fraction_lost_aggregate  = (fraction_lost_aggregate +
          total_number_of_packets / 2) / total_number_of_packets;
    if (fraction_lost_aggregate > 255)
      return;

    owner_->OnReceivedRtcpReceiverReport(fraction_lost_aggregate, rtt,
                                         total_number_of_packets, now_ms);
  }

 private:
  std::map<uint32_t, uint32_t> ssrc_to_last_received_extended_high_seq_num_;
  BitrateControllerImpl* owner_;
};

BitrateController* BitrateController::CreateBitrateController(
    Clock* clock,
    BitrateObserver* observer) {
  return new BitrateControllerImpl(clock, observer);
}

BitrateControllerImpl::BitrateControllerImpl(Clock* clock,
                                             BitrateObserver* observer)
    : clock_(clock),
      observer_(observer),
      last_bitrate_update_ms_(clock_->TimeInMilliseconds()),
      critsect_(CriticalSectionWrapper::CreateCriticalSection()),
      bandwidth_estimation_(),
      reserved_bitrate_bps_(0),
      last_bitrate_bps_(0),
      last_fraction_loss_(0),
      last_rtt_ms_(0),
      last_reserved_bitrate_bps_(0),
      remb_suppressor_(new RembSuppressor(clock)) {
}

BitrateControllerImpl::~BitrateControllerImpl() {
  delete critsect_;
}

RtcpBandwidthObserver* BitrateControllerImpl::CreateRtcpBandwidthObserver() {
  return new RtcpBandwidthObserverImpl(this);
}

void BitrateControllerImpl::SetStartBitrate(int start_bitrate_bps) {
  CriticalSectionScoped cs(critsect_);
  bandwidth_estimation_.SetSendBitrate(start_bitrate_bps);
}

void BitrateControllerImpl::SetMinMaxBitrate(int min_bitrate_bps,
                                             int max_bitrate_bps) {
  CriticalSectionScoped cs(critsect_);
  bandwidth_estimation_.SetMinMaxBitrate(min_bitrate_bps, max_bitrate_bps);
}

void BitrateControllerImpl::SetReservedBitrate(uint32_t reserved_bitrate_bps) {
  {
    CriticalSectionScoped cs(critsect_);
    reserved_bitrate_bps_ = reserved_bitrate_bps;
  }
  MaybeTriggerOnNetworkChanged();
}

void BitrateControllerImpl::OnReceivedEstimatedBitrate(uint32_t bitrate) {
  {
    CriticalSectionScoped cs(critsect_);
    if (remb_suppressor_->SuppresNewRemb(bitrate)) {
      return;
    }
    bandwidth_estimation_.UpdateReceiverEstimate(bitrate);
  }
  MaybeTriggerOnNetworkChanged();
}

int64_t BitrateControllerImpl::TimeUntilNextProcess() {
  const int64_t kBitrateControllerUpdateIntervalMs = 25;
  CriticalSectionScoped cs(critsect_);
  int64_t time_since_update_ms =
      clock_->TimeInMilliseconds() - last_bitrate_update_ms_;
  return std::max<int64_t>(
      kBitrateControllerUpdateIntervalMs - time_since_update_ms, 0);
}

int32_t BitrateControllerImpl::Process() {
  if (TimeUntilNextProcess() > 0)
    return 0;
  {
    CriticalSectionScoped cs(critsect_);
    bandwidth_estimation_.UpdateEstimate(clock_->TimeInMilliseconds());
  }
  MaybeTriggerOnNetworkChanged();
  last_bitrate_update_ms_ = clock_->TimeInMilliseconds();
  return 0;
}

void BitrateControllerImpl::OnReceivedRtcpReceiverReport(
    uint8_t fraction_loss,
    int64_t rtt,
    int number_of_packets,
    int64_t now_ms) {
  {
    CriticalSectionScoped cs(critsect_);
    bandwidth_estimation_.UpdateReceiverBlock(fraction_loss, rtt,
                                              number_of_packets, now_ms);
  }
  MaybeTriggerOnNetworkChanged();
}

void BitrateControllerImpl::MaybeTriggerOnNetworkChanged() {
  uint32_t bitrate;
  uint8_t fraction_loss;
  int64_t rtt;
  bool new_bitrate = false;
  {
    CriticalSectionScoped cs(critsect_);
    bandwidth_estimation_.CurrentEstimate(&bitrate, &fraction_loss, &rtt);
    bitrate -= std::min(bitrate, reserved_bitrate_bps_);
    bitrate = std::max(bitrate, bandwidth_estimation_.GetMinBitrate());

    if (bitrate != last_bitrate_bps_ || fraction_loss != last_fraction_loss_ ||
        rtt != last_rtt_ms_ ||
        last_reserved_bitrate_bps_ != reserved_bitrate_bps_) {
      last_bitrate_bps_ = bitrate;
      last_fraction_loss_ = fraction_loss;
      last_rtt_ms_ = rtt;
      last_reserved_bitrate_bps_ = reserved_bitrate_bps_;
      new_bitrate = true;
    }
  }
  if (new_bitrate)
    observer_->OnNetworkChanged(bitrate, fraction_loss, rtt);
}

bool BitrateControllerImpl::AvailableBandwidth(uint32_t* bandwidth) const {
  CriticalSectionScoped cs(critsect_);
  uint32_t bitrate;
  uint8_t fraction_loss;
  int64_t rtt;
  bandwidth_estimation_.CurrentEstimate(&bitrate, &fraction_loss, &rtt);
  if (bitrate) {
    *bandwidth = bitrate - std::min(bitrate, reserved_bitrate_bps_);
    *bandwidth = std::max(*bandwidth, bandwidth_estimation_.GetMinBitrate());
    return true;
  }
  return false;
}

void BitrateControllerImpl::SetBitrateSent(uint32_t bitrate_sent_bps) {
  CriticalSectionScoped cs(critsect_);
  remb_suppressor_->SetBitrateSent(bitrate_sent_bps);
}

void BitrateControllerImpl::SetCodecMode(webrtc::VideoCodecMode mode) {
  CriticalSectionScoped cs(critsect_);
  remb_suppressor_->SetEnabled(mode == kScreensharing);
}

}  // namespace webrtc
