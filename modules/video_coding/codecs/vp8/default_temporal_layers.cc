/* Copyright (c) 2013 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/video_coding/codecs/vp8/default_temporal_layers.h"

#include <stdlib.h>

#include <algorithm>
#include <array>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include "modules/include/module_common_types.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "rtc_base/arraysize.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "system_wrappers/include/field_trial.h"

namespace webrtc {
DefaultTemporalLayers::PendingFrame::PendingFrame() = default;
DefaultTemporalLayers::PendingFrame::PendingFrame(
    bool expired,
    uint8_t updated_buffers_mask,
    const DependencyInfo& dependency_info)
    : expired(expired),
      updated_buffer_mask(updated_buffers_mask),
      dependency_info(dependency_info) {}

namespace {
using BufferFlags = Vp8FrameConfig::BufferFlags;
using FreezeEntropy = Vp8FrameConfig::FreezeEntropy;
using Vp8BufferReference = Vp8FrameConfig::Vp8BufferReference;

constexpr BufferFlags kNone = BufferFlags::kNone;
constexpr BufferFlags kReference = BufferFlags::kReference;
constexpr BufferFlags kUpdate = BufferFlags::kUpdate;
constexpr BufferFlags kReferenceAndUpdate = BufferFlags::kReferenceAndUpdate;
constexpr FreezeEntropy kFreezeEntropy = FreezeEntropy::kFreezeEntropy;

static constexpr uint8_t kUninitializedPatternIndex =
    std::numeric_limits<uint8_t>::max();
static constexpr std::array<Vp8BufferReference, 3> kAllBuffers = {
    {Vp8BufferReference::kLast, Vp8BufferReference::kGolden,
     Vp8BufferReference::kAltref}};

std::vector<unsigned int> GetTemporalIds(size_t num_layers) {
  switch (num_layers) {
    case 1:
      // Temporal layer structure (single layer):
      // 0 0 0 0 ...
      return {0};
    case 2:
      // Temporal layer structure:
      //   1   1 ...
      // 0   0   ...
      return {0, 1};
    case 3:
      // Temporal layer structure:
      //   2   2   2   2 ...
      //     1       1   ...
      // 0       0       ...
      return {0, 2, 1, 2};
    case 4:
      // Temporal layer structure:
      //   3   3   3   3   3   3   3   3 ...
      //     2       2       2       2   ...
      //         1               1       ...
      // 0               0               ...
      return {0, 3, 2, 3, 1, 3, 2, 3};
    default:
      RTC_NOTREACHED();
      break;
  }
  RTC_NOTREACHED();
  return {0};
}

uint8_t GetUpdatedBuffers(const Vp8FrameConfig& config) {
  uint8_t flags = 0;
  if (config.last_buffer_flags & BufferFlags::kUpdate) {
    flags |= static_cast<uint8_t>(Vp8BufferReference::kLast);
  }
  if (config.golden_buffer_flags & BufferFlags::kUpdate) {
    flags |= static_cast<uint8_t>(Vp8BufferReference::kGolden);
  }
  if (config.arf_buffer_flags & BufferFlags::kUpdate) {
    flags |= static_cast<uint8_t>(Vp8BufferReference::kAltref);
  }
  return flags;
}
}  // namespace

std::vector<DefaultTemporalLayers::DependencyInfo>
DefaultTemporalLayers::GetDependencyInfo(size_t num_layers) {
  // For indexing in the patterns described below (which temporal layers they
  // belong to), see the diagram above.
  // Layer sync is done similarly for all patterns (except single stream) and
  // happens every 8 frames:
  // TL1 layer syncs by periodically by only referencing TL0 ('last'), but still
  // updating 'golden', so it can be used as a reference by future TL1 frames.
  // TL2 layer syncs just before TL1 by only depending on TL0 (and not depending
  // on TL1's buffer before TL1 has layer synced).
  // TODO(pbos): Consider cyclically updating 'arf' (and 'golden' for 1TL) for
  // the base layer in 1-3TL instead of 'last' periodically on long intervals,
  // so that if scene changes occur (user walks between rooms or rotates webcam)
  // the 'arf' (or 'golden' respectively) is not stuck on a no-longer relevant
  // keyframe.

  switch (num_layers) {
    case 1:
      // Always reference and update the same buffer.
      return {{"S", {kReferenceAndUpdate, kNone, kNone}}};
    case 2:
      // All layers can reference but not update the 'alt' buffer, this means
      // that the 'alt' buffer reference is effectively the last keyframe.
      // TL0 also references and updates the 'last' buffer.
      // TL1 also references 'last' and references and updates 'golden'.
      if (!field_trial::IsDisabled("WebRTC-UseShortVP8TL2Pattern")) {
        // Shortened 4-frame pattern:
        //   1---1   1---1 ...
        //  /   /   /   /
        // 0---0---0---0 ...
        return {{"SS", {kReferenceAndUpdate, kNone, kNone}},
                {"-S", {kReference, kUpdate, kNone}},
                {"SR", {kReferenceAndUpdate, kNone, kNone}},
                {"-D", {kReference, kReference, kNone, kFreezeEntropy}}};
      } else {
        // "Default" 8-frame pattern:
        //   1---1---1---1   1---1---1---1 ...
        //  /   /   /   /   /   /   /   /
        // 0---0---0---0---0---0---0---0 ...
        return {{"SS", {kReferenceAndUpdate, kNone, kNone}},
                {"-S", {kReference, kUpdate, kNone}},
                {"SR", {kReferenceAndUpdate, kNone, kNone}},
                {"-R", {kReference, kReferenceAndUpdate, kNone}},
                {"SR", {kReferenceAndUpdate, kNone, kNone}},
                {"-R", {kReference, kReferenceAndUpdate, kNone}},
                {"SR", {kReferenceAndUpdate, kNone, kNone}},
                {"-D", {kReference, kReference, kNone, kFreezeEntropy}}};
      }
    case 3:
      if (field_trial::IsEnabled("WebRTC-UseShortVP8TL3Pattern")) {
        // This field trial is intended to check if it is worth using a shorter
        // temporal pattern, trading some coding efficiency for less risk of
        // dropped frames.
        // The coding efficiency will decrease somewhat since the higher layer
        // state is more volatile, but it will be offset slightly by updating
        // the altref buffer with TL2 frames, instead of just referencing lower
        // layers.
        // If a frame is dropped in a higher layer, the jitter
        // buffer on the receive side won't be able to decode any higher layer
        // frame until the next sync frame. So we expect a noticeable decrease
        // in frame drops on links with high packet loss.

        // TL0 references and updates the 'last' buffer.
        // TL1  references 'last' and references and updates 'golden'.
        // TL2 references both 'last' & 'golden' and references and updates
        // 'arf'.
        return {{"SSS", {kReferenceAndUpdate, kNone, kNone}},
                {"--S", {kReference, kNone, kUpdate}},
                {"-DR", {kReference, kUpdate, kNone}},
                {"--D", {kReference, kReference, kReference, kFreezeEntropy}}};
      } else {
        // All layers can reference but not update the 'alt' buffer, this means
        // that the 'alt' buffer reference is effectively the last keyframe.
        // TL0 also references and updates the 'last' buffer.
        // TL1 also references 'last' and references and updates 'golden'.
        // TL2 references both 'last' and 'golden' but updates no buffer.
        return {{"SSS", {kReferenceAndUpdate, kNone, kNone}},
                {"--D", {kReference, kNone, kNone, kFreezeEntropy}},
                {"-SS", {kReference, kUpdate, kNone}},
                {"--D", {kReference, kReference, kNone, kFreezeEntropy}},
                {"SRR", {kReferenceAndUpdate, kNone, kNone}},
                {"--D", {kReference, kReference, kNone, kFreezeEntropy}},
                {"-DS", {kReference, kReferenceAndUpdate, kNone}},
                {"--D", {kReference, kReference, kNone, kFreezeEntropy}}};
      }
    case 4:
      // TL0 references and updates only the 'last' buffer.
      // TL1 references 'last' and updates and references 'golden'.
      // TL2 references 'last' and 'golden', and references and updates 'arf'.
      // TL3 references all buffers but update none of them.
      // TODO(philipel): Set decode target information for this structure.
      return {{"----", {kReferenceAndUpdate, kNone, kNone}},
              {"----", {kReference, kNone, kNone, kFreezeEntropy}},
              {"----", {kReference, kNone, kUpdate}},
              {"----", {kReference, kNone, kReference, kFreezeEntropy}},
              {"----", {kReference, kUpdate, kNone}},
              {"----", {kReference, kReference, kReference, kFreezeEntropy}},
              {"----", {kReference, kReference, kReferenceAndUpdate}},
              {"----", {kReference, kReference, kReference, kFreezeEntropy}},
              {"----", {kReferenceAndUpdate, kNone, kNone}},
              {"----", {kReference, kReference, kReference, kFreezeEntropy}},
              {"----", {kReference, kReference, kReferenceAndUpdate}},
              {"----", {kReference, kReference, kReference, kFreezeEntropy}},
              {"----", {kReference, kReferenceAndUpdate, kNone}},
              {"----", {kReference, kReference, kReference, kFreezeEntropy}},
              {"----", {kReference, kReference, kReferenceAndUpdate}},
              {"----", {kReference, kReference, kReference, kFreezeEntropy}}};
    default:
      RTC_NOTREACHED();
      break;
  }
  RTC_NOTREACHED();
  return {{"", {kNone, kNone, kNone}}};
}

DefaultTemporalLayers::DefaultTemporalLayers(int number_of_temporal_layers)
    : num_layers_(std::max(1, number_of_temporal_layers)),
      temporal_ids_(GetTemporalIds(num_layers_)),
      temporal_pattern_(GetDependencyInfo(num_layers_)),
      pattern_idx_(kUninitializedPatternIndex) {
  RTC_CHECK_GE(kMaxTemporalStreams, number_of_temporal_layers);
  RTC_CHECK_GE(number_of_temporal_layers, 0);
  RTC_CHECK_LE(number_of_temporal_layers, 4);
  // pattern_idx_ wraps around temporal_pattern_.size, this is incorrect if
  // temporal_ids_ are ever longer. If this is no longer correct it needs to
  // wrap at max(temporal_ids_.size(), temporal_pattern_.size()).
  RTC_DCHECK_LE(temporal_ids_.size(), temporal_pattern_.size());

#if RTC_DCHECK_IS_ON
  checker_ = TemporalLayersChecker::CreateTemporalLayersChecker(
      Vp8TemporalLayersType::kFixedPattern, number_of_temporal_layers);
#endif

  // Always need to start with a keyframe, so pre-populate all frame counters.
  for (Vp8BufferReference buffer : kAllBuffers) {
    frames_since_buffer_refresh_[buffer] = 0;
  }

  kf_buffers_ = {kAllBuffers.begin(), kAllBuffers.end()};
  for (const DependencyInfo& info : temporal_pattern_) {
    uint8_t updated_buffers = GetUpdatedBuffers(info.frame_config);

    for (Vp8BufferReference buffer : kAllBuffers) {
      if (static_cast<uint8_t>(buffer) & updated_buffers)
        kf_buffers_.erase(buffer);
    }
  }
}

DefaultTemporalLayers::~DefaultTemporalLayers() = default;

void DefaultTemporalLayers::SetQpLimits(size_t stream_index,
                                        int min_qp,
                                        int max_qp) {
  RTC_DCHECK_LT(stream_index, StreamCount());
  // Ignore.
}

void DefaultTemporalLayers::SetFecControllerOverride(
    FecControllerOverride* fec_controller_override) {
  // Ignore.
}

size_t DefaultTemporalLayers::StreamCount() const {
  return 1;
}

bool DefaultTemporalLayers::SupportsEncoderFrameDropping(
    size_t stream_index) const {
  RTC_DCHECK_LT(stream_index, StreamCount());
  // This class allows the encoder drop frames as it sees fit.
  return true;
}

void DefaultTemporalLayers::OnRatesUpdated(
    size_t stream_index,
    const std::vector<uint32_t>& bitrates_bps,
    int framerate_fps) {
  RTC_DCHECK_LT(stream_index, StreamCount());
  RTC_DCHECK_GT(bitrates_bps.size(), 0);
  RTC_DCHECK_LE(bitrates_bps.size(), num_layers_);
  // |bitrates_bps| uses individual rate per layer, but Vp8EncoderConfig wants
  // the accumulated rate, so sum them up.
  new_bitrates_bps_ = bitrates_bps;
  new_bitrates_bps_->resize(num_layers_);
  for (size_t i = 1; i < num_layers_; ++i) {
    (*new_bitrates_bps_)[i] += (*new_bitrates_bps_)[i - 1];
  }
}

Vp8EncoderConfig DefaultTemporalLayers::UpdateConfiguration(
    size_t stream_index) {
  RTC_DCHECK_LT(stream_index, StreamCount());

  Vp8EncoderConfig config;

  if (!new_bitrates_bps_) {
    return config;
  }

  config.temporal_layer_config.emplace();
  Vp8EncoderConfig::TemporalLayerConfig& ts_config =
      config.temporal_layer_config.value();

  for (size_t i = 0; i < num_layers_; ++i) {
    ts_config.ts_target_bitrate[i] = (*new_bitrates_bps_)[i] / 1000;
    // ..., 4, 2, 1
    ts_config.ts_rate_decimator[i] = 1 << (num_layers_ - i - 1);
  }

  ts_config.ts_number_layers = num_layers_;
  ts_config.ts_periodicity = temporal_ids_.size();
  std::copy(temporal_ids_.begin(), temporal_ids_.end(),
            ts_config.ts_layer_id.begin());

  new_bitrates_bps_.reset();

  return config;
}

bool DefaultTemporalLayers::IsSyncFrame(const Vp8FrameConfig& config) const {
  // Since we always assign TL0 to 'last' in these patterns, we can infer layer
  // sync by checking if temporal id > 0 and we only reference TL0 or buffers
  // containing the last key-frame.
  if (config.packetizer_temporal_idx == 0) {
    // TL0 frames are per definition not sync frames.
    return false;
  }

  if ((config.last_buffer_flags & BufferFlags::kReference) == 0) {
    // Sync frames must reference TL0.
    return false;
  }

  if ((config.golden_buffer_flags & BufferFlags::kReference) &&
      kf_buffers_.find(Vp8BufferReference::kGolden) == kf_buffers_.end()) {
    // Referencing a golden frame that contains a non-(base layer|key frame).
    return false;
  }
  if ((config.arf_buffer_flags & BufferFlags::kReference) &&
      kf_buffers_.find(Vp8BufferReference::kAltref) == kf_buffers_.end()) {
    // Referencing an altref frame that contains a non-(base layer|key frame).
    return false;
  }

  return true;
}

Vp8FrameConfig DefaultTemporalLayers::NextFrameConfig(size_t stream_index,
                                                      uint32_t timestamp) {
  RTC_DCHECK_LT(stream_index, StreamCount());
  RTC_DCHECK_GT(num_layers_, 0);
  RTC_DCHECK_GT(temporal_pattern_.size(), 0);

  RTC_DCHECK_GT(kUninitializedPatternIndex, temporal_pattern_.size());
  const bool first_frame = (pattern_idx_ == kUninitializedPatternIndex);

  pattern_idx_ = (pattern_idx_ + 1) % temporal_pattern_.size();
  DependencyInfo dependency_info = temporal_pattern_[pattern_idx_];
  Vp8FrameConfig& tl_config = dependency_info.frame_config;
  tl_config.encoder_layer_id = tl_config.packetizer_temporal_idx =
      temporal_ids_[pattern_idx_ % temporal_ids_.size()];

  if (pattern_idx_ == 0) {
    // Start of new pattern iteration, set up clear state by invalidating any
    // pending frames, so that we don't make an invalid reference to a buffer
    // containing data from a previous iteration.
    for (auto& it : pending_frames_) {
      it.second.expired = true;
    }
  }

  if (first_frame) {
    tl_config = Vp8FrameConfig::GetIntraFrameConfig();
  } else {
    // Last is always ok to reference as it contains the base layer. For other
    // buffers though, we need to check if the buffer has actually been
    // refreshed this cycle of the temporal pattern. If the encoder dropped
    // a frame, it might not have.
    ValidateReferences(&tl_config.golden_buffer_flags,
                       Vp8BufferReference::kGolden);
    ValidateReferences(&tl_config.arf_buffer_flags,
                       Vp8BufferReference::kAltref);
    // Update search order to let the encoder know which buffers contains the
    // most recent data.
    UpdateSearchOrder(&tl_config);
    // Figure out if this a sync frame (non-base-layer frame with only
    // base-layer references).
    tl_config.layer_sync = IsSyncFrame(tl_config);

    // Increment frame age, this needs to be in sync with |pattern_idx_|,
    // so must update it here. Resetting age to 0 must be done when encoding is
    // complete though, and so in the case of pipelining encoder it might lag.
    // To prevent this data spill over into the next iteration,
    // the |pedning_frames_| map is reset in loops. If delay is constant,
    // the relative age should still be OK for the search order.
    for (Vp8BufferReference buffer : kAllBuffers) {
      ++frames_since_buffer_refresh_[buffer];
    }
  }

  // Add frame to set of pending frames, awaiting completion.
  pending_frames_[timestamp] =
      PendingFrame{false, GetUpdatedBuffers(tl_config), dependency_info};

#if RTC_DCHECK_IS_ON
  // Checker does not yet support encoder frame dropping, so validate flags
  // here before they can be dropped.
  // TODO(sprang): Update checker to support dropping.
  RTC_DCHECK(checker_->CheckTemporalConfig(first_frame, tl_config));
#endif

  return tl_config;
}

void DefaultTemporalLayers::ValidateReferences(BufferFlags* flags,
                                               Vp8BufferReference ref) const {
  // Check if the buffer specified by |ref| is actually referenced, and if so
  // if it also a dynamically updating one (buffers always just containing
  // keyframes are always safe to reference).
  if ((*flags & BufferFlags::kReference) &&
      kf_buffers_.find(ref) == kf_buffers_.end()) {
    auto it = frames_since_buffer_refresh_.find(ref);
    if (it == frames_since_buffer_refresh_.end() ||
        it->second >= pattern_idx_) {
      // No valid buffer state, or buffer contains frame that is older than the
      // current pattern. This reference is not valid, so remove it.
      *flags = static_cast<BufferFlags>(*flags & ~BufferFlags::kReference);
    }
  }
}

void DefaultTemporalLayers::UpdateSearchOrder(Vp8FrameConfig* config) {
  // Figure out which of the buffers we can reference, and order them so that
  // the most recently refreshed is first. Otherwise prioritize last first,
  // golden second, and altref third.
  using BufferRefAge = std::pair<Vp8BufferReference, size_t>;
  std::vector<BufferRefAge> eligible_buffers;
  if (config->last_buffer_flags & BufferFlags::kReference) {
    eligible_buffers.emplace_back(
        Vp8BufferReference::kLast,
        frames_since_buffer_refresh_[Vp8BufferReference::kLast]);
  }
  if (config->golden_buffer_flags & BufferFlags::kReference) {
    eligible_buffers.emplace_back(
        Vp8BufferReference::kGolden,
        frames_since_buffer_refresh_[Vp8BufferReference::kGolden]);
  }
  if (config->arf_buffer_flags & BufferFlags::kReference) {
    eligible_buffers.emplace_back(
        Vp8BufferReference::kAltref,
        frames_since_buffer_refresh_[Vp8BufferReference::kAltref]);
  }

  std::sort(eligible_buffers.begin(), eligible_buffers.end(),
            [](const BufferRefAge& lhs, const BufferRefAge& rhs) {
              if (lhs.second != rhs.second) {
                // Lower count has highest precedence.
                return lhs.second < rhs.second;
              }
              return lhs.first < rhs.first;
            });

  // Populate the search order fields where possible.
  if (!eligible_buffers.empty()) {
    config->first_reference = eligible_buffers.front().first;
    if (eligible_buffers.size() > 1)
      config->second_reference = eligible_buffers[1].first;
  }
}

void DefaultTemporalLayers::OnEncodeDone(size_t stream_index,
                                         uint32_t rtp_timestamp,
                                         size_t size_bytes,
                                         bool is_keyframe,
                                         int qp,
                                         CodecSpecificInfo* info) {
  RTC_DCHECK_LT(stream_index, StreamCount());
  RTC_DCHECK_GT(num_layers_, 0);

  if (size_bytes == 0) {
    RTC_LOG(LS_WARNING) << "Empty frame; treating as dropped.";
    OnFrameDropped(stream_index, rtp_timestamp);
    return;
  }

  auto pending_frame = pending_frames_.find(rtp_timestamp);
  RTC_DCHECK(pending_frame != pending_frames_.end());

  PendingFrame& frame = pending_frame->second;
  const Vp8FrameConfig& frame_config = frame.dependency_info.frame_config;
#if RTC_DCHECK_IS_ON
  if (is_keyframe) {
    // Signal key-frame so checker resets state.
    RTC_DCHECK(checker_->CheckTemporalConfig(true, frame_config));
  }
#endif

  CodecSpecificInfoVP8& vp8_info = info->codecSpecific.VP8;
  if (num_layers_ == 1) {
    vp8_info.temporalIdx = kNoTemporalIdx;
    vp8_info.layerSync = false;
  } else {
    if (is_keyframe) {
      // Restart the temporal pattern on keyframes.
      pattern_idx_ = 0;
      vp8_info.temporalIdx = 0;
      vp8_info.layerSync = true;  // Keyframes are always sync frames.

      for (Vp8BufferReference buffer : kAllBuffers) {
        if (kf_buffers_.find(buffer) != kf_buffers_.end()) {
          // Update frame count of all kf-only buffers, regardless of state of
          // |pending_frames_|.
          frames_since_buffer_refresh_[buffer] = 0;
        } else {
          // Key-frames update all buffers, this should be reflected when
          // updating state in FrameEncoded().
          frame.updated_buffer_mask |= static_cast<uint8_t>(buffer);
        }
      }
    } else {
      // Delta frame, update codec specifics with temporal id and sync flag.
      vp8_info.temporalIdx = frame_config.packetizer_temporal_idx;
      vp8_info.layerSync = frame_config.layer_sync;
    }
  }

  vp8_info.useExplicitDependencies = true;
  RTC_DCHECK_EQ(vp8_info.referencedBuffersCount, 0u);
  RTC_DCHECK_EQ(vp8_info.updatedBuffersCount, 0u);

  GenericFrameInfo& generic_frame_info = info->generic_frame_info.emplace();

  for (int i = 0; i < static_cast<int>(Vp8FrameConfig::Buffer::kCount); ++i) {
    bool references = false;
    bool updates = is_keyframe;

    if (!is_keyframe &&
        frame_config.References(static_cast<Vp8FrameConfig::Buffer>(i))) {
      RTC_DCHECK_LT(vp8_info.referencedBuffersCount,
                    arraysize(CodecSpecificInfoVP8::referencedBuffers));
      references = true;
      vp8_info.referencedBuffers[vp8_info.referencedBuffersCount++] = i;
    }

    if (is_keyframe ||
        frame_config.Updates(static_cast<Vp8FrameConfig::Buffer>(i))) {
      RTC_DCHECK_LT(vp8_info.updatedBuffersCount,
                    arraysize(CodecSpecificInfoVP8::updatedBuffers));
      updates = true;
      vp8_info.updatedBuffers[vp8_info.updatedBuffersCount++] = i;
    }

    if (references || updates)
      generic_frame_info.encoder_buffers.emplace_back(i, references, updates);
  }

  // The templates are always present on keyframes, and then refered to by
  // subsequent frames.
  if (is_keyframe) {
    info->template_structure = GetTemplateStructure(num_layers_);
  }
  generic_frame_info.decode_target_indications =
      frame.dependency_info.decode_target_indications;
  generic_frame_info.temporal_id = frame_config.packetizer_temporal_idx;

  if (!frame.expired) {
    for (Vp8BufferReference buffer : kAllBuffers) {
      if (frame.updated_buffer_mask & static_cast<uint8_t>(buffer)) {
        frames_since_buffer_refresh_[buffer] = 0;
      }
    }
  }

  pending_frames_.erase(pending_frame);
}

void DefaultTemporalLayers::OnFrameDropped(size_t stream_index,
                                           uint32_t rtp_timestamp) {
  auto pending_frame = pending_frames_.find(rtp_timestamp);
  RTC_DCHECK(pending_frame != pending_frames_.end());
  pending_frames_.erase(pending_frame);
}

void DefaultTemporalLayers::OnPacketLossRateUpdate(float packet_loss_rate) {}

void DefaultTemporalLayers::OnRttUpdate(int64_t rtt_ms) {}

void DefaultTemporalLayers::OnLossNotification(
    const VideoEncoder::LossNotification& loss_notification) {}

FrameDependencyStructure DefaultTemporalLayers::GetTemplateStructure(
    int num_layers) const {
  RTC_CHECK_LT(num_layers, 5);
  RTC_CHECK_GT(num_layers, 0);

  FrameDependencyStructure template_structure;
  template_structure.num_decode_targets = num_layers;

  using Builder = GenericFrameInfo::Builder;
  switch (num_layers) {
    case 1: {
      template_structure.templates = {
          Builder().T(0).Dtis("S").Build(),
          Builder().T(0).Dtis("S").Fdiffs({1}).Build(),
      };
      return template_structure;
    }
    case 2: {
      template_structure.templates = {
          Builder().T(0).Dtis("SS").Build(),
          Builder().T(0).Dtis("SS").Fdiffs({2}).Build(),
          Builder().T(0).Dtis("SR").Fdiffs({2}).Build(),
          Builder().T(1).Dtis("-S").Fdiffs({1}).Build(),
          Builder().T(1).Dtis("-D").Fdiffs({1, 2}).Build(),
      };
      return template_structure;
    }
    case 3: {
      template_structure.templates = {
          Builder().T(0).Dtis("SSS").Build(),
          Builder().T(0).Dtis("SSS").Fdiffs({4}).Build(),
          Builder().T(0).Dtis("SRR").Fdiffs({4}).Build(),
          Builder().T(1).Dtis("-SR").Fdiffs({2}).Build(),
          Builder().T(1).Dtis("-DR").Fdiffs({2, 4}).Build(),
          Builder().T(2).Dtis("--D").Fdiffs({1}).Build(),
          Builder().T(2).Dtis("--D").Fdiffs({1, 3}).Build(),
      };
      return template_structure;
    }
    case 4: {
      template_structure.templates = {
          Builder().T(0).Dtis("SSSS").Build(),
          Builder().T(0).Dtis("SSSS").Fdiffs({8}).Build(),
          Builder().T(1).Dtis("-SRR").Fdiffs({4}).Build(),
          Builder().T(1).Dtis("-SRR").Fdiffs({4, 8}).Build(),
          Builder().T(2).Dtis("--SR").Fdiffs({2}).Build(),
          Builder().T(2).Dtis("--SR").Fdiffs({2, 4}).Build(),
          Builder().T(3).Dtis("---D").Fdiffs({1}).Build(),
          Builder().T(3).Dtis("---D").Fdiffs({1, 3}).Build(),
      };
      return template_structure;
    }
    default:
      RTC_NOTREACHED();
      // To make the compiler happy!
      return template_structure;
  }
}

// Returns list of temporal dependencies for each frame in the temporal pattern.
// Values are lists of indecies in the pattern.
std::vector<std::set<uint8_t>> GetTemporalDependencies(
    int num_temporal_layers) {
  switch (num_temporal_layers) {
    case 1:
      return {{0}};
    case 2:
      if (!field_trial::IsDisabled("WebRTC-UseShortVP8TL2Pattern")) {
        return {{2}, {0}, {0}, {1, 2}};
      } else {
        return {{6}, {0}, {0}, {1, 2}, {2}, {3, 4}, {4}, {5, 6}};
      }
    case 3:
      if (field_trial::IsEnabled("WebRTC-UseShortVP8TL3Pattern")) {
        return {{0}, {0}, {0}, {0, 1, 2}};
      } else {
        return {{4}, {0}, {0}, {0, 2}, {0}, {2, 4}, {2, 4}, {4, 6}};
      }
    case 4:
      return {{8},    {0},         {0},         {0, 2},
              {0},    {0, 2, 4},   {0, 2, 4},   {0, 4, 6},
              {0},    {4, 6, 8},   {4, 6, 8},   {4, 8, 10},
              {4, 8}, {8, 10, 12}, {8, 10, 12}, {8, 12, 14}};
    default:
      RTC_NOTREACHED();
      return {};
  }
}

DefaultTemporalLayersChecker::DefaultTemporalLayersChecker(
    int num_temporal_layers)
    : TemporalLayersChecker(num_temporal_layers),
      num_layers_(std::max(1, num_temporal_layers)),
      temporal_ids_(GetTemporalIds(num_layers_)),
      temporal_dependencies_(GetTemporalDependencies(num_layers_)),
      pattern_idx_(255) {
  int i = 0;
  while (temporal_ids_.size() < temporal_dependencies_.size()) {
    temporal_ids_.push_back(temporal_ids_[i++]);
  }
}

DefaultTemporalLayersChecker::~DefaultTemporalLayersChecker() = default;

bool DefaultTemporalLayersChecker::CheckTemporalConfig(
    bool frame_is_keyframe,
    const Vp8FrameConfig& frame_config) {
  if (!TemporalLayersChecker::CheckTemporalConfig(frame_is_keyframe,
                                                  frame_config)) {
    return false;
  }
  if (frame_config.drop_frame) {
    return true;
  }

  if (frame_is_keyframe) {
    pattern_idx_ = 0;
    last_ = BufferState();
    golden_ = BufferState();
    arf_ = BufferState();
    return true;
  }

  ++pattern_idx_;
  if (pattern_idx_ == temporal_ids_.size()) {
    // All non key-frame buffers should be updated each pattern cycle.
    if (!last_.is_keyframe && !last_.is_updated_this_cycle) {
      RTC_LOG(LS_ERROR) << "Last buffer was not updated during pattern cycle.";
      return false;
    }
    if (!arf_.is_keyframe && !arf_.is_updated_this_cycle) {
      RTC_LOG(LS_ERROR) << "Arf buffer was not updated during pattern cycle.";
      return false;
    }
    if (!golden_.is_keyframe && !golden_.is_updated_this_cycle) {
      RTC_LOG(LS_ERROR)
          << "Golden buffer was not updated during pattern cycle.";
      return false;
    }
    last_.is_updated_this_cycle = false;
    arf_.is_updated_this_cycle = false;
    golden_.is_updated_this_cycle = false;
    pattern_idx_ = 0;
  }
  uint8_t expected_tl_idx = temporal_ids_[pattern_idx_];
  if (frame_config.packetizer_temporal_idx != expected_tl_idx) {
    RTC_LOG(LS_ERROR) << "Frame has an incorrect temporal index. Expected: "
                      << static_cast<int>(expected_tl_idx) << " Actual: "
                      << static_cast<int>(frame_config.packetizer_temporal_idx);
    return false;
  }

  bool need_sync = temporal_ids_[pattern_idx_] > 0 &&
                   temporal_ids_[pattern_idx_] != kNoTemporalIdx;
  std::vector<int> dependencies;

  if (frame_config.last_buffer_flags & BufferFlags::kReference) {
    uint8_t referenced_layer = temporal_ids_[last_.pattern_idx];
    if (referenced_layer > 0) {
      need_sync = false;
    }
    if (!last_.is_keyframe) {
      dependencies.push_back(last_.pattern_idx);
    }
  } else if (frame_config.first_reference == Vp8BufferReference::kLast ||
             frame_config.second_reference == Vp8BufferReference::kLast) {
    RTC_LOG(LS_ERROR)
        << "Last buffer not referenced, but present in search order.";
    return false;
  }

  if (frame_config.arf_buffer_flags & BufferFlags::kReference) {
    uint8_t referenced_layer = temporal_ids_[arf_.pattern_idx];
    if (referenced_layer > 0) {
      need_sync = false;
    }
    if (!arf_.is_keyframe) {
      dependencies.push_back(arf_.pattern_idx);
    }
  } else if (frame_config.first_reference == Vp8BufferReference::kAltref ||
             frame_config.second_reference == Vp8BufferReference::kAltref) {
    RTC_LOG(LS_ERROR)
        << "Altret buffer not referenced, but present in search order.";
    return false;
  }

  if (frame_config.golden_buffer_flags & BufferFlags::kReference) {
    uint8_t referenced_layer = temporal_ids_[golden_.pattern_idx];
    if (referenced_layer > 0) {
      need_sync = false;
    }
    if (!golden_.is_keyframe) {
      dependencies.push_back(golden_.pattern_idx);
    }
  } else if (frame_config.first_reference == Vp8BufferReference::kGolden ||
             frame_config.second_reference == Vp8BufferReference::kGolden) {
    RTC_LOG(LS_ERROR)
        << "Golden buffer not referenced, but present in search order.";
    return false;
  }

  if (need_sync != frame_config.layer_sync) {
    RTC_LOG(LS_ERROR) << "Sync bit is set incorrectly on a frame. Expected: "
                      << need_sync << " Actual: " << frame_config.layer_sync;
    return false;
  }

  if (!frame_is_keyframe) {
    size_t i;
    for (i = 0; i < dependencies.size(); ++i) {
      if (temporal_dependencies_[pattern_idx_].find(dependencies[i]) ==
          temporal_dependencies_[pattern_idx_].end()) {
        RTC_LOG(LS_ERROR)
            << "Illegal temporal dependency out of defined pattern "
               "from position "
            << static_cast<int>(pattern_idx_) << " to position "
            << static_cast<int>(dependencies[i]);
        return false;
      }
    }
  }

  if (frame_config.last_buffer_flags & BufferFlags::kUpdate) {
    last_.is_updated_this_cycle = true;
    last_.pattern_idx = pattern_idx_;
    last_.is_keyframe = false;
  }
  if (frame_config.arf_buffer_flags & BufferFlags::kUpdate) {
    arf_.is_updated_this_cycle = true;
    arf_.pattern_idx = pattern_idx_;
    arf_.is_keyframe = false;
  }
  if (frame_config.golden_buffer_flags & BufferFlags::kUpdate) {
    golden_.is_updated_this_cycle = true;
    golden_.pattern_idx = pattern_idx_;
    golden_.is_keyframe = false;
  }
  return true;
}

}  // namespace webrtc
