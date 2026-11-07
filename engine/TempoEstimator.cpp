// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#include "TempoEstimator.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace retrolooper
{
namespace
{
constexpr int kHop = 512;
constexpr std::size_t kMinSamples = 4096;

[[nodiscard]] inline float fastAbs(const float x) noexcept
{
    return x >= 0.0f ? x : -x;
}

/** Single-pole low-pass; alpha in (0,1]. */
[[nodiscard]] inline float onePoleLowpass(const float x, float& z, const float alpha) noexcept
{
    z += alpha * (x - z);
    return z;
}

/** Multi-band onset proxy: low/mid/high envelopes blended into positive spectral-flux style onset strength. */
void buildOnsetEnvelope(const float* mono, const std::size_t numSamples, std::vector<float>& outFlux) noexcept
{
    const int nHops = static_cast<int>(static_cast<std::int64_t>(numSamples) / kHop);
    outFlux.assign(static_cast<std::size_t>(nHops), 0.0f);
    if (nHops <= 1)
    {
        return;
    }

    // Coarse crossover behavior: LP@~200Hz + LP@~1200Hz -> low / mid / high.
    float lp200 = 0.0f;
    float lp1200 = 0.0f;
    float prevLow = 0.0f;
    float prevMid = 0.0f;
    float prevHigh = 0.0f;
    constexpr float a200 = 0.020f;
    constexpr float a1200 = 0.110f;

    for (int h = 0; h < nHops; ++h)
    {
        const std::size_t base = static_cast<std::size_t>(h) * static_cast<std::size_t>(kHop);
        const int n = std::min(kHop, static_cast<int>(numSamples - base));
        if (n <= 0)
        {
            break;
        }

        double lowE = 0.0;
        double midE = 0.0;
        double highE = 0.0;
        for (int s = 0; s < n; ++s)
        {
            const float x = mono[base + static_cast<std::size_t>(s)];
            const float low = onePoleLowpass(x, lp200, a200);
            const float lowMid = onePoleLowpass(x, lp1200, a1200);
            const float mid = lowMid - low;
            const float high = x - lowMid;
            lowE += static_cast<double>(fastAbs(low));
            midE += static_cast<double>(fastAbs(mid));
            highE += static_cast<double>(fastAbs(high));
        }

        const float lowRms = static_cast<float>(lowE / static_cast<double>(n));
        const float midRms = static_cast<float>(midE / static_cast<double>(n));
        const float highRms = static_cast<float>(highE / static_cast<double>(n));

        const float dLow = std::max(0.0f, lowRms - prevLow);
        const float dMid = std::max(0.0f, midRms - prevMid);
        const float dHigh = std::max(0.0f, highRms - prevHigh);
        prevLow = lowRms;
        prevMid = midRms;
        prevHigh = highRms;

        // Weight transients (mid/high) higher than low-end.
        outFlux[static_cast<std::size_t>(h)] = 0.25f * dLow + 0.45f * dMid + 0.30f * dHigh;
    }
}

[[nodiscard]] double autocorrAtLag(const std::vector<float>& onset, const int lag) noexcept
{
    const int n = static_cast<int>(onset.size()) - lag;
    if (n <= 2)
    {
        return 0.0;
    }
    double s = 0.0;
    for (int i = lag; i < static_cast<int>(onset.size()); ++i)
    {
        s += static_cast<double>(onset[static_cast<std::size_t>(i)] * onset[static_cast<std::size_t>(i - lag)]);
    }
    return s / static_cast<double>(n);
}

[[nodiscard]] double lagFromBpm(const double bpm, const double sampleRate) noexcept
{
    return (60.0 / bpm) * sampleRate / static_cast<double>(kHop);
}

/** Prior favoring BPM close to hint, but with soft tolerance for 1/2 and 2x relationships. */
[[nodiscard]] double tempoPriorWeight(const double bpm, const double hintBpm) noexcept
{
    if (hintBpm <= 1.0)
    {
        return 1.0;
    }
    const auto relErr = [](double a, double b) noexcept
    {
        return std::abs(a - b) / std::max(1.0, b);
    };
    const double e0 = relErr(bpm, hintBpm);
    const double eHalf = relErr(bpm, hintBpm * 0.5);
    const double eDouble = relErr(bpm, hintBpm * 2.0);
    const double e = std::min(e0, std::min(eHalf, eDouble));
    // ~0.2 error still gets non-zero support; tighter around exact family.
    return std::exp(-6.0 * e);
}

/** True when lag is ~2× or ~½× ref (same tempo family at another octave). */
[[nodiscard]] bool isOctaveRelatedLag(const int lag, const int ref) noexcept
{
    if (lag <= 0 || ref <= 0)
    {
        return false;
    }
    const int tol = std::max(2, std::min(lag, ref) / 12);
    return std::abs(lag * 2 - ref) <= tol || std::abs(lag - ref * 2) <= tol;
}

[[nodiscard]] int nearestHarmonicLag(const int lag, const int refLag) noexcept
{
    if (refLag <= 0 || lag <= 0)
    {
        return lag;
    }
    const double ratio = static_cast<double>(lag) / static_cast<double>(refLag);
    if (ratio < 0.75)
    {
        return lag * 2;
    }
    if (ratio > 1.5)
    {
        return lag / 2;
    }
    return lag;
}
} // namespace

TempoEstimateResult estimateTempoFromMonoAudio(const float* mono,
    const std::size_t numSamples,
    const double sampleRate,
    const double minBpm,
    const double maxBpm,
    const int countsPerBar) noexcept
{
    TempoEstimateResult out{};
    if (mono == nullptr || numSamples < kMinSamples || sampleRate < 8000.0 || minBpm >= maxBpm)
    {
        return out;
    }
    const int barCounts = std::clamp(countsPerBar, 1, 16);

    const int nHops = static_cast<int>(static_cast<std::int64_t>(numSamples) / kHop);
    if (nHops < 8)
    {
        return out;
    }

    std::vector<float> flux;
    buildOnsetEnvelope(mono, numSamples, flux);
    if (flux.size() < 8)
    {
        return out;
    }
    double meanFlux = 0.0;
    for (const float f : flux)
    {
        meanFlux += static_cast<double>(f);
    }
    meanFlux /= static_cast<double>(flux.size());

    int lagMin = static_cast<int>(std::floor((60.0 / maxBpm) * sampleRate / static_cast<double>(kHop)));
    int lagMax = static_cast<int>(std::ceil((60.0 / minBpm) * sampleRate / static_cast<double>(kHop)));
    lagMin = std::max(2, lagMin);
    lagMax = std::min(static_cast<int>(flux.size()) - 1, lagMax);
    if (lagMin >= lagMax)
    {
        return out;
    }

    std::vector<double> ac(static_cast<std::size_t>(lagMax + 1), 0.0);
    for (int lag = lagMin; lag <= lagMax; ++lag)
    {
        ac[static_cast<std::size_t>(lag)] = autocorrAtLag(flux, lag);
    }

    // Constrained BPM hypothesis scoring with harmonic disambiguation.
    // Score = autocorr strength + mild smoothness + prior against half/double ambiguity.
    struct Hypothesis
    {
        int lag = 0;
        double score = 0.0;
        double bpm = 0.0;
    };

    // First pass best by raw AC.
    int bestLagRaw = lagMin;
    double bestRaw = ac[static_cast<std::size_t>(lagMin)];
    for (int lag = lagMin + 1; lag <= lagMax; ++lag)
    {
        const double v = ac[static_cast<std::size_t>(lag)];
        if (v > bestRaw)
        {
            bestRaw = v;
            bestLagRaw = lag;
        }
    }
    const double bestBpmRaw = 60.0 / (static_cast<double>(bestLagRaw * kHop) / sampleRate);

    Hypothesis best{};
    for (int lag = lagMin; lag <= lagMax; ++lag)
    {
        const double bpm = 60.0 / (static_cast<double>(lag * kHop) / sampleRate);
        const double prior = tempoPriorWeight(bpm, bestBpmRaw);
        const int nearLag = nearestHarmonicLag(lag, bestLagRaw);
        const int safeNearLag = std::clamp(nearLag, lagMin, lagMax);
        const double harmonicBonus =
            (safeNearLag == lag) ? 0.0 : 0.08 * ac[static_cast<std::size_t>(safeNearLag)];
        const double smoothness = 0.5 * (ac[static_cast<std::size_t>(std::max(lagMin, lag - 1))]
                                        + ac[static_cast<std::size_t>(std::min(lagMax, lag + 1))]);
        double barBonus = 0.0;
        const int barLag = lag * barCounts;
        if (barLag <= lagMax)
        {
            barBonus = 0.28 * ac[static_cast<std::size_t>(barLag)];
        }
        const double score =
            (0.76 * ac[static_cast<std::size_t>(lag)] + 0.16 * smoothness + harmonicBonus + barBonus) * prior;
        if (score > best.score || best.lag == 0)
        {
            best = {lag, score, bpm};
        }
    }
    int bestLag = best.lag;
    if (bestLag <= 0)
    {
        return out;
    }

    // Octave disambiguation: the tallest AC peak is often the first subharmonic (half BPM). The true
    // fundamental can sit below a global threshold, so compare only a harmonic ladder around the
    // best-scoring lag using autocorr × BPM (favor faster tempos when peaks are comparable).
    {
        std::vector<int> cands;
        auto pushCand = [&](int x)
        {
            if (x < lagMin || x > lagMax || x <= 0)
            {
                return;
            }
            if (std::find(cands.begin(), cands.end(), x) != cands.end())
            {
                return;
            }
            cands.push_back(x);
        };
        pushCand(bestLag);
        pushCand(bestLagRaw);
        for (const int seed : { bestLag, bestLagRaw })
        {
            for (int x = seed; x >= lagMin;)
            {
                if (x < 4)
                {
                    break;
                }
                x /= 2;
                pushCand(x);
            }
            for (int x = seed; x <= lagMax && x > 0;)
            {
                const int next = x * 2;
                if (next > lagMax || next <= x)
                {
                    break;
                }
                x = next;
                pushCand(x);
            }
        }

        int chosen = bestLag;
        double bestWeighted = -1.0;
        for (const int lag : cands)
        {
            const double v = ac[static_cast<std::size_t>(lag)];
            const double bpmCand = 60.0 / (static_cast<double>(lag * kHop) / sampleRate);
            // Prefer musically mid tempos slightly; bpm factor resists half-time locks.
            const double midPrior = std::exp(-0.5 * std::pow((std::log(std::max(1.0, bpmCand)) - std::log(110.0)) / 0.65, 2.0));
            const double weighted = v * bpmCand * (0.82 + 0.18 * midPrior);
            if (weighted > bestWeighted)
            {
                bestWeighted = weighted;
                chosen = lag;
            }
        }

        // Prefer the quarter-note pulse (division 1). Only promote an 8th-note octave when that
        // half-lag peak is nearly as strong as the beat lag — keeps 1-bar / 4-count detections stable.
        while (chosen >= 4)
        {
            const int fastLag = chosen / 2;
            if (fastLag < lagMin || fastLag > lagMax)
            {
                break;
            }
            const double fastBpm = 60.0 / (static_cast<double>(fastLag * kHop) / sampleRate);
            if (fastBpm > 168.0)
            {
                break;
            }
            const double acSlow = ac[static_cast<std::size_t>(chosen)];
            const double acFast = ac[static_cast<std::size_t>(fastLag)];
            const int barSlow = chosen * barCounts;
            const int barFast = fastLag * barCounts;
            const double barSlowAc = (barSlow <= lagMax) ? ac[static_cast<std::size_t>(barSlow)] : 0.0;
            const double barFastAc = (barFast <= lagMax) ? ac[static_cast<std::size_t>(barFast)] : 0.0;
            if (acFast >= 0.88 * std::max(1.0e-12, acSlow) && barFastAc >= 0.92 * std::max(1.0e-12, barSlowAc))
            {
                chosen = fastLag;
                continue;
            }
            break;
        }
        bestLag = chosen;
    }

    const double bestVal = ac[static_cast<std::size_t>(bestLag)];

    const double periodSec = static_cast<double>(bestLag * kHop) / sampleRate;
    if (periodSec <= 1.0e-6)
    {
        return out;
    }
    const double bpm = 60.0 / periodSec;
    if (!std::isfinite(bpm) || bpm < minBpm * 0.5 || bpm > maxBpm * 1.5)
    {
        return out;
    }

    double maxNeighbour = 0.0;
    const int w = std::max(1, (lagMax - lagMin) / 32);
    for (int lag = lagMin; lag <= lagMax; ++lag)
    {
        if (std::abs(lag - bestLag) <= w)
        {
            continue;
        }
        if (isOctaveRelatedLag(lag, bestLag))
        {
            continue;
        }
        maxNeighbour = std::max(maxNeighbour, ac[static_cast<std::size_t>(lag)]);
    }

    const double denom = maxNeighbour + meanFlux * 1.0e-3 + 1.0e-9;
    const double confidence = std::clamp(bestVal / denom / 2.6, 0.0, 1.0);

    // Beat phase estimate: choose offset in hop-domain maximizing onset energy at periodic beat instants.
    int bestPhaseHop = 0;
    double bestPhaseScore = -1.0;
    for (int phase = 0; phase < bestLag; ++phase)
    {
        double s = 0.0;
        for (int i = phase; i < static_cast<int>(flux.size()); i += bestLag)
        {
            s += flux[static_cast<std::size_t>(i)];
        }
        if (s > bestPhaseScore)
        {
            bestPhaseScore = s;
            bestPhaseHop = phase;
        }
    }

    out.bpm = bpm;
    out.confidence = confidence;
    out.beatPeriodSamples = periodSec * sampleRate;
    out.phaseOffsetSamples = static_cast<double>(bestPhaseHop * kHop);
    return out;
}
} // namespace retrolooper
