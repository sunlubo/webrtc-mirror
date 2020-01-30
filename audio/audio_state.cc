/*
 *  Copyright (c) 2015 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "audio/audio_state.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "audio/audio_receive_stream.h"
#include "audio/audio_send_stream.h"
#include "modules/audio_device/include/audio_device.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/ref_counted_object.h"
#include "rtc_base/thread.h"

namespace webrtc {
namespace internal {

AudioState::AudioState(const AudioState::Config& config)
    : config_(config),
      audio_transport_(config_.audio_mixer, config_.audio_processing.get()) {
  process_thread_checker_.Detach();
  RTC_DCHECK(config_.audio_mixer);
  RTC_DCHECK(config_.audio_device_module);
}

AudioState::~AudioState() {
  RTC_DCHECK(thread_checker_.IsCurrent());
  RTC_DCHECK(receiving_streams_.empty());
  RTC_DCHECK(sending_streams_.empty());
  null_audio_poller_.Stop();
}

AudioProcessing* AudioState::audio_processing() {
  RTC_DCHECK(config_.audio_processing);
  return config_.audio_processing.get();
}

AudioTransport* AudioState::audio_transport() {
  return &audio_transport_;
}

bool AudioState::typing_noise_detected() const {
  RTC_DCHECK(thread_checker_.IsCurrent());
  return audio_transport_.typing_noise_detected();
}

void AudioState::AddReceivingStream(webrtc::AudioReceiveStream* stream) {
  RTC_DCHECK(thread_checker_.IsCurrent());
  RTC_DCHECK_EQ(0, receiving_streams_.count(stream));
  receiving_streams_.insert(stream);
  if (!config_.audio_mixer->AddSource(
          static_cast<internal::AudioReceiveStream*>(stream))) {
    RTC_DLOG(LS_ERROR) << "Failed to add source to mixer.";
  }

  // Make sure playback is initialized; start playing if enabled.
  UpdateNullAudioPollerState();
  auto* adm = config_.audio_device_module.get();
  if (!adm->Playing()) {
    if (adm->InitPlayout() == 0) {
      if (playout_enabled_) {
        adm->StartPlayout();
      }
    } else {
      RTC_DLOG_F(LS_ERROR) << "Failed to initialize playout.";
    }
  }
}

void AudioState::RemoveReceivingStream(webrtc::AudioReceiveStream* stream) {
  RTC_DCHECK(thread_checker_.IsCurrent());
  auto count = receiving_streams_.erase(stream);
  RTC_DCHECK_EQ(1, count);
  config_.audio_mixer->RemoveSource(
      static_cast<internal::AudioReceiveStream*>(stream));
  UpdateNullAudioPollerState();
  if (receiving_streams_.empty()) {
    config_.audio_device_module->StopPlayout();
  }
}

void AudioState::AddSendingStream(webrtc::AudioSendStream* stream,
                                  int sample_rate_hz,
                                  size_t num_channels) {
  RTC_DCHECK(thread_checker_.IsCurrent());
  auto& properties = sending_streams_[stream];
  properties.sample_rate_hz = sample_rate_hz;
  properties.num_channels = num_channels;
  UpdateAudioTransportWithSendingStreams();

  // Make sure recording is initialized; start recording if enabled.
  auto* adm = config_.audio_device_module.get();
  if (!adm->Recording()) {
    if (adm->InitRecording() == 0) {
      if (recording_enabled_) {
        adm->StartRecording();
      }
    } else {
      RTC_DLOG_F(LS_ERROR) << "Failed to initialize recording.";
    }
  }
}

void AudioState::RemoveSendingStream(webrtc::AudioSendStream* stream) {
  RTC_DCHECK(thread_checker_.IsCurrent());
  auto count = sending_streams_.erase(stream);
  RTC_DCHECK_EQ(1, count);
  UpdateAudioTransportWithSendingStreams();
  if (sending_streams_.empty()) {
    config_.audio_device_module->StopRecording();
  }
}

void AudioState::SetPlayout(bool enabled) {
  RTC_LOG(INFO) << "SetPlayout(" << enabled << ")";
  RTC_DCHECK(thread_checker_.IsCurrent());
  if (playout_enabled_ != enabled) {
    playout_enabled_ = enabled;
    if (enabled) {
      UpdateNullAudioPollerState();
      if (!receiving_streams_.empty()) {
        config_.audio_device_module->StartPlayout();
      }
    } else {
      config_.audio_device_module->StopPlayout();
      UpdateNullAudioPollerState();
    }
  }
}

void AudioState::SetRecording(bool enabled) {
  RTC_LOG(INFO) << "SetRecording(" << enabled << ")";
  RTC_DCHECK(thread_checker_.IsCurrent());
  if (recording_enabled_ != enabled) {
    recording_enabled_ = enabled;
    if (enabled) {
      if (!sending_streams_.empty()) {
        config_.audio_device_module->StartRecording();
      }
    } else {
      config_.audio_device_module->StopRecording();
    }
  }
}

void AudioState::SetStereoChannelSwapping(bool enable) {
  RTC_DCHECK(thread_checker_.IsCurrent());
  audio_transport_.SetStereoChannelSwapping(enable);
}

void AudioState::UpdateAudioTransportWithSendingStreams() {
  RTC_DCHECK(thread_checker_.IsCurrent());
  std::vector<AudioSender*> audio_senders;
  int max_sample_rate_hz = 8000;
  size_t max_num_channels = 1;
  for (const auto& kv : sending_streams_) {
    audio_senders.push_back(kv.first);
    max_sample_rate_hz = std::max(max_sample_rate_hz, kv.second.sample_rate_hz);
    max_num_channels = std::max(max_num_channels, kv.second.num_channels);
  }
  audio_transport_.UpdateAudioSenders(std::move(audio_senders),
                                      max_sample_rate_hz, max_num_channels);
}

void AudioState::UpdateNullAudioPollerState() {
  // Run NullAudioPoller when there are receiving streams and playout is
  // disabled.
  if (!receiving_streams_.empty() && !playout_enabled_) {
    if (!null_audio_poller_.Running()) {
      // TODO(srte): Replace current thread with an explicit task queue
      // instance.
      null_audio_poller_ =
          RepeatingTaskHandle::Start(rtc::Thread::Current(), [this] {
            // WebRTC uses 10ms audio windows by default
            constexpr TimeDelta kPollInterval = TimeDelta::ms(10);
            constexpr Frequency kSampleRate = Frequency::kHz(48);
            constexpr size_t kSamplesPerPoll =
                static_cast<size_t>(kSampleRate * kPollInterval);
            constexpr size_t kNumChannels = 1;
            int16_t audio_sample_buffer[kSamplesPerPoll * kNumChannels];
            // Output variables from |NeedMorePlayData|.
            size_t n_samples;
            int64_t elapsed_time_ms;
            int64_t ntp_time_ms;
            audio_transport_.NeedMorePlayData(kSamplesPerPoll, sizeof(int16_t),
                                              kNumChannels, kSampleRate.hertz(),
                                              audio_sample_buffer, n_samples,
                                              &elapsed_time_ms, &ntp_time_ms);
            return kPollInterval;
          });
    }
  } else {
    null_audio_poller_.Stop();
  }
}
}  // namespace internal

rtc::scoped_refptr<AudioState> AudioState::Create(
    const AudioState::Config& config) {
  return new rtc::RefCountedObject<internal::AudioState>(config);
}
}  // namespace webrtc
