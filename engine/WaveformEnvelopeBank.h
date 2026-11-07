// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "Track.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cmath>

namespace retrolooper
{
/** Matches {@code track_visual::kEnvelopeBins}. Fixed-size so the audio thread never allocates. */
inline constexpr int kWaveformEnvelopeBins = 256;

/**
 * Audio-thread waveform overview: strided min/max fold of each loop into 256 bins, published
 * with a double buffer so the UI never reads a torn frame. Recording uses a frozen span so
 * bins do not remap (swim) as the take grows.
 */
class WaveformEnvelopeBank
{
  public:
    static constexpr int kTracks = 8;

    WaveformEnvelopeBank() noexcept
    {
        for (int t = 0; t < kTracks; ++t)
        {
            workMin_[static_cast<std::size_t>(t)].fill(0.f);
            workMax_[static_cast<std::size_t>(t)].fill(0.f);
            publishedMinPages_[static_cast<std::size_t>(t)][0].fill(0.f);
            publishedMinPages_[static_cast<std::size_t>(t)][1].fill(0.f);
            publishedMaxPages_[static_cast<std::size_t>(t)][0].fill(0.f);
            publishedMaxPages_[static_cast<std::size_t>(t)][1].fill(0.f);
            publishedPage_[static_cast<std::size_t>(t)].store(0, std::memory_order_relaxed);
            epoch_[static_cast<std::size_t>(t)].store(0, std::memory_order_relaxed);
            lastFoldLen_[static_cast<std::size_t>(t)] = 0;
            captureLen_[static_cast<std::size_t>(t)] = 0;
            captureSpan_[static_cast<std::size_t>(t)] = 0;
            overdubFoldCounter_[static_cast<std::size_t>(t)] = 0;
        }
    }

    void rebuildFromTrack(int trackIndex, const Track& tr, std::size_t hintSpan = 0, bool force = false) noexcept
    {
        if (trackIndex < 0 || trackIndex >= kTracks)
        {
            return;
        }
        const auto ti = static_cast<std::size_t>(trackIndex);
        auto& destMin = workMin_[ti];
        auto& destMax = workMax_[ti];
        const auto st = tr.state();
        const bool recording = st == TrackState::Recording;
        const bool overdub = trackStateIsOverdub(st);
        const std::size_t n = recording ? tr.recordedSamples() : tr.lengthSamples();

        if (!recording)
        {
            const bool finishCapture = captureSpan_[ti] != 0;
            captureLen_[ti] = 0;
            captureSpan_[ti] = 0;
            if (!force && !overdub && !finishCapture && n == lastFoldLen_[ti] && n >= 2)
            {
                overdubFoldCounter_[ti] = 0;
                return;
            }
            if (!force && overdub && !finishCapture && n == lastFoldLen_[ti] && n >= 2)
            {
                // Full-loop refold is enough at ~UI rate; every audio block starves ASIO.
                if ((overdubFoldCounter_[ti]++ % 8) != 0)
                {
                    return;
                }
            }
            else
            {
                overdubFoldCounter_[ti] = 0;
            }
            lastFoldLen_[ti] = n;
            if (n < 2)
            {
                destMin.fill(0.f);
                destMax.fill(0.f);
                publish(ti);
                return;
            }
            foldRange(tr, 0, n, n, destMin, destMax);
            publish(ti);
            return;
        }

        overdubFoldCounter_[ti] = 0;
        lastFoldLen_[ti] = n;
        if (n < 2)
        {
            destMin.fill(0.f);
            destMax.fill(0.f);
            captureLen_[ti] = 0;
            captureSpan_[ti] = 0;
            publish(ti);
            return;
        }

        if (captureLen_[ti] > n)
        {
            destMin.fill(0.f);
            destMax.fill(0.f);
            captureLen_[ti] = 0;
            captureSpan_[ti] = 0;
        }

        std::size_t span = captureSpan_[ti];
        if (span == 0)
        {
            span = std::max(n, hintSpan > 1 ? hintSpan : n);
        }
        if (n > span)
        {
            span = n;
            foldRange(tr, 0, n, span, destMin, destMax);
            captureLen_[ti] = n;
            captureSpan_[ti] = span;
            publish(ti);
            return;
        }

        if (captureLen_[ti] == 0)
        {
            foldRange(tr, 0, n, span, destMin, destMax);
        }
        else if (n > captureLen_[ti])
        {
            accumulateSpan(tr, captureLen_[ti], n, span, destMin, destMax);
        }
        captureLen_[ti] = n;
        captureSpan_[ti] = span;
        publish(ti);
    }

    /** Peak magnitude per bin (max of |min|, |max|) — tests and simple meters. */
    void copyPublished(int trackIndex, float* dest, int n) const noexcept
    {
        if (dest == nullptr || n <= 0 || trackIndex < 0 || trackIndex >= kTracks)
        {
            return;
        }
        const auto ti = static_cast<std::size_t>(trackIndex);
        const int page = publishedPage_[ti].load(std::memory_order_acquire) & 1;
        const auto& srcMin = publishedMinPages_[ti][static_cast<std::size_t>(page)];
        const auto& srcMax = publishedMaxPages_[ti][static_cast<std::size_t>(page)];
        const int bins = n < kWaveformEnvelopeBins ? n : kWaveformEnvelopeBins;
        for (int i = 0; i < bins; ++i)
        {
            dest[i] = std::max(std::abs(srcMin[static_cast<std::size_t>(i)]),
                std::abs(srcMax[static_cast<std::size_t>(i)]));
        }
        for (int i = bins; i < n; ++i)
        {
            dest[i] = 0.f;
        }
    }

    void copyPublishedMinMax(int trackIndex, float* mins, float* maxs, int n) const noexcept
    {
        if (mins == nullptr || maxs == nullptr || n <= 0 || trackIndex < 0 || trackIndex >= kTracks)
        {
            return;
        }
        const auto ti = static_cast<std::size_t>(trackIndex);
        const int page = publishedPage_[ti].load(std::memory_order_acquire) & 1;
        const auto& srcMin = publishedMinPages_[ti][static_cast<std::size_t>(page)];
        const auto& srcMax = publishedMaxPages_[ti][static_cast<std::size_t>(page)];
        const int bins = n < kWaveformEnvelopeBins ? n : kWaveformEnvelopeBins;
        for (int i = 0; i < bins; ++i)
        {
            mins[i] = srcMin[static_cast<std::size_t>(i)];
            maxs[i] = srcMax[static_cast<std::size_t>(i)];
        }
        for (int i = bins; i < n; ++i)
        {
            mins[i] = 0.f;
            maxs[i] = 0.f;
        }
    }

    [[nodiscard]] bool trackNeedsLivePublish(int trackIndex, const Track& tr) const noexcept
    {
        if (trackIndex < 0 || trackIndex >= kTracks)
        {
            return false;
        }
        const auto st = tr.state();
        return st == TrackState::Recording || trackStateIsOverdub(st)
            || captureSpan_[static_cast<std::size_t>(trackIndex)] != 0;
    }

    [[nodiscard]] std::uint32_t epoch(int trackIndex) const noexcept
    {
        if (trackIndex < 0 || trackIndex >= kTracks)
        {
            return 0;
        }
        return epoch_[static_cast<std::size_t>(trackIndex)].load(std::memory_order_acquire);
    }

  private:
    static void accumulateBin(const Track& tr, const std::size_t i0, const std::size_t i1, float& mn, float& mx) noexcept
    {
        const int nCh = tr.channels();
        const std::size_t span = i1 - i0;
        const std::size_t step = (span > 32u) ? (span / 32u) : std::size_t{1};
        mn = 1.0f;
        mx = -1.0f;
        for (std::size_t i = i0; i < i1; i += step)
        {
            const float L = tr.readSample(0, i);
            const float R = (nCh > 1) ? tr.readSample(1, i) : L;
            const float s = 0.5f * (L + R);
            if (s < mn)
            {
                mn = s;
            }
            if (s > mx)
            {
                mx = s;
            }
        }
    }

    static int binForSample(const std::size_t i, const std::size_t span) noexcept
    {
        if (span <= 1)
        {
            return 0;
        }
        const auto b = static_cast<int>((i * static_cast<std::size_t>(kWaveformEnvelopeBins)) / span);
        return std::min(kWaveformEnvelopeBins - 1, std::max(0, b));
    }

    static void foldRange(const Track& tr, const std::size_t begin, const std::size_t end, const std::size_t span,
        std::array<float, kWaveformEnvelopeBins>& destMin, std::array<float, kWaveformEnvelopeBins>& destMax) noexcept
    {
        destMin.fill(0.f);
        destMax.fill(0.f);
        if (end <= begin || span < 2)
        {
            return;
        }
        std::array<float, kWaveformEnvelopeBins> mn{};
        std::array<float, kWaveformEnvelopeBins> mx{};
        mn.fill(1.f);
        mx.fill(-1.f);
        const std::size_t n = end - begin;
        for (int b = 0; b < kWaveformEnvelopeBins; ++b)
        {
            std::size_t i0 = begin + static_cast<std::size_t>(b) * n / static_cast<std::size_t>(kWaveformEnvelopeBins);
            std::size_t i1 = begin + static_cast<std::size_t>(b + 1) * n / static_cast<std::size_t>(kWaveformEnvelopeBins);
            if (span != n)
            {
                i0 = begin + static_cast<std::size_t>(b) * span / static_cast<std::size_t>(kWaveformEnvelopeBins);
                i1 = begin + static_cast<std::size_t>(b + 1) * span / static_cast<std::size_t>(kWaveformEnvelopeBins);
            }
            if (i1 <= i0)
            {
                i1 = i0 + 1;
            }
            if (i0 >= end)
            {
                destMin[static_cast<std::size_t>(b)] = 0.f;
                destMax[static_cast<std::size_t>(b)] = 0.f;
                continue;
            }
            if (i1 > end)
            {
                i1 = end;
            }
            accumulateBin(tr, i0, i1, mn[static_cast<std::size_t>(b)], mx[static_cast<std::size_t>(b)]);
            destMin[static_cast<std::size_t>(b)] = mn[static_cast<std::size_t>(b)];
            destMax[static_cast<std::size_t>(b)] = mx[static_cast<std::size_t>(b)];
        }
    }

    static void accumulateSpan(const Track& tr, const std::size_t begin, const std::size_t end, const std::size_t span,
        std::array<float, kWaveformEnvelopeBins>& destMin, std::array<float, kWaveformEnvelopeBins>& destMax) noexcept
    {
        if (end <= begin || span < 2)
        {
            return;
        }
        const int nCh = tr.channels();
        for (std::size_t i = begin; i < end; ++i)
        {
            const int b = binForSample(i, span);
            const float L = tr.readSample(0, i);
            const float R = (nCh > 1) ? tr.readSample(1, i) : L;
            const float s = 0.5f * (L + R);
            auto& mn = destMin[static_cast<std::size_t>(b)];
            auto& mx = destMax[static_cast<std::size_t>(b)];
            if (mn == 0.f && mx == 0.f)
            {
                mn = s;
                mx = s;
            }
            else
            {
                if (s < mn)
                {
                    mn = s;
                }
                if (s > mx)
                {
                    mx = s;
                }
            }
        }
    }

    void publish(const std::size_t ti) noexcept
    {
        const int cur = publishedPage_[ti].load(std::memory_order_relaxed) & 1;
        const int page = 1 - cur;
        publishedMinPages_[ti][static_cast<std::size_t>(page)] = workMin_[ti];
        publishedMaxPages_[ti][static_cast<std::size_t>(page)] = workMax_[ti];
        publishedPage_[ti].store(page, std::memory_order_release);
        epoch_[ti].fetch_add(1, std::memory_order_release);
    }

    std::array<std::array<float, kWaveformEnvelopeBins>, kTracks> workMin_{};
    std::array<std::array<float, kWaveformEnvelopeBins>, kTracks> workMax_{};
    std::array<std::array<std::array<float, kWaveformEnvelopeBins>, 2>, kTracks> publishedMinPages_{};
    std::array<std::array<std::array<float, kWaveformEnvelopeBins>, 2>, kTracks> publishedMaxPages_{};
    std::array<std::atomic<int>, kTracks> publishedPage_{};
    std::array<std::atomic<std::uint32_t>, kTracks> epoch_{};
    std::array<std::size_t, kTracks> lastFoldLen_{};
    std::array<std::size_t, kTracks> captureLen_{};
    std::array<std::size_t, kTracks> captureSpan_{};
    std::array<int, kTracks> overdubFoldCounter_{};
};
} // namespace retrolooper
