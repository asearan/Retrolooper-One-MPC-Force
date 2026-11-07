// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <atomic>
#include <cstdint>

namespace retrolooper
{
/**
 * Session tempo ownership. Audio-thread safe (atomics only, no mutex / heap).
 *
 * Empty: no master loop yet — UI/click BPM is a preview and detections may adopt.
 * SyncSet: first live take or first import has locked master BPM; later detections must not steal it.
 */
enum class TempoSyncState : std::uint8_t
{
    Empty = 0,
    SyncSet = 1
};

class TempoManager
{
  public:
    static constexpr float kDefaultBpm = 120.0f;
    static constexpr float kMinBpm = 40.0f;
    static constexpr float kMaxBpm = 300.0f;

    [[nodiscard]] float masterBpm() const noexcept;
    [[nodiscard]] TempoSyncState state() const noexcept;
    [[nodiscard]] bool isLocked() const noexcept;

    /** UI / tap / MIDI — whole BPM only. Does not change Empty vs SyncSet. */
    void setMasterBpm(float bpm) noexcept;
    /** Live-loop / restore — clamp only, fractions allowed. */
    void setMasterBpmExact(float bpm) noexcept;

    /**
     * First detected session tempo: stores BPM (fractions kept) and transitions Empty → SyncSet.
     * No-op (returns false) if already locked.
     */
    bool lockDetectedTempo(float bpm) noexcept;

    /** All loops gone — detections may adopt again. Last BPM is kept for the click. */
    void resetToEmpty() noexcept;
    /** Restore a previously locked session tempo (Undo All after Clear). */
    void restoreLockedTempo(float bpm) noexcept;

  private:
    [[nodiscard]] static float sanitizeBpm(float bpm, bool allowFraction) noexcept;

    std::atomic<float> masterBpm_{kDefaultBpm};
    std::atomic<TempoSyncState> state_{TempoSyncState::Empty};
};
} // namespace retrolooper
