// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#include "TempoManager.h"

#include <algorithm>
#include <cmath>

namespace retrolooper
{
float TempoManager::sanitizeBpm(const float bpm, const bool allowFraction) noexcept
{
    if (!(bpm > 1.0f) || !std::isfinite(bpm))
    {
        return kDefaultBpm;
    }
    const float clamped = std::clamp(bpm, kMinBpm, kMaxBpm);
    if (allowFraction)
    {
        return clamped;
    }
    return std::clamp(std::round(clamped), kMinBpm, kMaxBpm);
}

float TempoManager::masterBpm() const noexcept
{
    return masterBpm_.load(std::memory_order_relaxed);
}

TempoSyncState TempoManager::state() const noexcept
{
    return state_.load(std::memory_order_acquire);
}

bool TempoManager::isLocked() const noexcept
{
    return state() == TempoSyncState::SyncSet;
}

void TempoManager::setMasterBpm(const float bpm) noexcept
{
    masterBpm_.store(sanitizeBpm(bpm, false), std::memory_order_relaxed);
}

void TempoManager::setMasterBpmExact(const float bpm) noexcept
{
    masterBpm_.store(sanitizeBpm(bpm, true), std::memory_order_relaxed);
}

bool TempoManager::lockDetectedTempo(const float bpm) noexcept
{
    auto expected = TempoSyncState::Empty;
    if (!state_.compare_exchange_strong(expected, TempoSyncState::SyncSet, std::memory_order_acq_rel, std::memory_order_acquire))
    {
        return false;
    }
    masterBpm_.store(sanitizeBpm(bpm, true), std::memory_order_relaxed);
    return true;
}

void TempoManager::resetToEmpty() noexcept
{
    state_.store(TempoSyncState::Empty, std::memory_order_release);
}

void TempoManager::restoreLockedTempo(const float bpm) noexcept
{
    masterBpm_.store(sanitizeBpm(bpm, true), std::memory_order_relaxed);
    state_.store(TempoSyncState::SyncSet, std::memory_order_release);
}
} // namespace retrolooper
