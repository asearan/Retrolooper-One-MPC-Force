// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#include "AudioImportAnalysisService.h"

#include "TempoEstimator.h"
#include "Track.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

namespace retrolooper
{
namespace
{
struct Candidate
{
    double bpm = 0.0;
    double score = 0.0;
};

double clampBpm(const double bpm, const AudioImportAnalysisService::Config& cfg) noexcept
{
    return std::clamp(bpm, cfg.minBpm, cfg.maxBpm);
}

double intrinsicFromLength(const std::size_t lenSamples,
    const double sampleRate,
    const double projectTempoBpm,
    const int globalBarLengthBars,
    const double quartersPerNotatedBar) noexcept
{
    if (lenSamples == 0 || sampleRate <= 1.0 || quartersPerNotatedBar <= 0.0)
    {
        return 0.0;
    }
    const double seconds = static_cast<double>(lenSamples) / sampleRate;
    if (seconds <= 1.0e-9)
    {
        return 0.0;
    }

    const double hint = (projectTempoBpm > 1.0) ? projectTempoBpm : 120.0;
    const int barSpan = std::max(1, globalBarLengthBars);
    const int barSpanQuarters =
        std::max(1, static_cast<int>(std::llround(quartersPerNotatedBar * static_cast<double>(barSpan))));

    double bestBpm = 0.0;
    double bestScore = 1.0e18;

    auto consider = [&](const int n) noexcept
    {
        if (n < 1)
        {
            return;
        }
        const double bpm = static_cast<double>(n) * 60.0 / seconds;
        if (bpm < 20.0 || bpm > 360.0)
        {
            return;
        }

        const double bars = static_cast<double>(n) / std::max(0.25, quartersPerNotatedBar);
        const double barsRound = std::round(bars);
        const bool wholeBars = std::abs(bars - barsRound) < 1.0e-6;
        const int ibars = static_cast<int>(barsRound);
        const double relHint = std::abs(bpm - hint) / std::max(1.0, hint);

        double barPen = 3.2;
        if (wholeBars && (ibars == 2 || ibars == 4))
        {
            barPen = 0.0;
        }
        else if (wholeBars && ibars == 1)
        {
            barPen = 0.40;
        }
        else if (wholeBars && ibars == 8)
        {
            // 8-bar is valid, but it is also the usual double-time of a 4-bar loop.
            barPen = 1.85;
        }
        else if (wholeBars && ibars == 16)
        {
            barPen = 2.4;
        }
        else if (wholeBars && (n % barSpanQuarters) == 0)
        {
            barPen = 0.85;
        }
        else if (!wholeBars && relHint < 0.035)
        {
            // Odd phrase lengths (7 quarters, etc.) when they match the session tempo.
            barPen = 0.0;
        }

        double rangePen = 8.0;
        if (bpm >= 58.0 && bpm <= 176.0)
        {
            rangePen = 0.0;
        }
        else if (bpm >= 48.0 && bpm <= 200.0)
        {
            rangePen = 0.55;
        }
        else if (bpm >= 20.0 && bpm <= 240.0)
        {
            rangePen = 2.2;
        }

        const double midPen = std::abs(std::log(std::max(1.0, bpm) / 100.0)) * 0.22;
        const double hintPen = relHint * 0.85;
        const double score = barPen + rangePen + midPen + hintPen;
        if (score < bestScore - 1.0e-9)
        {
            bestScore = score;
            bestBpm = bpm;
        }
    };

    // Power-of-two measure spans capped by the configured global bar length.
    for (int bars = barSpan; bars >= 1; bars /= 2)
    {
        consider(std::max(1, static_cast<int>(std::llround(quartersPerNotatedBar * static_cast<double>(bars)))));
    }

    // Odd phrase lengths (e.g. 7 quarters): search near the hint-implied quarter count,
    // but only for musically plausible meters — not 27/4 just because 120 × duration ≈ 27.
    auto plausibleOddMeter = [](const int n) noexcept
    {
        switch (n)
        {
            case 3:
            case 5:
            case 6:
            case 7:
            case 9:
            case 10:
            case 11:
            case 12:
            case 14:
            case 15:
            case 18:
            case 21:
                return true;
            default:
                return false;
        }
    };
    const int nHint = std::max(1, static_cast<int>(std::llround(hint * seconds / 60.0)));
    const int qRound = std::max(1, static_cast<int>(std::llround(quartersPerNotatedBar)));
    for (int n = std::max(1, nHint - 12); n <= nHint + 12; ++n)
    {
        const bool wholeBars = (n % qRound) == 0;
        if (!wholeBars && !plausibleOddMeter(n))
        {
            continue;
        }
        consider(n);
    }

    return bestBpm;
}

void mergeCandidate(std::vector<Candidate>& out, const double bpm, const double score) noexcept
{
    if (!std::isfinite(bpm) || bpm <= 0.0 || score <= 0.0)
    {
        return;
    }
    for (auto& c : out)
    {
        if (std::abs(c.bpm - bpm) <= 0.8)
        {
            c.score += score;
            c.bpm = 0.5 * (c.bpm + bpm);
            return;
        }
    }
    out.push_back({bpm, score});
}

void normalizeHalfDouble(std::vector<Candidate>& candidates, const AudioImportAnalysisService::Config& cfg) noexcept
{
    std::vector<Candidate> expanded;
    expanded.reserve(candidates.size() * 3);
    for (const auto& c : candidates)
    {
        const double b = clampBpm(c.bpm, cfg);
        mergeCandidate(expanded, b, c.score);
        if (b * 2.0 <= cfg.maxBpm + 0.001)
        {
            mergeCandidate(expanded, b * 2.0, c.score * 0.50);
        }
        if (b * 0.5 >= cfg.minBpm - 0.001)
        {
            mergeCandidate(expanded, b * 0.5, c.score * 0.62);
        }
    }
    candidates = std::move(expanded);
}

double selectionWeightForContent(const Candidate& c, const ImportContentType contentType, const double projectTempoBpm) noexcept
{
    double w = 1.0;
    if (contentType == ImportContentType::RhythmicLoop || contentType == ImportContentType::StronglyPercussiveLoop)
    {
        if (c.bpm < 50.0)
        {
            w *= 0.82;
        }
        else if (c.bpm > 185.0)
        {
            w *= 0.72;
        }
        else if (c.bpm > 168.0)
        {
            w *= 0.90;
        }
    }
    if (projectTempoBpm > 1.0)
    {
        const double rel = std::abs(c.bpm - projectTempoBpm) / std::max(1.0, projectTempoBpm);
        w *= std::clamp(1.0 - rel * 0.28, 0.82, 1.08);
    }
    return w;
}

double octaveAwareDistance(const double a, const double b) noexcept
{
    if (a <= 0.0 || b <= 0.0)
    {
        return 1.0e9;
    }
    const double d0 = std::abs(a - b);
    const double dHalf = std::abs(a * 0.5 - b);
    const double dDouble = std::abs(a * 2.0 - b);
    return std::min(d0, std::min(dHalf, dDouble));
}

double normalizeToQuarterNotePulse(double bpm, const AudioImportAnalysisService::Config& cfg) noexcept
{
    // Fold only extreme 8th/16th (or whole-note) detections. A 70 BPM loop must not become 140
    // just because 140 sits closer to a "typical pop" band.
    constexpr double kQuarterPrefHi = 185.0;
    constexpr double kQuarterPrefLo = 55.0;
    while (bpm > kQuarterPrefHi && (bpm * 0.5) >= std::max(cfg.minBpm, kQuarterPrefLo))
    {
        bpm *= 0.5;
    }
    while (bpm < kQuarterPrefLo && (bpm * 2.0) <= std::min(cfg.maxBpm, kQuarterPrefHi))
    {
        bpm *= 2.0;
    }
    return std::clamp(bpm, cfg.minBpm, cfg.maxBpm);
}

double loopLengthFitError(const double bpm, const double seconds) noexcept
{
    if (bpm <= 0.0 || seconds <= 0.0)
    {
        return 1.0e9;
    }
    const double beats = seconds * bpm / 60.0;
    const double nearest = std::round(beats);
    if (nearest < 1.0)
    {
        return 1.0e9;
    }
    return std::abs(beats - nearest) / nearest;
}

double snapOctaveToLoopLength(const double bpm, const double seconds, const AudioImportAnalysisService::Config& cfg) noexcept
{
    const double cands[3] = {bpm * 0.5, bpm, bpm * 2.0};
    double best = bpm;
    double bestErr = loopLengthFitError(bpm, seconds);
    for (const double c : cands)
    {
        if (c < cfg.minBpm - 0.001 || c > cfg.maxBpm + 0.001)
        {
            continue;
        }
        const double err = loopLengthFitError(c, seconds);
        // Switch only when the other octave fits the file length clearly better — never on a tie
        // (4 bars at 70 and 8 bars at 140 both land on integer beats).
        if (err + 1.0e-9 < bestErr * 0.45 && err < 0.02)
        {
            bestErr = err;
            best = c;
        }
    }
    return best;
}

void classifyMaterial(const std::vector<float>& mono, TempoDetectionResult& out) noexcept
{
    if (mono.empty())
    {
        out.contentType = ImportContentType::OneShotOrNonLoop;
        out.isLikelyLoop = false;
        out.loopQuality = 0.0;
        return;
    }
    double sumAbs = 0.0;
    double zcr = 0.0;
    double transients = 0.0;
    float prev = mono.front();
    for (std::size_t i = 1; i < mono.size(); ++i)
    {
        const float x = mono[i];
        sumAbs += std::abs(x);
        if ((x >= 0.0f) != (prev >= 0.0f))
        {
            zcr += 1.0;
        }
        const float d = x - prev;
        if (std::abs(d) > 0.12f)
        {
            transients += 1.0;
        }
        prev = x;
    }
    const double n = static_cast<double>(std::max<std::size_t>(1, mono.size() - 1));
    const double avgAbs = sumAbs / n;
    const double zcrRate = zcr / n;
    const double transientRate = transients / n;
    out.loopQuality = std::clamp((transientRate * 8.0 + avgAbs * 1.2), 0.0, 1.0);

    if (transientRate < 0.002 && avgAbs < 0.025)
    {
        out.contentType = ImportContentType::AmbientTexture;
        out.isLikelyLoop = false;
        return;
    }
    if (transientRate < 0.0035)
    {
        out.contentType = ImportContentType::MelodicHarmonicLoop;
        out.isLikelyLoop = true;
        return;
    }
    if (zcrRate < 0.06 && transientRate > 0.008)
    {
        out.contentType = ImportContentType::StronglyPercussiveLoop;
        out.isLikelyLoop = true;
        return;
    }
    if (avgAbs < 0.02 && transientRate < 0.006)
    {
        out.contentType = ImportContentType::OneShotOrNonLoop;
        out.isLikelyLoop = false;
        return;
    }
    out.contentType = ImportContentType::RhythmicLoop;
    out.isLikelyLoop = true;
}
} // namespace

std::vector<float> AudioImportAnalysisService::buildMono(const Track& track, const std::size_t maxSamples) noexcept
{
    const std::size_t len = std::min(maxSamples, track.lengthSamples());
    std::vector<float> mono(len, 0.0f);
    if (len == 0)
    {
        return mono;
    }
    const int ch = track.channels();
    for (std::size_t i = 0; i < len; ++i)
    {
        const float l = track.readSample(0, i);
        const float r = track.readSample(ch > 1 ? 1 : 0, i);
        mono[i] = 0.5f * (l + r);
    }
    return mono;
}

TempoDetectionResult AudioImportAnalysisService::analyzeTrack(const Track& track,
    const double sampleRate,
    const double projectTempoBpm,
    const int globalBarLengthBars,
    const double quartersPerNotatedBar,
    const Config& cfg) noexcept
{
    TempoDetectionResult out{};
    const std::size_t len = track.lengthSamples();
    if (len == 0 || sampleRate < 8000.0)
    {
        return out;
    }

    const std::size_t maxSamples =
        static_cast<std::size_t>(std::min(static_cast<double>(len), cfg.maxAnalysisSeconds * sampleRate));
    auto mono = buildMono(track, maxSamples);
    if (mono.size() < 4096)
    {
        return out;
    }

    // Intrinsic tempo is a length/notation property of the loop span — use the full
    // buffer duration, not silence-trimmed content (silent restoreTrack fixtures etc.).
    out.intrinsicBpm = intrinsicFromLength(maxSamples,
        sampleRate,
        projectTempoBpm,
        globalBarLengthBars,
        quartersPerNotatedBar);

    // boundary-aware trim for analysis only
    std::size_t first = 0;
    std::size_t last = mono.size() - 1;
    while (first + 1 < mono.size() && std::abs(mono[first]) < cfg.silenceThresholdLinear)
    {
        ++first;
    }
    while (last > first + 1 && std::abs(mono[last]) < cfg.silenceThresholdLinear)
    {
        --last;
    }
    out.estimatedLoopLengthSeconds = static_cast<double>(last - first + 1) / sampleRate;
    out.detectedStartSeconds = static_cast<double>(first) / sampleRate;
    out.detectedEndSeconds = static_cast<double>(last) / sampleRate;

    if (last <= first + 2048)
    {
        out.contentType = ImportContentType::OneShotOrNonLoop;
        out.isLikelyLoop = false;
        return out;
    }

    std::vector<float> trimmed(mono.begin() + static_cast<std::ptrdiff_t>(first),
        mono.begin() + static_cast<std::ptrdiff_t>(last + 1));
    classifyMaterial(trimmed, out);

    const int countsPerBar =
        std::clamp(static_cast<int>(std::llround(std::max(1.0, quartersPerNotatedBar))), 1, 16);
    std::vector<Candidate> candidates;
    const auto full = estimateTempoFromMonoAudio(
        trimmed.data(), trimmed.size(), sampleRate, cfg.minBpm, cfg.maxBpm, countsPerBar);
    mergeCandidate(candidates, full.bpm, full.confidence);

    // Multi-window consistency candidates.
    const std::size_t half = trimmed.size() / 2;
    if (half > 4096)
    {
        const auto a = estimateTempoFromMonoAudio(
            trimmed.data(), half, sampleRate, cfg.minBpm, cfg.maxBpm, countsPerBar);
        const auto b = estimateTempoFromMonoAudio(trimmed.data() + static_cast<std::ptrdiff_t>(trimmed.size() - half),
            half,
            sampleRate,
            cfg.minBpm,
            cfg.maxBpm,
            countsPerBar);
        mergeCandidate(candidates, a.bpm, a.confidence * 0.85);
        mergeCandidate(candidates, b.bpm, b.confidence * 0.85);
    }

    if (out.intrinsicBpm > 0.0)
    {
        mergeCandidate(candidates, out.intrinsicBpm, 0.65);
    }
    normalizeHalfDouble(candidates, cfg);

    if (candidates.empty())
    {
        out.confidence = 0.0;
        return out;
    }
    std::sort(candidates.begin(), candidates.end(), [&](const Candidate& a, const Candidate& b)
    {
        const double sa = a.score * selectionWeightForContent(a, out.contentType, projectTempoBpm);
        const double sb = b.score * selectionWeightForContent(b, out.contentType, projectTempoBpm);
        return sa > sb;
    });
    const double maxScore = std::max(1.0e-9, candidates.front().score);
    const std::size_t useN = std::min<std::size_t>(4, candidates.size());
    out.candidateBpms.clear();
    for (std::size_t i = 0; i < useN; ++i)
    {
        out.candidateBpms.push_back(std::clamp(candidates[i].bpm, cfg.minBpm, cfg.maxBpm));
    }

    // Choose default BPM by blending confidence rank with intrinsic/project anchors to reduce half/double-time errors.
    std::size_t bestIdx = 0;
    double bestFinal = -1.0e18;
    for (std::size_t i = 0; i < useN; ++i)
    {
        const double bpm = out.candidateBpms[i];
        double s = candidates[i].score * selectionWeightForContent(candidates[i], out.contentType, projectTempoBpm);
        if (out.intrinsicBpm > 1.0)
        {
            const double di = octaveAwareDistance(bpm, out.intrinsicBpm);
            s *= std::clamp(1.20 - (di / 80.0), 0.78, 1.24);
        }
        if (projectTempoBpm > 1.0)
        {
            const double dp = octaveAwareDistance(bpm, projectTempoBpm);
            s *= std::clamp(1.12 - (dp / 120.0), 0.85, 1.12);
        }
        // For rhythmic/percussive content, avoid promoting likely double-time over strong quarter-note candidates.
        if ((out.contentType == ImportContentType::RhythmicLoop || out.contentType == ImportContentType::StronglyPercussiveLoop)
            && bpm > 185.0)
        {
            s *= 0.78;
        }
        if (s > bestFinal)
        {
            bestFinal = s;
            bestIdx = i;
        }
    }
    out.detectedBpm = out.candidateBpms[bestIdx];
    if (out.contentType == ImportContentType::RhythmicLoop
        || out.contentType == ImportContentType::StronglyPercussiveLoop
        || out.contentType == ImportContentType::MelodicHarmonicLoop)
    {
        out.detectedBpm = normalizeToQuarterNotePulse(out.detectedBpm, cfg);
        if (out.estimatedLoopLengthSeconds > 0.05)
        {
            out.detectedBpm = snapOctaveToLoopLength(out.detectedBpm, out.estimatedLoopLengthSeconds, cfg);
        }
    }
    out.halfTimeCandidate = (out.detectedBpm * 0.5 >= cfg.minBpm) ? out.detectedBpm * 0.5 : 0.0;
    out.doubleTimeCandidate = (out.detectedBpm * 2.0 <= cfg.maxBpm) ? out.detectedBpm * 2.0 : 0.0;

    const double agreement = (useN > 1)
        ? std::clamp(1.0 - (std::abs(out.candidateBpms[0] - out.candidateBpms[1]) / 40.0), 0.0, 1.0)
        : 0.75;
    const double peakStrength = std::clamp(candidates.front().score / maxScore, 0.0, 1.0);
    const double materialPenalty = (out.contentType == ImportContentType::AmbientTexture
                                       || out.contentType == ImportContentType::OneShotOrNonLoop)
        ? 0.45
        : 1.0;
    out.confidence = std::clamp((0.58 * candidates.front().score + 0.22 * agreement + 0.20 * out.loopQuality) * materialPenalty,
        0.0,
        1.0);

    if (out.intrinsicBpm > 0.0)
    {
        const double barsRaw = (out.estimatedLoopLengthSeconds * out.detectedBpm / 60.0)
            / std::max(0.25, quartersPerNotatedBar);
        int bestBars = 0;
        double bestErr = 1.0e9;
        for (int b : {1, 2, 4, 8})
        {
            const double err = std::abs(barsRaw - static_cast<double>(b));
            if (err < bestErr)
            {
                bestErr = err;
                bestBars = b;
            }
        }
        if (bestBars > 0)
        {
            out.estimatedBars.push_back(bestBars);
        }
    }

    out.estimateUsable = out.confidence >= 0.65 && out.isLikelyLoop && peakStrength > 0.45;
    return out;
}
} // namespace retrolooper
