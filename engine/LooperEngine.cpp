// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#include "LooperEngine.h"
#include "AudioImportAnalysisService.h"
#include "DurationFamilyTempo.h"
#include "FlushDenormals.h"
#include "LoopTrackStateMachine.h"
#include "StereoPan.h"
#include "TempoEstimator.h"

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4127 4244 4305 4456)
#endif
#include "signalsmith-stretch/signalsmith-stretch.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <array>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <thread>
#include <utility>
#include <type_traits>
#include <vector>

namespace retrolooper
{
namespace
{
static_assert(metering::kMeterTracks == static_cast<std::size_t>(LooperEngine::kMaxTracks),
    "metering track count must match LooperEngine::kMaxTracks");
static_assert(WaveformEnvelopeBank::kTracks == LooperEngine::kMaxTracks,
    "waveform envelope track count must match LooperEngine::kMaxTracks");

float clamp01(float v) noexcept
{
    return std::clamp(v, 0.0f, 1.0f);
}

struct AudioCallbackScope
{
    std::atomic<int>& depth;
    explicit AudioCallbackScope(std::atomic<int>& d) noexcept
        : depth(d)
    {
        depth.fetch_add(1, std::memory_order_relaxed);
    }
    ~AudioCallbackScope()
    {
        depth.fetch_sub(1, std::memory_order_release);
    }
};

/** Mobius Out-Sync style: halve until <= max, then double until >= min. */
double foldMobiusTempo(double bpm, const double minBpm, const double maxBpm) noexcept
{
    if (!(bpm > 0.0) || !std::isfinite(bpm))
    {
        return 0.0;
    }
    int guard = 64;
    while (bpm > maxBpm && guard-- > 0)
    {
        bpm *= 0.5;
    }
    guard = 64;
    while (bpm < minBpm && guard-- > 0)
    {
        bpm *= 2.0;
    }
    return bpm;
}

/** Loop read index for a global transport sample and per-track phase anchor (shared musical clock). */
std::size_t loopPositionFromTransport(
    const std::int64_t transportSample, const std::int64_t origin, const std::size_t len) noexcept
{
    if (len == 0)
    {
        return 0;
    }
    const std::int64_t li = static_cast<std::int64_t>(len);
    std::int64_t r = (transportSample - origin) % li;
    if (r < 0)
    {
        r += li;
    }
    return static_cast<std::size_t>(r);
}

/** Snap {@code pos} forward to the next boundary of {@code period}, or keep {@code pos} if already on a boundary. */
std::int64_t alignToPeriodBoundary(std::int64_t pos, std::int64_t period) noexcept
{
    if (period <= 0)
    {
        return pos;
    }
    const auto r = pos % period;
    if (r == 0)
    {
        return pos;
    }
    return pos + (period - r);
}

/** Snap forward to the next period boundary anchored at {@code origin}. */
std::int64_t alignToPeriodBoundaryWithOrigin(const std::int64_t pos, const std::int64_t period, const std::int64_t origin) noexcept
{
    if (period <= 0)
    {
        return pos;
    }
    std::int64_t rel = (pos - origin) % period;
    if (rel < 0)
    {
        rel += period;
    }
    if (rel == 0)
    {
        return pos;
    }
    return pos + (period - rel);
}

float sampleHardwareInput(const float* const* hw, const int hwCh, const int channelIndex, const int sampleIndex) noexcept
{
    if (hw == nullptr || hwCh <= 0 || channelIndex < 0)
    {
        return 0.0f;
    }
    const int ci = std::min(channelIndex, hwCh - 1);
    const float* p = hw[ci];
    return p != nullptr ? p[sampleIndex] : 0.0f;
}

float readRecordChannel(const TrackIoRouting& io,
    const int trackChannel,
    const int sample,
    const float* const* hw,
    const int hwCh,
    const float* instL,
    const float* instR,
    const int instCh,
    const int trackIndex,
    const float* const* trackRecord,
    const int numTrackRecordChannels) noexcept
{
    if (trackRecord != nullptr && trackIndex >= 0 && numTrackRecordChannels >= (trackIndex + 1) * 2)
    {
        const float* left = trackRecord[trackIndex * 2];
        const float* right = trackRecord[trackIndex * 2 + 1];
        if (left != nullptr)
        {
            const float L = left[sample];
            const float R = (right != nullptr) ? right[sample] : L;
            if (io.recordMode == TrackIoChannelMode::Mono)
            {
                return 0.5f * (L + R);
            }
            return trackChannel == 0 ? L : R;
        }
    }
    if (io.recordInput != TrackRecordInput::Hardware && io.recordInput != TrackRecordInput::InstrumentPlugin)
    {
        return 0.0f;
    }
    if (io.recordInput == TrackRecordInput::InstrumentPlugin)
    {
        if (instCh <= 0 || instL == nullptr)
        {
            return 0.0f;
        }
        const float L = instL[sample];
        const float R = (instCh > 1 && instR != nullptr) ? instR[sample] : L;
        if (io.recordMode == TrackIoChannelMode::Mono)
        {
            return 0.5f * (L + R);
        }
        return trackChannel == 0 ? L : R;
    }

    if (hw == nullptr || hwCh <= 0)
    {
        return 0.0f;
    }
    const int start = static_cast<int>(io.hardwareInputStartChannel);
    if (io.recordMode == TrackIoChannelMode::Mono)
    {
        const int ch = std::clamp(start, 0, hwCh - 1);
        return sampleHardwareInput(hw, hwCh, ch, sample);
    }
    const int c0 = std::clamp(start, 0, hwCh - 1);
    const int c1 = std::clamp(start + 1, 0, hwCh - 1);
    return trackChannel == 0 ? sampleHardwareInput(hw, hwCh, c0, sample) : sampleHardwareInput(hw, hwCh, c1, sample);
}

float readLoopSampleSeamed(const Track& track, const int channel, const std::size_t pos, const std::size_t len,
    const int xfSamples) noexcept
{
    const float s = track.readSample(channel, pos);
    if (xfSamples <= 0 || len < 4u)
    {
        return s;
    }
    const auto xf = std::max<std::size_t>(2u, std::min(static_cast<std::size_t>(xfSamples), len / 2u));
    if (pos < xf || pos >= len - xf)
    {
        // Meet at one shared endpoint instead of crossfading unrelated sample
        // regions. Smoothstep returns to the untouched signal without a gain
        // bump, extra samples, or a changed loop period. Constant loops remain
        // constant; short loops receive a proportionally bounded correction.
        const float first = track.readSample(channel, 0);
        const float last = track.readSample(channel, len - 1u);
        const float join = 0.5f * (first + last);
        const bool head = pos < xf;
        const auto distance = head ? pos : len - 1u - pos;
        const float t = static_cast<float>(distance) / static_cast<float>(xf - 1u);
        const float weight = 1.0f - t * t * (3.0f - 2.0f * t);
        return s + (join - (head ? first : last)) * weight;
    }
    return s;
}

float hermiteInterp(const float y0, const float y1, const float y2, const float y3, const float t) noexcept
{
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + y1;
}

void sampleLoopStereo(const Track& track,
    const double positionUnwrapped,
    const std::size_t len,
    const int xfSamples,
    float& outL,
    float& outR) noexcept
{
    if (len == 0 || !std::isfinite(positionUnwrapped))
    {
        outL = outR = 0.0f;
        return;
    }
    double p = positionUnwrapped;
    if (!(p >= 0.0 && p < static_cast<double>(len)))
    {
        p = std::fmod(p, static_cast<double>(len));
        if (p < 0.0)
        {
            p += static_cast<double>(len);
        }
    }
    const double i1d = std::floor(p);
    const auto i1 = static_cast<std::size_t>(i1d);
    const float t = static_cast<float>(p - i1d);
    const auto i0 = (i1 == 0) ? (len - 1) : (i1 - 1);
    const auto i2 = (i1 + 1 < len) ? (i1 + 1) : 0;
    const auto i3 = (i2 + 1 < len) ? (i2 + 1) : 0;
    const int chR = track.channels() > 1 ? 1 : 0;
    const auto xf = xfSamples <= 0
        ? std::size_t{0}
        : std::max<std::size_t>(2u, std::min(static_cast<std::size_t>(xfSamples), len / 2u));
    const bool interior = xf == 0 || len < 4u || (i1 >= xf + 1 && i1 + 2 < len - xf);
    const auto tap = [&](const int channel, const std::size_t pos) noexcept
    {
        return interior ? track.readSample(channel, pos) : readLoopSampleSeamed(track, channel, pos, len, xfSamples);
    };
    outL = hermiteInterp(tap(0, i0), tap(0, i1), tap(0, i2), tap(0, i3), t);
    if (chR == 0)
    {
        outR = outL;
        return;
    }
    outR = hermiteInterp(tap(chR, i0), tap(chR, i1), tap(chR, i2), tap(chR, i3), t);
}

double playbackRateFromBpm(const double masterBpm, const double refBpm) noexcept
{
    if (!(refBpm > 1.0e-9) || !(masterBpm > 1.0e-9))
    {
        return 1.0;
    }
    const double ratio = masterBpm / refBpm;
    if (!std::isfinite(ratio) || ratio <= 0.0)
    {
        return 1.0;
    }
    return std::clamp(ratio, 0.25, 4.0);
}

bool ratioNeedsVarispeed(const double ratio) noexcept
{
    return std::abs(ratio - 1.0) > 1.0e-3;
}

void fillLoopPlanar(const Track& track,
    double startUnwrapped,
    const int count,
    const int xfSamples,
    std::vector<float>& left,
    std::vector<float>& right)
{
    if (count <= 0)
    {
        return;
    }
    const auto n = static_cast<std::size_t>(count);
    if (left.size() < n || right.size() < n)
    {
        return;
    }
    const std::size_t len = track.lengthSamples();
    double pos = startUnwrapped;
    for (int i = 0; i < count; ++i)
    {
        sampleLoopStereo(track, pos, len, xfSamples, left[static_cast<std::size_t>(i)], right[static_cast<std::size_t>(i)]);
        pos += 1.0;
    }
}

void routePlaybackSample(const TrackIoRouting& io,
    float* const* const output,
    const int numOutputChannels,
    const int sample,
    const float l,
    const float r) noexcept
{
    if (output == nullptr || numOutputChannels <= 0)
    {
        return;
    }
    if (io.hardwareOutputStartChannel == TrackIoRouting::kUseMainStereoBus)
    {
        if (numOutputChannels >= 2 && output[0] != nullptr && output[1] != nullptr)
        {
            output[0][sample] += l;
            output[1][sample] += r;
        }
        else if (numOutputChannels >= 1 && output[0] != nullptr)
        {
            output[0][sample] += 0.5f * (l + r);
        }
        return;
    }
    if (io.hardwareOutputStartChannel == TrackIoRouting::kSilentOutput)
    {
        return;
    }

    const int b = static_cast<int>(io.hardwareOutputStartChannel);
    if (io.playbackMode == TrackIoChannelMode::Stereo)
    {
        if (b < numOutputChannels && output[b] != nullptr)
        {
            output[b][sample] += l;
        }
        if (b + 1 < numOutputChannels && output[b + 1] != nullptr)
        {
            output[b + 1][sample] += r;
        }
        else if (b < numOutputChannels && output[b] != nullptr)
        {
            output[b][sample] += 0.5f * (l + r);
        }
    }
    else
    {
        if (b < numOutputChannels && output[b] != nullptr)
        {
            output[b][sample] += 0.5f * (l + r);
        }
    }
}
} // namespace

struct TrackTimeStretcher
{
    signalsmith::stretch::SignalsmithStretch<float> stretch;
    double configuredRate = 0.0;

    bool ensureConfigured(const double sampleRate)
    {
        if (configuredRate > 0.0 && std::abs(configuredRate - sampleRate) < 0.5)
        {
            return false;
        }
        configuredRate = sampleRate;
        const float sr = static_cast<float>(std::clamp(sampleRate, 8000.0, 384000.0));
        stretch.presetDefault(2, sr, true);
        stretch.setTransposeFactor(1.0f);
        return true;
    }
};

LooperEngine::LooperEngine()
{
    metroMixL_.assign(static_cast<std::size_t>(kMetroScratchCap), 0.0f);
    metroMixR_.assign(static_cast<std::size_t>(kMetroScratchCap), 0.0f);
    stereoBusScratchL_.assign(static_cast<std::size_t>(kMetroScratchCap), 0.0f);
    stereoBusScratchR_.assign(static_cast<std::size_t>(kMetroScratchCap), 0.0f);
    waveformEnvelopes_ = std::make_unique<WaveformEnvelopeBank>();
    trackPlayheads_.fill(0);
    trackWriteheads_.fill(0);
    sharedLoopPhaseOrigin_ = 0;
    trackPhaseFromStart_.fill(false);
    trackPhaseOrigin_.fill(0);
    trackRecordPhaseAnchor_.fill(false);
    trackRecordPhaseOrigin_.fill(0);
    trackRecordInputGateTransport_.fill(-1);
    publishMusicalClockUi();
    stretchSourceUnwrapped_.fill(0.0);
    stretchFrac_.fill(0.0);
    stretchWarm_.fill(false);
    stretchPhaseOrigin_.fill(0);
    for (int t = 0; t < kMaxTracks; ++t)
    {
        const auto i = static_cast<std::size_t>(t);
        trackOutPeakL_[i].store(0.0f, std::memory_order_relaxed);
        trackOutPeakR_[i].store(0.0f, std::memory_order_relaxed);
        trackInPeakL_[i].store(0.0f, std::memory_order_relaxed);
        trackInPeakR_[i].store(0.0f, std::memory_order_relaxed);
        trackOutRmsL_[i].store(0.0f, std::memory_order_relaxed);
        trackOutRmsR_[i].store(0.0f, std::memory_order_relaxed);
        trackInRmsL_[i].store(0.0f, std::memory_order_relaxed);
        trackInRmsR_[i].store(0.0f, std::memory_order_relaxed);
        crossfadeAssign_[i].store(0, std::memory_order_relaxed);
    }
    for (auto& scheduled : scheduledCommands_)
    {
        scheduled.active = false;
    }
    serialTailLen_.fill(0);
    serialTailHandoffSample_.fill(0);
    serialFadeInEndSample_.fill(0);
    muteGain_.fill(1.0f);
    muteGainTarget_.fill(1.0f);
    stopFadeReadPos_.fill(0);
    pendingRecordEndExclusive_.fill(-1);
    finishRecordingToPlaying_.fill(false);
    lastTransitionError_.fill(TransitionError::None);
}

LooperEngine::~LooperEngine() = default;

void LooperEngine::prepare(double sampleRate, bool resetTransport)
{
    const double previousRate = transport_.sampleRate();
    const bool sampleRateChanged = !(previousRate > 1.0) || std::abs(previousRate - sampleRate) > 0.5;
    // Device preparation runs with callbacks stopped. Keep loop audio, history,
    // transport and scheduled sample positions in the same time domain.
    if (sampleRateChanged && previousRate > 0.0)
    {
        const double ratio=sampleRate/previousRate;
        const auto scale=[ratio](auto& value) { value=static_cast<std::decay_t<decltype(value)>>(std::llround(static_cast<double>(value)*ratio)); };
        for (auto& track : tracks_) track.resampleAudio(previousRate,sampleRate);
        if (!resetTransport)
        {
            auto pos=transport_.samplePosition(); scale(pos); transport_.setSamplePosition(pos);
            scale(sharedLoopPhaseOrigin_);
            for(auto& v:trackPhaseOrigin_)scale(v);
            for(auto& v:trackRecordPhaseOrigin_)scale(v);
            for(auto& v:trackPlayheads_)scale(v);
            for(auto& v:trackWriteheads_)scale(v);
            for(auto& v:stopFadeReadPos_)scale(v);
            for(auto& v:trackRecordInputGateTransport_)if(v>=0)scale(v);
            for(auto& v:pendingRecordEndExclusive_)if(v>=0)scale(v);
            for(auto& v:serialTailHandoffSample_)scale(v);
            for(auto& v:serialFadeInEndSample_)scale(v);
            for(auto& slot:scheduledCommands_) if(slot.active) { scale(slot.executeAtSample); if(slot.command.executeAtSample>=0)scale(slot.command.executeAtSample); }
            if(clusteredPlayExecuteAt_>=0)scale(clusteredPlayExecuteAt_);
            if(idleLaunchGroupEndsAt_>=0)scale(idleLaunchGroupEndsAt_);
        }
    }
    transport_.setSampleRate(sampleRate);
    serialCrossfadeSamples_ = static_cast<int>(std::lround(sampleRate * 0.008));
    serialCrossfadeSamples_ = std::clamp(serialCrossfadeSamples_, 64, 2048);
    muteRampSamples_ = std::max(1, static_cast<int>(std::lround(sampleRate * static_cast<double>(kMuteRampMilliseconds) * 0.001)));
    refreshLoopCrossfadeSamples();
    if (resetTransport)
    {
        transport_.reset();
        sharedLoopPhaseOrigin_ = 0;
        publishMusicalClockUi();
        for (int i = 0; i < kMaxTracks; ++i)
        {
            serialTailLen_[static_cast<std::size_t>(i)] = 0;
            serialTailHandoffSample_[static_cast<std::size_t>(i)] = 0;
            serialFadeInEndSample_[static_cast<std::size_t>(i)] = 0;
            muteGain_[static_cast<std::size_t>(i)] = 1.0f;
            muteGainTarget_[static_cast<std::size_t>(i)] = 1.0f;
            stopFadeReadPos_[static_cast<std::size_t>(i)] = 0;
        }
        pendingRecordEndExclusive_.fill(-1);
        finishRecordingToPlaying_.fill(false);
        metroClickResidualLen_ = 0;
        metroClickResidualOff_ = 0;
    }
    if (resetTransport || sampleRateChanged)
    {
        for (int i = 0; i < kMaxTracks; ++i)
        {
            const auto ti = static_cast<std::size_t>(i);
            if (stretchEngines_[ti] != nullptr)
            {
                stretchEngines_[ti]->stretch.reset();
            }
            stretchWarm_[ti] = false;
            stretchFrac_[ti] = 0.0;
            stretchSourceUnwrapped_[ti] = 0.0;
            stretchPhaseOrigin_[ti] = 0;
        }
    }
    if (resetTransport) trackRecordInputGateTransport_.fill(-1);
    liveTransientDetector_.prepare(sampleRate);
    refreshIsRecordingFlag();
    masterGainSmooth_.reset(sampleRate, 0.020);
    masterGainSmooth_.setCurrentAndTargetValue(masterGain_.load(std::memory_order_relaxed));
    for (int i = 0; i < kMaxTracks; ++i)
    {
        tracks_[static_cast<std::size_t>(i)].prepareMixSmoothing(sampleRate, 0.020);
        // Build the stretcher here. The first tempo-changed callback otherwise
        // allocates it on the audio thread and drops the buffer.
        ensureStretchEngine(i);
    }
    ensureAudioScratchCapacity(kMetroScratchCap);
    prepareLiveRecordHeadroom();
    publishMusicalClockUi();
}

void LooperEngine::ensureAudioScratchCapacity(const int maxSamples) noexcept
{
    const int n = std::max(maxSamples, kMetroScratchCap);
    const auto grow = [](std::vector<float>& v, const int count)
    {
        const auto sz = static_cast<std::size_t>(std::max(0, count));
        if (v.size() < sz)
        {
            v.resize(sz, 0.0f);
        }
    };
    grow(mixVolScratch_, n);
    grow(mixPanScratch_, n);
    grow(mixInScratch_, n);
    grow(mixFxScratch_, n);
    grow(mixDryLScratch_, n);
    grow(mixDryRScratch_, n);
    grow(stretchOutL_, n);
    grow(stretchOutR_, n);
    grow(stereoBusScratchL_, n);
    grow(stereoBusScratchR_, n);
    grow(metroMixL_, n);
    grow(metroMixR_, n);
    // outputSeekLength at high rates is about a fifth of a second of source audio.
    grow(stretchInL_, std::max(n * 8, 262144));
    grow(stretchInR_, std::max(n * 8, 262144));
    const int tailN = std::max(serialCrossfadeSamples_, 64);
    for (int i = 0; i < kMaxTracks; ++i)
    {
        grow(serialTailL_[static_cast<std::size_t>(i)], tailN);
        grow(serialTailR_[static_cast<std::size_t>(i)], tailN);
    }
}

void LooperEngine::resetStretchForTrack(const int trackIndex) noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return;
    }
    const auto ti = static_cast<std::size_t>(trackIndex);
    stretchSourceUnwrapped_[ti] = 0.0;
    stretchFrac_[ti] = 0.0;
    stretchWarm_[ti] = false;
    stretchPhaseOrigin_[ti] = 0;
    if (stretchEngines_[ti] != nullptr)
    {
        stretchEngines_[ti]->stretch.reset();
    }
}

void LooperEngine::resetLoopPlaybackToBeginning(const int trackIndex, const std::int64_t phaseOriginSample) noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return;
    }
    const auto ti = static_cast<std::size_t>(trackIndex);
    resetStretchForTrack(trackIndex);
    const std::size_t len = tracks_[ti].lengthSamples();
    if (len == 0)
    {
        trackPlayheads_[ti] = 0;
        return;
    }
    const std::int64_t pos = phaseOriginSample >= 0 ? phaseOriginSample : transport_.samplePosition();
    trackPlayheads_[ti] = loopPositionFromTransport(pos, sharedLoopPhaseOrigin_, len);
}

void LooperEngine::ensureStretchEngine(const int trackIndex)
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return;
    }
    const auto ti = static_cast<std::size_t>(trackIndex);
    if (stretchEngines_[ti] == nullptr)
    {
        stretchEngines_[ti] = std::make_unique<TrackTimeStretcher>();
    }
    if (stretchEngines_[ti]->ensureConfigured(transport_.sampleRate()))
    {
        stretchWarm_[ti] = false;
        stretchFrac_[ti] = 0.0;
    }
}

void LooperEngine::renderTimeStretchBlock(const int trackIndex,
    const Track& track,
    double tempoRatio,
    const int numSamples,
    float* outL,
    float* outR,
    const double musicalSourceStart,
    double& phaseUnwrappedInOut)
{
    const auto ti = static_cast<std::size_t>(trackIndex);
    const auto len = track.lengthSamples();
    if (len == 0 || numSamples <= 0 || outL == nullptr || outR == nullptr)
    {
        return;
    }

    if (!std::isfinite(tempoRatio) || tempoRatio <= 0.0)
    {
        tempoRatio = 1.0;
    }
    tempoRatio = std::clamp(tempoRatio, 0.25, 4.0);

    ensureStretchEngine(trackIndex);
    auto& st = stretchEngines_[ti]->stretch;

    const std::int64_t phaseOrigin = playbackPhaseOrigin(trackIndex);
    const bool originChanged = stretchWarm_[ti] && stretchPhaseOrigin_[ti] != phaseOrigin;
    // Tempo-preserving origin rebases update stretchPhaseOrigin_ first so this stays false.
    const bool needSeek = !stretchWarm_[ti] || originChanged;
    if (needSeek)
    {
        const float rate = static_cast<float>(tempoRatio);
        int seekN = static_cast<int>(std::lround(st.outputSeekLength(rate)));
        // A short seek makes Signalsmith infer a zero rate and the following audio plays at the wrong pitch.
        const int bufCap = static_cast<int>(std::min(stretchInL_.size(), stretchInR_.size()));
        seekN = std::clamp(seekN, 1, std::max(1, bufCap));
        fillLoopPlanar(track, musicalSourceStart, seekN, loopCrossfadeSamples_, stretchInL_, stretchInR_);
        float* seekPtrs[2] = {stretchInL_.data(), stretchInR_.data()};
        st.outputSeek(seekPtrs, seekN);
        stretchSourceUnwrapped_[ti] = musicalSourceStart + static_cast<double>(seekN);
        stretchFrac_[ti] = 0.0;
        stretchWarm_[ti] = true;
        stretchPhaseOrigin_[ti] = phaseOrigin;
    }

    double want = static_cast<double>(numSamples) * tempoRatio + stretchFrac_[ti];
    int inN = static_cast<int>(std::lround(want));
    inN = std::clamp(inN, 0, std::max(1, numSamples) * 8);
    stretchFrac_[ti] = want - static_cast<double>(inN);

    float dummy = 0.0f;
    float* inPtrs[2] = {&dummy, &dummy};
    if (inN > 0)
    {
        fillLoopPlanar(track, stretchSourceUnwrapped_[ti], inN, loopCrossfadeSamples_, stretchInL_, stretchInR_);
        inPtrs[0] = stretchInL_.data();
        inPtrs[1] = stretchInR_.data();
    }
    float* outPtrs[2] = {outL, outR};
    st.process(inPtrs, inN, outPtrs, numSamples);

    stretchSourceUnwrapped_[ti] += static_cast<double>(inN);
    phaseUnwrappedInOut = musicalSourceStart + static_cast<double>(numSamples) * tempoRatio;
}

void LooperEngine::setTrackReferenceBpm(const int trackIndex, const double bpm) noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return;
    }
    tracks_[static_cast<std::size_t>(trackIndex)].setReferenceBpm(bpm);
}

#include "_segment_gen.cpp"

void LooperEngine::processBlock(const float* const* hardwareInput,
    const int numHardwareInputChannels,
    const float* instrumentInputLeft,
    const float* instrumentInputRight,
    const int numInstrumentChannels,
    float* const* output,
    const int numSamples,
    const int numOutputChannels,
    const float* const* trackRecordInput,
    const int numTrackRecordChannels)
{
    const AudioCallbackScope inCallback(audioCallbackDepth_);
    for (auto& track : tracks_) track.serviceSaveSnapshot();
    const ScopedNoDenormals flushDenormals;
    applyPendingTempoPhasePreserve();
    {
        const float tgt = crossfaderTarget_.load(std::memory_order_relaxed);
        crossfaderSmooth_ += 0.12f * (tgt - crossfaderSmooth_);
    }
    auto refreshBlockMasterBpm = [this]()
    {
        blockMasterBpm_ = transport_.tempoBpm();
        if (!(blockMasterBpm_ > 1.0) || !std::isfinite(blockMasterBpm_))
        {
            blockMasterBpm_ = 120.0;
        }
    };
    refreshBlockMasterBpm();
    const auto blockStartSample = transport_.samplePosition();
    const auto blockEndSample = blockStartSample + numSamples;

    // A mode switch cancels a mode-dependent cue; never reinterpret it as
    // a different transport action at the old boundary. Only touch queues
    // and pending track states here, on the audio thread.
    const int playModeContext = dubModeEnabled_ ? 2 : muteModeEnabled_ ? 1 : 0;
    for (auto& slot : scheduledCommands_)
    {
        if (!slot.active || slot.playModeContext < 0 || slot.playModeContext == playModeContext)
            continue;
        slot.active = false;
        const int t = slot.command.trackIndex;
        if (t < 0 || t >= kMaxTracks)
            continue;
        auto& track = tracks_[static_cast<std::size_t>(t)];
        const auto state = track.state();
        if (state == TrackState::Playing_DubPending)
            track.setState(TrackState::Playing);
        else if (state == TrackState::Armed)
            track.setState(track.lengthSamples() > 0 ? TrackState::Stopped : TrackState::Empty);
        else if (trackStateIsMutePending(state))
            track.setState(trackStateCancelMutePending(state));
    }
    if (releaseMutedOnMuteModeExit_.exchange(false, std::memory_order_acq_rel))
    {
        releaseMutedTracksOnMuteModeExit();
    }

    const bool drainStartedIdle = !anyTrackPlayingOrPendingLaunch();
    if (drainStartedIdle || syncMode_ != SyncMode::Synced)
        idleLaunchGroupEndsAt_ = -1;
    // Decide once for this command drain. A producer may append another track
    // trigger while we consume the first; it must not suddenly quantise merely
    // because applying the first command changed the engine's running state.
    clusteredPlayExecuteAt_ = drainStartedIdle ? blockStartSample : -1;

    EngineCommand command;
    while (popCommand(command))
    {
        // Reject unavailable Record actions before scheduling or arming them.
        // Otherwise Separate Commands can display a permanent false dub cue
        // and even interrupt another track's capture for an action it rejects.
        if (command.type == CommandType::RecordToggle && command.trackIndex >= 0
            && command.trackIndex < kMaxTracks)
        {
            const auto& track = tracks_[static_cast<std::size_t>(command.trackIndex)];
            if (resolveLoopTrackTransition(runtimeStateForTrack(track), LoopTrackCommand::Record,
                    track.lengthSamples() > 0, trackBehaviorMode_, afterRecordMode_, dubModeEnabled_).noOp)
                continue;
        }
        const bool hadQueuedSameType = command.trackIndex >= 0 && command.trackIndex < kMaxTracks
            && [&]()
            {
                for (const auto& slot : scheduledCommands_)
                {
                    if (slot.active && slot.command.trackIndex == command.trackIndex
                        && slot.command.type == command.type)
                    {
                        return true;
                    }
                }
                return false;
            }();
        const bool hadQueuedStopAll = [&]()
        {
            for (const auto& slot : scheduledCommands_)
            {
                if (slot.active && slot.command.type == CommandType::StopAll)
                {
                    return true;
                }
            }
            return false;
        }();
        if (perTrackIsolationLock_ && command.trackIndex >= 0 && command.trackIndex < kMaxTracks)
        {
            // Per-track command replaces any stale queued action for that track.
            cancelAllScheduledCommandsForTrack(command.trackIndex);
        }
        const bool incomingStopAll = command.type == CommandType::StopAll
            || (command.type == CommandType::PlayAll && !command.immediate && playStopAllShouldStop());
        if (incomingStopAll && hadQueuedStopAll)
        {
            for (int t = 0; t < kMaxTracks; ++t)
            {
                cancelAllScheduledCommandsForTrack(t);
            }
            EngineCommand stopAll{};
            stopAll.type = CommandType::StopAll;
            stopAll.trackIndex = -1;
            applyCommand(stopAll, blockStartSample, blockStartSample);
            continue;
        }
        // Second Rec/Stop while waiting for Beat/Bar/Loop executes the cued action now.
        // Second Play/Mute still cancels the wait.
        if (hadQueuedSameType
            && (command.type == CommandType::RecordToggle || command.type == CommandType::MuteToggle
                || command.type == CommandType::PlayStopToggle || command.type == CommandType::Play
                || command.type == CommandType::Stop || command.type == CommandType::OverdubToggle
                || command.type == CommandType::UndoOverdub || command.type == CommandType::RedoOverdub))
        {
            const auto st = tracks_[static_cast<std::size_t>(command.trackIndex)].state();
            const bool cuedStopNow = command.type == CommandType::Stop
                || (command.type == CommandType::PlayStopToggle
                    && !playStopShouldOverdub(command.trackIndex) && !playStopShouldMute(command.trackIndex)
                    && (st == TrackState::Recording || trackStateAdvancesPlayhead(st)));
            const bool cuedRecordNow = command.type == CommandType::RecordToggle;
            if (cuedStopNow || cuedRecordNow)
            {
                applyCommand(command, blockStartSample, blockStartSample);
                continue;
            }
            if (st == TrackState::Armed || st == TrackState::Playing_DubPending || trackStateIsMutePending(st)
                || command.type == CommandType::UndoOverdub || command.type == CommandType::RedoOverdub)
            {
                applyCommand(command, -1, blockStartSample);
            }
            continue;
        }
        if (command.type == CommandType::Stop && command.trackIndex >= 0 && command.trackIndex < kMaxTracks
            && hadQueuedStopAll)
        {
            applyCommand(command, blockStartSample, blockStartSample);
            continue;
        }
        if (tryScheduleSerialHandoff(command, blockStartSample))
        {
            continue;
        }
        if (command.type == CommandType::PlayAll && !command.immediate && playStopAllShouldStop())
        {
            command.type = CommandType::StopAll;
        }
        if (command.type == CommandType::PlayAll || command.type == CommandType::StopAll)
        {
            for (int t = 0; t < kMaxTracks; ++t)
            {
                cancelAllScheduledCommandsForTrack(t);
            }
        }
        // Keyboard chords arrive as individual OS events, often in different
        // audio callbacks. Keep a fixed 100ms window from the first idle launch;
        // never delay that launch or extend the window with subsequent keys.
        if (drainStartedIdle && !clockGridIsActive() && syncMode_ == SyncMode::Synced && idleLaunchGroupEndsAt_ < 0
            && commandStartsPlayback(command) && command.executeAtSample < 0)
            idleLaunchGroupEndsAt_ = blockStartSample
                + static_cast<std::int64_t>(transport_.sampleRate() * 0.100);
        const auto executeAt = computeExecuteSampleForCommand(command);
        if (executeAt > blockStartSample && exclusiveLaunchPullsSerialParallelBed(command))
        {
            maybeStartSerialParallelBed(transport_.samplePosition());
        }
        if (command.type == CommandType::MuteToggle && command.trackIndex >= 0 && command.trackIndex < kMaxTracks
            && trackStateIsMutePending(tracks_[static_cast<std::size_t>(command.trackIndex)].state()))
        {
            applyCommand(command, -1, blockStartSample);
            continue;
        }
        if (command.type == CommandType::RecordToggle && command.trackIndex >= 0 && command.trackIndex < kMaxTracks
            && tracks_[static_cast<std::size_t>(command.trackIndex)].state() == TrackState::Playing_DubPending)
        {
            applyCommand(command, -1, blockStartSample);
            continue;
        }
        if (executeAt <= blockStartSample)
        {
            applyCommand(command, -1, blockStartSample);
        }
        else
        {
            const bool pushed = pushScheduledCommand(command, executeAt);
            if (pushed)
            {
                armPendingStateForScheduledCommand(command);
            }
        }
    }

    refreshIsRecordingFlag();
    applyPendingTempoPhasePreserve();
    refreshBlockMasterBpm();
    const bool recordingNow = isRecording_.load(std::memory_order_acquire);
    // Empty session first take: stream onsets into the real-time detector.
    // SyncSet: skip detection and capture as a normal record/overdub.
    liveTransientCapture_ = recordingNow && (tempoManager_.state() == TempoSyncState::Empty);

    for (int channel = 0; channel < numOutputChannels; ++channel)
    {
        if (output == nullptr || output[channel] == nullptr)
        {
            continue;
        }
        std::fill_n(output[channel], numSamples, 0.0f);
    }

    float inputPeak = 0.0f;
    metering::TrackMeterBlockAccum meterAccum{};

    std::array<std::pair<std::int64_t, EngineCommand>, kCommandQueueSize> schedEvents{};
    int schedCount = 0;
    for (auto& slot : scheduledCommands_)
    {
        if (!slot.active)
        {
            continue;
        }
        if (slot.executeAtSample < blockStartSample)
        {
            schedEvents[static_cast<std::size_t>(schedCount)] = {blockStartSample, slot.command};
            ++schedCount;
            slot.active = false;
        }
        else if (slot.executeAtSample >= blockStartSample && slot.executeAtSample < blockEndSample)
        {
            schedEvents[static_cast<std::size_t>(schedCount)] = {slot.executeAtSample, slot.command};
            ++schedCount;
            slot.active = false;
        }
    }
    std::sort(schedEvents.begin(), schedEvents.begin() + static_cast<std::size_t>(schedCount),
        [](const auto& a, const auto& b) { return a.first < b.first; });

    std::int64_t segStart = blockStartSample;
    std::size_t si = 0;
    while (segStart < blockEndSample)
    {
        while (si < static_cast<std::size_t>(schedCount) && schedEvents[si].first == segStart)
        {
            applyCommand(schedEvents[si].second, schedEvents[si].first, segStart);
            ++si;
            applyPendingTempoPhasePreserve();
            refreshBlockMasterBpm();
        }
        std::int64_t segEnd = blockEndSample;
        if (si < static_cast<std::size_t>(schedCount))
        {
            segEnd = schedEvents[si].first;
        }
        const auto scratchCap = static_cast<std::int64_t>(
            std::min(mixVolScratch_.size(), stretchOutL_.size()));
        if (scratchCap > 0 && segEnd - segStart > scratchCap)
        {
            segEnd = segStart + scratchCap;
        }
        const int off = static_cast<int>(segStart - blockStartSample);
        const int len = static_cast<int>(segEnd - segStart);
        if (len > 0)
        {
            processTrackAudioSegment(blockStartSample,
                off,
                len,
                hardwareInput,
                numHardwareInputChannels,
                instrumentInputLeft,
                instrumentInputRight,
                numInstrumentChannels,
                output,
                numOutputChannels,
                inputPeak,
                meterAccum,
                trackRecordInput,
                numTrackRecordChannels);
        }
        segStart = segEnd;
    }

    if (output != nullptr && numOutputChannels > 0)
    {
        for (int trackIndex = 0; trackIndex < kMaxTracks; ++trackIndex)
        {
            const auto ti = static_cast<std::size_t>(trackIndex);
            const int K = serialTailLen_[ti];
            if (K <= 0)
            {
                continue;
            }
            const std::int64_t S = serialTailHandoffSample_[ti];
            const auto& io = trackIo_[ti];
            for (int sample = 0; sample < numSamples; ++sample)
            {
                const std::int64_t ts = blockStartSample + static_cast<std::int64_t>(sample);
                if (ts < S || ts >= S + static_cast<std::int64_t>(K))
                {
                    continue;
                }
                const int idx = static_cast<int>(ts - S);
                const float alpha = static_cast<float>(K - idx) / static_cast<float>(K);
                const float l = serialTailL_[ti][static_cast<std::size_t>(idx)] * alpha;
                const float r = serialTailR_[ti][static_cast<std::size_t>(idx)] * alpha;
                metering::accumulateOutputStereo(meterAccum, ti, l, r);
                routePlaybackSample(io, output, numOutputChannels, sample, l, r);
            }
            if (blockEndSample >= S + static_cast<std::int64_t>(K))
            {
                serialTailLen_[ti] = 0;
            }
        }
    }

    lastInputPeak_.store(inputPeak, std::memory_order_relaxed);

    const double sr = transport_.sampleRate();
    const float peakHoldDecay = (sr > 1.0 && numSamples > 0)
        ? static_cast<float>(std::exp(-static_cast<double>(numSamples) / (sr * 0.15)))
        : 0.5f;
    auto holdPeak = [peakHoldDecay](std::atomic<float>& slot, const float blockPeak) noexcept
    {
        const float prev = slot.load(std::memory_order_relaxed);
        slot.store((blockPeak >= prev) ? blockPeak : prev * peakHoldDecay, std::memory_order_relaxed);
    };

    for (int t = 0; t < kMaxTracks; ++t)
    {
        const auto ti = static_cast<std::size_t>(t);
        holdPeak(trackOutPeakL_[ti], meterAccum.outPeakL[ti]);
        holdPeak(trackOutPeakR_[ti], meterAccum.outPeakR[ti]);
        holdPeak(trackInPeakL_[ti], meterAccum.inPeakL[ti]);
        holdPeak(trackInPeakR_[ti], meterAccum.inPeakR[ti]);
        const int on = meterAccum.outSampleCount[ti];
        trackOutRmsL_[ti].store(metering::blockRms(meterAccum.outSumSqL[ti], on), std::memory_order_relaxed);
        trackOutRmsR_[ti].store(metering::blockRms(meterAccum.outSumSqR[ti], on), std::memory_order_relaxed);
        trackInRmsL_[ti].store(metering::blockRms(meterAccum.inSumSqL[ti], meterAccum.inSampleCountL[ti]),
            std::memory_order_relaxed);
        trackInRmsR_[ti].store(metering::blockRms(meterAccum.inSumSqR[ti], meterAccum.inSampleCountR[ti]),
            std::memory_order_relaxed);
    }

    publishWaveformEnvelopes();

    if (output != nullptr && numOutputChannels > 0)
    {
        if (metronomeMixToDevice_.load(std::memory_order_relaxed))
        {
            metroMixValidSamples_ = 0;
            renderMetronome(blockStartSample, numSamples, output, numOutputChannels);
        }
        else
        {
            metroMixValidSamples_ = 0;
            const int cap = std::min(static_cast<int>(metroMixL_.size()), static_cast<int>(metroMixR_.size()));
            if (numSamples > 0 && numSamples <= cap)
            {
                std::fill_n(metroMixL_.data(), numSamples, 0.0f);
                std::fill_n(metroMixR_.data(), numSamples, 0.0f);
                float* dest[2] = {metroMixL_.data(), metroMixR_.data()};
                renderMetronome(blockStartSample, numSamples, dest, 2);
                metroMixValidSamples_ = numSamples;
            }
        }

        if (!deferDeviceMasterGain_.load(std::memory_order_relaxed))
        {
            applyMasterGainToOutputs(output, numOutputChannels, numSamples);
        }
    }

    for (int t = 0; t < kMaxTracks; ++t)
    {
        const auto tt = static_cast<std::size_t>(t);
        const std::int64_t fe = serialFadeInEndSample_[tt];
        if (fe > 0 && blockEndSample >= fe)
        {
            serialFadeInEndSample_[tt] = 0;
        }
    }

    transport_.advance(numSamples);
    firstPassRecordOffsetInBlock_.store(-1, std::memory_order_release);
    publishMusicalClockUi();
    publishLoopViewAtomics();
}

void LooperEngine::processBlock(int numSamples)
{
    processBlock(nullptr, 0, nullptr, nullptr, 0, nullptr, numSamples, 0);
}

bool LooperEngine::enqueueCommand(const EngineCommand& command) noexcept
{
    return commandQueue_.push(command);
}

void LooperEngine::resetStartupCommandState() noexcept
{
    EngineCommand discarded;
    while (commandQueue_.pop(discarded)) {}
    for (auto& scheduled : scheduledCommands_)
    {
        scheduled.active = false;
    }
    clusteredPlayExecuteAt_ = -1;
    idleLaunchGroupEndsAt_ = -1;
}

void LooperEngine::queueTempoPhasePreserveFrom(const double previousBpm) noexcept
{
    const double applied = transport_.tempoBpm();
    if (!(previousBpm > 1.0) || !(applied > 1.0) || std::abs(applied - previousBpm) < 1.0e-9)
    {
        return;
    }
    if (!pendingTempoPhaseAdjust_.load(std::memory_order_acquire))
    {
        tempoPhaseAdjustFromBpm_.store(previousBpm, std::memory_order_relaxed);
    }
    pendingTempoPhaseAdjust_.store(true, std::memory_order_release);
}

void LooperEngine::applyPendingTempoPhasePreserve() noexcept
{
    if (!pendingTempoPhaseAdjust_.exchange(false, std::memory_order_acq_rel))
    {
        return;
    }
    const double fromBpm = tempoPhaseAdjustFromBpm_.load(std::memory_order_relaxed);
    const double toBpm = transport_.tempoBpm();
    const double sr = transport_.sampleRate();
    if (!(fromBpm > 1.0) || !(toBpm > 1.0) || sr <= 1.0 || std::abs(fromBpm - toBpm) < 1.0e-9)
    {
        return;
    }
    const double oldSpb = sr * 60.0 / fromBpm;
    const double newSpb = sr * 60.0 / toBpm;
    if (!(oldSpb > 1.0e-9) || !(newSpb > 1.0e-9))
    {
        return;
    }
    const std::int64_t pos = transport_.samplePosition();
    auto rebaseOrigin = [&](const std::int64_t origin) noexcept -> std::int64_t {
        const double beats = static_cast<double>(pos - origin) / oldSpb;
        return pos - static_cast<std::int64_t>(std::llround(beats * newSpb));
    };

    sharedLoopPhaseOrigin_ = rebaseOrigin(sharedLoopPhaseOrigin_);
    for (int t = 0; t < kMaxTracks; ++t)
    {
        const auto ti = static_cast<std::size_t>(t);
        if (trackPhaseFromStart_[ti])
        {
            trackPhaseOrigin_[ti] = rebaseOrigin(trackPhaseOrigin_[ti]);
        }
        if (trackRecordPhaseAnchor_[ti])
        {
            trackRecordPhaseOrigin_[ti] = rebaseOrigin(trackRecordPhaseOrigin_[ti]);
        }
        // Keep the stretcher streaming: origin moved to preserve the same source sample.
        stretchPhaseOrigin_[ti] = playbackPhaseOrigin(t);
    }
}

void LooperEngine::setTempo(double bpm, bool preservePhase) noexcept
{
    const double previous = transport_.tempoBpm();
    tempoManager_.setMasterBpm(static_cast<float>(bpm));
    transport_.setTempo(static_cast<double>(tempoManager_.masterBpm()));
    if (preservePhase)
    {
        queueTempoPhasePreserveFrom(previous);
    }
}

void LooperEngine::commitUserTempo(double bpm, bool preservePhase) noexcept
{
    setTempo(bpm, preservePhase);
    tempoManager_.restoreLockedTempo(static_cast<float>(transport_.tempoBpm()));
    userTempoCommitted_ = true;
}

void LooperEngine::setDetectedTempo(double bpm, bool preservePhase) noexcept
{
    const double previous = transport_.tempoBpm();
    tempoManager_.setMasterBpmExact(static_cast<float>(bpm));
    transport_.setTempo(static_cast<double>(tempoManager_.masterBpm()));
    if (preservePhase)
    {
        queueTempoPhasePreserveFrom(previous);
    }
}

void LooperEngine::restoreTempoState(const double bpm, const bool locked, const bool userCommitted) noexcept
{
    setDetectedTempo(bpm);
    if (locked)
    {
        tempoManager_.restoreLockedTempo(static_cast<float>(transport_.tempoBpm()));
    }
    else
    {
        tempoManager_.resetToEmpty();
    }
    userTempoCommitted_ = locked && userCommitted;
}

void LooperEngine::applyExternalBeatClock(const double tempoBpm, const double phaseBeats, const double quantumBeats,
    const bool snapPhaseOrigin) noexcept
{
    if (!(tempoBpm > 1.0) || !std::isfinite(tempoBpm))
    {
        return;
    }
    // External BPM estimates jitter. Retuning stretch / samples-per-beat every
    // block is what makes the click and loops skip or drag. Only follow a real change.
    if (std::abs(tempoBpm - transport_.tempoBpm()) >= 0.8)
    {
        setTempo(tempoBpm);
    }
    if (!snapPhaseOrigin)
    {
        return;
    }
    // Link owns this origin rewrite; do not also rebase from setTempo.
    pendingTempoPhaseAdjust_.store(false, std::memory_order_release);
    const double q = (quantumBeats > 0.5 && std::isfinite(quantumBeats)) ? quantumBeats : 4.0;
    double phase = phaseBeats;
    if (!std::isfinite(phase))
    {
        phase = 0.0;
    }
    phase = std::fmod(phase, q);
    if (phase < 0.0)
    {
        phase += q;
    }
    const double spb = transport_.samplesPerBeat();
    if (!(spb > 1.0e-9))
    {
        return;
    }
    const auto pos = transport_.samplePosition();
    sharedLoopPhaseOrigin_ = pos - static_cast<std::int64_t>(std::llround(phase * spb));
}

void LooperEngine::commitExternalTempo(double bpm) noexcept
{
    if(!std::isfinite(bpm) || bpm<40 || bpm>240)return;
    const double previous=transport_.tempoBpm();
    tempoManager_.restoreLockedTempo(static_cast<float>(bpm));transport_.setTempo(bpm);userTempoCommitted_=true;
    queueTempoPhasePreserveFrom(previous);applyPendingTempoPhasePreserve();
}

void LooperEngine::followExternalTransport(bool playing,double quarterNotePosition,bool transportEdge) noexcept
{
    if(!std::isfinite(quarterNotePosition))return;
    for(int t=0;t<kMaxTracks;++t) {
        cancelAllScheduledCommandsForTrack(t);
        auto& tr=tracks_[static_cast<std::size_t>(t)];
        if(tr.state()==TrackState::Armed)tr.setState(tr.lengthSamples()>0?TrackState::Stopped:TrackState::Empty);
        else if(trackStateIsPending(tr.state()))tr.setState(trackStateCancelMutePending(tr.state())==TrackState::Playing_DubPending?TrackState::Playing:trackStateCancelMutePending(tr.state()));
        if(!transportEdge && (tr.state()==TrackState::Recording || trackStateIsOverdub(tr.state()))) {
            applyCommand({CommandType::Stop,t,true},transport_.samplePosition(),transport_.samplePosition());
        }
    }
    if(transportEdge)applyCommand({playing?CommandType::PlayAll:CommandType::StopAll,-1,true},transport_.samplePosition(),transport_.samplePosition());
    if(!playing)return;
    for(int t=0;t<kMaxTracks;++t) {
        auto& tr=tracks_[static_cast<std::size_t>(t)];
        if(!trackStateAdvancesPlayhead(tr.state()) || tr.lengthSamples()==0)continue;
        const auto ti=static_cast<std::size_t>(t);
        const double reference=tr.referenceBpm()>1?tr.referenceBpm():transport_.tempoBpm();
        const double loopBeats=tr.lengthSamples()*reference/(transport_.sampleRate()*60.0);
        if(!(loopBeats>0))continue;
        double phase=std::fmod(quarterNotePosition,loopBeats);if(phase<0)phase+=loopBeats;
        const auto origin=transport_.samplePosition()-static_cast<std::int64_t>(std::llround(phase*transport_.samplesPerBeat()));
        clearRecordPhaseAnchor(t);trackPhaseFromStart_[ti]=true;trackPhaseOrigin_[ti]=origin;
        resetStretchForTrack(t);trackPlayheads_[ti]=static_cast<std::size_t>(phase/loopBeats*tr.lengthSamples())%tr.lengthSamples();
        stretchPhaseOrigin_[ti]=origin;
    }
}

TempoSyncState LooperEngine::tempoSyncState() const noexcept
{
    return tempoManager_.state();
}

bool LooperEngine::tempoIsLocked() const noexcept
{
    return tempoManager_.isLocked();
}

bool LooperEngine::isRecording() const noexcept
{
    return isRecording_.load(std::memory_order_acquire);
}

bool LooperEngine::loopIsClosed() const noexcept
{
    return loopIsClosed_.load(std::memory_order_relaxed);
}

void LooperEngine::publishMusicalClockUi() noexcept
{
    transportSamplePositionUi_.store(transport_.samplePosition(), std::memory_order_relaxed);
    sharedLoopPhaseOriginUi_.store(sharedLoopPhaseOrigin_, std::memory_order_relaxed);
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    transportUiSteadyNs_.store(ns, std::memory_order_relaxed);
}

std::int64_t LooperEngine::uiSharedLoopPhaseOrigin() const noexcept
{
    return sharedLoopPhaseOriginUi_.load(std::memory_order_relaxed);
}

std::int64_t LooperEngine::uiMusicalTransportSample() const noexcept
{
    const auto pos = transportSamplePositionUi_.load(std::memory_order_relaxed);
    if (!playStopAllShouldStop())
    {
        return pos;
    }
    const auto t0 = transportUiSteadyNs_.load(std::memory_order_relaxed);
    if (t0 <= 0)
    {
        return pos;
    }
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    const double dt = static_cast<double>(now - t0) * 1.0e-9;
    const double sr = transport_.sampleRate();
    if (!(sr > 1.0) || !(dt >= 0.0) || dt > 0.25)
    {
        return pos;
    }
    return pos + static_cast<std::int64_t>(dt * sr);
}

void LooperEngine::publishLoopViewAtomics() noexcept
{
    int ref = -1;
    std::size_t refLen = 0;
    for (int i = 0; i < kMaxTracks; ++i)
    {
        const auto& tr = tracks_[static_cast<std::size_t>(i)];
        const auto st = tr.state();
        if (tr.lengthSamples() > refLen && trackStateAdvancesPlayhead(st))
        {
            ref = i;
            refLen = tr.lengthSamples();
        }
    }
    if (ref < 0)
    {
        for (int i = 0; i < kMaxTracks; ++i)
        {
            const std::size_t len = tracks_[static_cast<std::size_t>(i)].lengthSamples();
            if (len > refLen)
            {
                ref = i;
                refLen = len;
            }
        }
    }

    const bool closed = ref >= 0 && refLen > 1u
        && tracks_[static_cast<std::size_t>(ref)].state() != TrackState::Recording;
    loopIsClosed_.store(closed, std::memory_order_relaxed);
    viewLoopSamples_.store(closed ? static_cast<std::uint64_t>(refLen) : 0u, std::memory_order_relaxed);
    std::uint64_t head = 0;
    if (closed)
    {
        const auto pos = trackLoopPositionSamples(ref);
        head = static_cast<std::uint64_t>(pos % refLen);
    }
    viewPlayheadSamples_.store(head, std::memory_order_relaxed);
}

void LooperEngine::refreshIsRecordingFlag() noexcept
{
    bool any = false;
    for (int i = 0; i < kMaxTracks; ++i)
    {
        if (tracks_[static_cast<std::size_t>(i)].state() == TrackState::Recording)
        {
            any = true;
            break;
        }
    }
    isRecording_.store(any, std::memory_order_release);
}

void LooperEngine::setCountInBars(int bars) noexcept
{
    countInBars_ = std::max(0, bars);
}

void LooperEngine::setSyncMode(SyncMode mode) noexcept
{
    syncMode_ = mode;
}

void LooperEngine::setTrackSyncUnit(TrackSyncUnit unit) noexcept
{
    trackSyncUnit_ = unit;
}

TrackSyncUnit LooperEngine::trackSyncUnit() const noexcept
{
    return trackSyncUnit_;
}

void LooperEngine::setSubcycles(int n) noexcept
{
    subcycles_ = std::clamp(n, 1, 32);
}

int LooperEngine::subcycles() const noexcept
{
    return subcycles_;
}

int LooperEngine::trackSyncMasterIndex() const noexcept
{
    if (tempoMasterTrack_ >= 0 && tempoMasterTrack_ < kMaxTracks
        && tracks_[static_cast<std::size_t>(tempoMasterTrack_)].lengthSamples() > 0)
    {
        return tempoMasterTrack_;
    }
    for (int i = 0; i < kMaxTracks; ++i)
    {
        if (tracks_[static_cast<std::size_t>(i)].lengthSamples() > 0)
        {
            return i;
        }
    }
    return -1;
}

void LooperEngine::reassignTrackSyncMasterAfterClear(const int clearedTrack) noexcept
{
    const bool masterGone = tempoMasterTrack_ == clearedTrack
        || tempoMasterTrack_ < 0 || tempoMasterTrack_ >= kMaxTracks
        || tracks_[static_cast<std::size_t>(tempoMasterTrack_)].lengthSamples() == 0;
    if (!masterGone)
    {
        return;
    }
    const int start = (clearedTrack >= 0 && clearedTrack < kMaxTracks) ? clearedTrack : 0;
    for (int k = 1; k < kMaxTracks; ++k)
    {
        const int i = (start + k) % kMaxTracks;
        if (tracks_[static_cast<std::size_t>(i)].lengthSamples() > 0)
        {
            tempoMasterTrack_ = i;
            return;
        }
    }
    tempoMasterTrack_ = -1;
    tempoMasterIsImport_ = false;
    userTempoCommitted_ = false;
    sharedLoopPhaseOrigin_ = 0;
    pendingDetectedTempoVotes_ = 0;
    pendingDetectedTempoBpm_ = 0.0;
    lastAcceptedDetectedTempoBpm_ = 0.0;
    tempoManager_.resetToEmpty();
    tempoManager_.setMasterBpm(TempoManager::kDefaultBpm);
    transport_.setTempo(static_cast<double>(TempoManager::kDefaultBpm));
}

bool LooperEngine::tempoMasterVacant() const noexcept
{
    if (tempoMasterTrack_ < 0 || tempoMasterTrack_ >= kMaxTracks)
    {
        return true;
    }
    return tracks_[static_cast<std::size_t>(tempoMasterTrack_)].lengthSamples() == 0;
}

void LooperEngine::claimTempoMasterIfVacant(const int trackIndex, const bool fromImport) noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks || !tempoMasterVacant())
    {
        return;
    }
    if (!trackIsSoleLoop(trackIndex))
    {
        return;
    }
    tempoMasterTrack_ = trackIndex;
    tempoMasterIsImport_ = fromImport;
}

void LooperEngine::lockSessionTempoFromLoop() noexcept
{
    if (tempoManager_.lockDetectedTempo(tempoManager_.masterBpm()))
    {
        userTempoCommitted_ = false;
    }
}

void LooperEngine::setQuantizeDivision(int beatsPerAction) noexcept
{
    quantizeDivision_ = beatsPerAction > 0 ? beatsPerAction : 1;
    quantizer_.setGridDivision(quantizeDivision_);
}

void LooperEngine::setTrackMix(int trackIndex, const TrackMix& mix) noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return;
    }

    tracks_[static_cast<std::size_t>(trackIndex)].setMix(mix);
    muteGainTarget_[static_cast<std::size_t>(trackIndex)] = mix.muted ? 0.0f : 1.0f;
}

void LooperEngine::setTrackPlaybackLevels(int trackIndex, float volume, float pan) noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks) return;
    auto& track = tracks_[static_cast<std::size_t>(trackIndex)];
    auto mix = track.mix(); mix.volume = volume; mix.pan = pan; track.setMix(mix);
}

void LooperEngine::restoreTrackCycleMetadata(int trackIndex,int cycles,int beats) noexcept
{
    if(trackIndex<0 || trackIndex>=kMaxTracks)return;
    auto& track=tracks_[static_cast<std::size_t>(trackIndex)];
    track.setCycleCount(std::max(1,cycles));track.setBeatsPerCycle(std::max(0,beats));
}

void LooperEngine::setTrackName(int trackIndex, std::string name)
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return;
    }

    tracks_[static_cast<std::size_t>(trackIndex)].setName(std::move(name));
}

std::string LooperEngine::trackName(int trackIndex) const
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return {};
    }

    return tracks_[static_cast<std::size_t>(trackIndex)].name();
}

void LooperEngine::setTrackLoopPrefs(int trackIndex, TrackLoopPrefs prefs) noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return;
    }

    withoutBeatTiming(prefs);
    prefs.afterRecordMode = prefs.afterRecordMode == static_cast<uint8_t>(AfterRecordMode::Dub)
        ? static_cast<uint8_t>(AfterRecordMode::Dub)
        : static_cast<uint8_t>(AfterRecordMode::Play);
    tracks_[static_cast<std::size_t>(trackIndex)].setLoopPrefs(prefs);
}

void LooperEngine::setTrackIoRouting(const int trackIndex, TrackIoRouting routing) noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return;
    }
    routing.hardwareInputStartChannel =
        static_cast<std::uint16_t>(std::clamp(static_cast<int>(routing.hardwareInputStartChannel), 0, 63));
    routing.recordSourceIndex =
        static_cast<std::uint16_t>(std::clamp(static_cast<int>(routing.recordSourceIndex), 0, 15));
    if (routing.hardwareOutputStartChannel != TrackIoRouting::kUseMainStereoBus
        && routing.hardwareOutputStartChannel != TrackIoRouting::kSilentOutput)
    {
        routing.hardwareOutputStartChannel =
            static_cast<std::uint16_t>(std::clamp(static_cast<int>(routing.hardwareOutputStartChannel), 0, 62));
    }
    if (trackStereoBusProcessor_)
    {
        // The mixer route carries playback; a direct device write would double it.
        routing.hardwareOutputStartChannel = TrackIoRouting::kSilentOutput;
    }
    trackIo_[static_cast<std::size_t>(trackIndex)] = routing;
}

TrackIoRouting LooperEngine::trackIoRouting(const int trackIndex) const noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return {};
    }
    return trackIo_[static_cast<std::size_t>(trackIndex)];
}

std::size_t LooperEngine::trackLoopPositionSamples(int trackIndex) const noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return 0;
    }
    const auto& track = tracks_[static_cast<std::size_t>(trackIndex)];
    const auto len = track.lengthSamples();
    if (len == 0)
    {
        return 0;
    }
    const auto st = track.state();
    if (!trackStateAdvancesPlayhead(st) && st != TrackState::Recording)
    {
        return 0;
    }
    return loopPositionFromTransport(transport_.samplePosition(), playbackPhaseOrigin(trackIndex), len);
}

bool LooperEngine::restoreTrack(int trackIndex, TrackState state, std::size_t lengthSamples) noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return false;
    }

    auto& track = tracks_[static_cast<std::size_t>(trackIndex)];
    track.clearAudio();
    track.ensureCapacity(lengthSamples);
    track.reserveOverdubHistory(lengthSamples);
    track.setLengthSamples(lengthSamples);
    track.setRecordedSamples(lengthSamples);
    track.setCycleCount(1);
    track.setState(state);
    trackPlayheads_[static_cast<std::size_t>(trackIndex)] = 0;
    trackWriteheads_[static_cast<std::size_t>(trackIndex)] = lengthSamples;
    trackRecordInputGateTransport_[static_cast<std::size_t>(trackIndex)] = -1;
    if (lengthSamples > 0)
    {
        claimTempoMasterIfVacant(trackIndex, false);
    }
    else if (tempoMasterTrack_ == trackIndex)
    {
        tempoMasterTrack_ = -1;
        tempoMasterIsImport_ = false;
        for (int i = 0; i < kMaxTracks; ++i)
        {
            if (i != trackIndex && tracks_[static_cast<std::size_t>(i)].lengthSamples() > 0)
            {
                tempoMasterTrack_ = i;
                break;
            }
        }
        // Idle start wipes loop audio without Clear. A tempo the user typed stays.
        // A tempo that only came from a loop can be set again by the next first loop.
        if (tempoMasterTrack_ < 0 && !userTempoCommitted_)
        {
            tempoManager_.resetToEmpty();
        }
    }
    resetStretchForTrack(trackIndex);
    {
        const auto ti = static_cast<std::size_t>(trackIndex);
        const bool silent = trackStateIsSilent(state) || track.mix().muted || state == TrackState::Stopped
            || state == TrackState::Empty;
        muteGainTarget_[ti] = silent ? 0.0f : 1.0f;
        muteGain_[ti] = silent ? 0.0f : 1.0f;
    }
    if (waveformEnvelopes_ != nullptr)
    {
        waveformEnvelopes_->rebuildFromTrack(trackIndex, track, 0, true);
    }
    return true;
}

void LooperEngine::waitUntilAudioCallbackIdle() const noexcept
{
    if (audioCallbackDepth_.load(std::memory_order_acquire) == 0)
    {
        return;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(80);
    while (audioCallbackDepth_.load(std::memory_order_acquire) != 0)
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            break;
        }
        std::this_thread::yield();
    }
}

bool LooperEngine::importAudioFile(
    const int trackIndex,
    const float* const* channelData,
    const int numChannels,
    const std::size_t numFrames,
    const bool deferTempoAssignment) noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks || channelData == nullptr || numFrames == 0)
    {
        return false;
    }
    const float* left = channelData[0];
    const float* right = (numChannels >= 2 && channelData[1] != nullptr) ? channelData[1] : left;
    auto& track = tracks_[static_cast<std::size_t>(trackIndex)];
    const auto tiu = static_cast<std::size_t>(trackIndex);
    const auto prior = track.state();
    const bool fadeOutFirst = trackStateAdvancesPlayhead(prior) && !trackStateIsSilent(prior)
        && track.lengthSamples() > 1;
    if (fadeOutFirst)
    {
        muteGainTarget_[tiu] = 0.0f;
        waitUntilAudioCallbackIdle();
    }
    muteGain_[tiu] = 0.0f;
    muteGainTarget_[tiu] = 0.0f;
    track.setState(TrackState::Stopped);
    track.freezeAudioBuffer();
    waitUntilAudioCallbackIdle();
    track.importReplaceStereo(left, right, numFrames);
    track.unfreezeAudioBuffer();
    if (track.lengthSamples() == 0)
    {
        return false;
    }
    trackPlayheads_[tiu] = 0;
    trackWriteheads_[tiu] = numFrames;
    trackRecordInputGateTransport_[tiu] = -1;
    claimTempoMasterIfVacant(trackIndex, true);
    if (!deferTempoAssignment)
    {
        if (shouldSetTempoFromThisTrack(trackIndex))
        {
            applyTempoDetectionForSoleLoop(trackIndex);
        }
        else
        {
            assignReferenceBpmForLoop(track);
        }
    }
    resetStretchForTrack(trackIndex);
    if (waveformEnvelopes_ != nullptr)
    {
        waveformEnvelopes_->rebuildFromTrack(trackIndex, track, 0, true);
    }
    return true;
}

bool LooperEngine::copyTrackStem(const int trackIndex, std::vector<float>& left, std::vector<float>& right) const
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        left.clear();
        right.clear();
        return false;
    }
    return tracks_[static_cast<std::size_t>(trackIndex)].copyLockedLoopAudio(left, right);
}

TempoDetectionResult LooperEngine::analyzeImportTempo(const int trackIndex) const noexcept
{
    TempoDetectionResult out;
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return out;
    }
    const auto& track = tracks_[static_cast<std::size_t>(trackIndex)];
    const auto len = track.lengthSamples();
    if (len == 0)
    {
        return out;
    }
    return AudioImportAnalysisService::analyzeTrack(
        track, transport_.sampleRate(), transport_.tempoBpm(), globalBarLengthBars_, quartersPerNotatedBar());
}

void LooperEngine::applyImportTempoDecisions(
    const int trackIndex,
    const bool requestedAlignSessionTempo,
    const bool enableStretchToMaster,
    const double overrideContentBpm,
    const bool treatAsOneShot,
    const bool skipOnsetDetection) noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return;
    }
    const auto ti = static_cast<std::size_t>(trackIndex);
    auto& track = tracks_[ti];
    if (track.lengthSamples() == 0)
    {
        return;
    }
    // A master index left on a track whose audio was cleared is not ownership.
    // The only remaining loop, on any track, takes it.
    claimTempoMasterIfVacant(trackIndex, true);
    // A typed tempo stays. A lock that no loop still owns does not block this one.
    if (requestedAlignSessionTempo && trackIsSoleLoop(trackIndex) && !userTempoCommitted_ && tempoManager_.isLocked())
    {
        tempoManager_.resetToEmpty();
    }
    // A UI request is not tempo ownership. In multi-file imports the first
    // loaded track owns adoption even if a follower's metadata resolves first.
    const bool alignSessionTempoToDetected = requestedAlignSessionTempo
        && !tempoManager_.isLocked() && shouldSetTempoFromThisTrack(trackIndex);

    TempoDetectionResult a{};
    if (!skipOnsetDetection)
    {
        a = analyzeImportTempo(trackIndex);
    }
    constexpr double kMinPlausible = 15.0;
    constexpr double kMaxPlausible = 640.0;

    double contentBpm = transport_.tempoBpm();
    const int wantBars = pendingLoopBars_[ti];
    const double sr = transport_.sampleRate();
    if (overrideContentBpm > 1.0)
    {
        // Filename / title BPM is authoritative — do not fold it to the detector's 40–240 band.
        contentBpm = std::clamp(overrideContentBpm, 30.0, 320.0);
    }
    else if (alignSessionTempoToDetected)
    {
        // First imported loop uses the same 2/4/8 duration family as a live first take.
        // Fixed 4-quarter intrinsic clocks a short 120 BPM file at ~198 and a 2-bar file at 60.
        applyLiveTakeDurationTempo(track, wantBars);
        const double fromCycle = tempoBpmFromCycleFrames(track.cycleLengthSamples(), track.beatsPerCycle());
        if (fromCycle >= kMinPlausible && fromCycle <= kMaxPlausible)
        {
            contentBpm = fromCycle;
        }
        else if (a.detectedBpm > 1.0 && a.estimateUsable)
        {
            contentBpm = std::round(std::clamp(a.detectedBpm, kTempoDetectionBpmMin, kTempoDetectionBpmMax));
        }
        else if (a.intrinsicBpm >= kMinPlausible && a.intrinsicBpm <= kMaxPlausible)
        {
            contentBpm = a.intrinsicBpm;
        }
    }
    else if (enableStretchToMaster)
    {
        // Fallback when the caller did not pass the preview's source BPM. Playback then
        // pitch-locks to the master; a wrong source BPM changes speed, not pitch.
        const double seconds = (sr > 1.0) ? static_cast<double>(track.lengthSamples()) / sr : 0.0;
        const double fromLen = estimateDurationFamilyBpm(seconds, transport_.tempoBpm());
        if (fromLen >= kMinPlausible && fromLen <= kMaxPlausible)
        {
            contentBpm = fromLen;
        }
    }
    else if (a.detectedBpm > 1.0 && a.estimateUsable)
    {
        contentBpm = std::round(std::clamp(a.detectedBpm, kTempoDetectionBpmMin, kTempoDetectionBpmMax));
    }
    else if (a.intrinsicBpm >= kMinPlausible && a.intrinsicBpm <= kMaxPlausible)
    {
        contentBpm = a.intrinsicBpm;
    }
    else if (skipOnsetDetection)
    {
        const double sole = intrinsicTempoBpmForLoopLength(track.lengthSamples(), true);
        if (sole >= kMinPlausible && sole <= kMaxPlausible)
        {
            contentBpm = sole;
        }
    }

    if (wantBars > 0 && overrideContentBpm <= 1.0 && sr > 1.0 && alignSessionTempoToDetected)
    {
        const double fromBars = static_cast<double>(wantBars) * quartersPerNotatedBar() * 60.0 * sr
            / static_cast<double>(track.lengthSamples());
        if (fromBars >= kMinPlausible && fromBars <= kMaxPlausible)
        {
            contentBpm = fromBars;
        }
    }

    if (alignSessionTempoToDetected && contentBpm > 1.0 && !tempoManager_.isLocked())
    {
        if (overrideContentBpm > 1.0)
        {
            setTempo(contentBpm);
        }
        else if (a.estimateUsable)
        {
            setTempo(std::clamp(contentBpm, kTempoDetectionBpmMin, kTempoDetectionBpmMax));
        }
        else
        {
            setTempo(std::clamp(contentBpm, 40.0, 240.0));
        }
        lockSessionTempoFromLoop();
    }

    const double masterNow = transport_.tempoBpm();
    if (enableStretchToMaster && !treatAsOneShot)
    {
        track.setReferenceBpm(contentBpm > 1.0e-6 ? contentBpm : masterNow);
    }
    else
    {
        track.setReferenceBpm(masterNow);
    }

    resetStretchForTrack(trackIndex);
    pendingLoopBars_[ti] = 0;
    if (wantBars > 0)
    {
        applyLiveTakeDurationTempo(track, wantBars);
        (void) rescaleLoopToBarCount(trackIndex, wantBars);
    }
}

std::size_t LooperEngine::samplesPerNotatedBar() const noexcept
{
    return static_cast<std::size_t>(
        std::max(std::int64_t{1}, static_cast<std::int64_t>(std::llround(transport_.samplesPerBeat() * quartersPerNotatedBar()))));
}

std::size_t LooperEngine::lengthQuantizeUnitSamples() const noexcept
{
    const auto bar = samplesPerNotatedBar();
    if (bar == 0)
    {
        return 0;
    }
    switch (lengthQuantizeGrid_)
    {
        case LengthQuantizeGrid::Eighth:
            return std::max(std::size_t{1}, bar / 8);
        case LengthQuantizeGrid::Quarter:
            return std::max(std::size_t{1}, bar / 4);
        case LengthQuantizeGrid::Half:
            return std::max(std::size_t{1}, bar / 2);
        case LengthQuantizeGrid::Bar:
            return bar;
        case LengthQuantizeGrid::Off:
        default:
            return 0;
    }
}

std::size_t LooperEngine::quantizeLengthSamples(const std::size_t lengthSamples) const noexcept
{
    const auto unit = lengthQuantizeUnitSamples();
    if (unit == 0 || lengthSamples == 0)
    {
        return lengthSamples;
    }
    const auto n = std::max<std::size_t>(1, (lengthSamples + unit / 2) / unit);
    constexpr std::size_t kMaxUnits = 512;
    return std::min(n, kMaxUnits) * unit;
}

bool LooperEngine::shouldApplyLengthQuantize(const int trackIndex) const noexcept
{
    if (lengthQuantizeGrid_ == LengthQuantizeGrid::Off || trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return false;
    }
    if (followerRecordUsesTrackSync(trackIndex))
    {
        return false;
    }
    // The take that defines session tempo keeps its raw duration. A tempo the user
    // already typed is the grid, so that first loop snaps to it like any later take.
    if (shouldSetTempoFromThisTrack(trackIndex) && !tempoManager_.isLocked())
    {
        return false;
    }
    return true;
}

void LooperEngine::applyLengthQuantizeToTrack(Track& track) noexcept
{
    const auto oldLen = track.lengthSamples();
    const auto next = quantizeLengthSamples(oldLen);
    if (next == 0 || next == oldLen)
    {
        return;
    }
    if (next > oldLen)
    {
        if (!track.ensureRealtimeCapacity(next)) return;
        track.silenceRange(oldLen,next);
    }
    track.setLengthSamples(next);
    track.setRecordedSamples(next);
}

bool LooperEngine::trimLoopByBars(const int trackIndex, const int deltaBars) noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks || deltaBars == 0)
    {
        return false;
    }
    auto& track = tracks_[static_cast<std::size_t>(trackIndex)];
    const auto st = track.state();
    if (st == TrackState::Empty || st == TrackState::Recording || st == TrackState::Armed)
    {
        return false;
    }
    const std::size_t len = track.lengthSamples();
    if (len == 0)
    {
        return false;
    }
    const std::size_t bar = samplesPerNotatedBar();
    const std::size_t unit = lengthQuantizeUnitSamples();
    const std::size_t minLen = unit > 0 ? unit : std::max(std::size_t{1}, bar / 8);
    const std::size_t cap = track.bufferCapacity();
    std::size_t next = len;
    if (deltaBars < 0)
    {
        const auto steps = static_cast<std::size_t>(-deltaBars);
        const std::size_t cut = bar * steps;
        if (cut == 0 || len <= minLen)
        {
            return false;
        }
        next = (cut >= len) ? minLen : std::max(minLen, len - cut);
    }
    else
    {
        const auto steps = static_cast<std::size_t>(deltaBars);
        const std::size_t add = bar * steps;
        if (add == 0 || cap <= len)
        {
            return false;
        }
        next = std::min(cap, len + add);
    }
    if (next == len)
    {
        return false;
    }

    const int oldCycles = track.cycleCount();
    track.setLengthSamples(next);
    {
        const int newCycles = std::max(1,
            static_cast<int>(std::llround(static_cast<double>(oldCycles) * static_cast<double>(next)
                / static_cast<double>(len))));
        track.setCycleCount(newCycles);
    }
    if (track.recordedSamples() > next)
    {
        track.setRecordedSamples(next);
    }
    else if (deltaBars > 0 && track.recordedSamples() < next)
    {
        track.setRecordedSamples(next);
    }

    const auto ti = static_cast<std::size_t>(trackIndex);
    trackPlayheads_[ti] = (next > 0) ? (trackPlayheads_[ti] % next) : 0;
    if (trackWriteheads_[ti] > next)
    {
        trackWriteheads_[ti] = next;
    }
    resetStretchForTrack(trackIndex);
    return true;
}

int LooperEngine::loopWholeBarCount(const int trackIndex) const noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return 0;
    }
    const auto& track = tracks_[static_cast<std::size_t>(trackIndex)];
    const auto st = track.state();
    const std::size_t len =
        (st == TrackState::Recording) ? track.recordedSamples() : track.lengthSamples();
    if (len == 0)
    {
        return pendingLoopBars_[static_cast<std::size_t>(trackIndex)];
    }
    const std::size_t bar = samplesPerNotatedBar();
    if (bar == 0)
    {
        return 0;
    }
    const int bars = static_cast<int>(std::llround(static_cast<double>(len) / static_cast<double>(bar)));
    return std::max(1, bars);
}

int LooperEngine::pendingLoopBars(const int trackIndex) const noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return 0;
    }
    return pendingLoopBars_[static_cast<std::size_t>(trackIndex)];
}

bool LooperEngine::rescaleLoopToBarCount(const int trackIndex, const int newBars) noexcept
{
    constexpr int kMaxLoopBars = 64;
    if (trackIndex < 0 || trackIndex >= kMaxTracks || newBars < 1 || newBars > kMaxLoopBars)
    {
        return false;
    }
    auto& track = tracks_[static_cast<std::size_t>(trackIndex)];
    const auto st = track.state();
    if (st == TrackState::Empty || st == TrackState::Armed || track.lengthSamples() == 0)
    {
        if (st == TrackState::Recording)
        {
            return false;
        }
        auto& pending = pendingLoopBars_[static_cast<std::size_t>(trackIndex)];
        if (pending == newBars)
        {
            return false;
        }
        pending = newBars;
        return true;
    }
    if (st == TrackState::Recording)
    {
        return false;
    }
    const std::size_t oldLen = track.lengthSamples();
    if (oldLen == 0)
    {
        return false;
    }
    const std::size_t bar = samplesPerNotatedBar();
    if (bar == 0)
    {
        return false;
    }
    if (static_cast<std::size_t>(newBars) > (std::numeric_limits<std::size_t>::max() / bar))
    {
        return false;
    }
    const int oldBars = std::max(1, loopWholeBarCount(trackIndex));
    const int oldCycles = track.cycleCount();
    const std::size_t newLen = bar * static_cast<std::size_t>(newBars);
    if (newLen == oldLen)
    {
        return false;
    }

    if (newLen > oldLen)
    {
        track.ensureCapacity(newLen);
        const int chans = track.channels();
        for (int c = 0; c < chans; ++c)
        {
            for (std::size_t i = oldLen; i < newLen; ++i)
            {
                track.writeSample(c, i, track.readSample(c, i % oldLen));
            }
        }
    }

    track.setLengthSamples(newLen);
    track.setRecordedSamples(newLen);
    track.setCycleCount(std::max(1, (oldCycles * newBars + oldBars / 2) / oldBars));

    const auto ti = static_cast<std::size_t>(trackIndex);
    trackPlayheads_[ti] = (newLen > 0) ? (trackPlayheads_[ti] % newLen) : 0;
    if (trackWriteheads_[ti] > newLen)
    {
        trackWriteheads_[ti] = newLen;
    }
    resetStretchForTrack(trackIndex);
    pendingLoopBars_[ti] = 0;
    if (waveformEnvelopes_ != nullptr)
    {
        waveformEnvelopes_->rebuildFromTrack(trackIndex, track);
    }
    return true;
}

bool LooperEngine::reverseTrack(const int trackIndex) noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return false;
    }
    auto& track = tracks_[static_cast<std::size_t>(trackIndex)];
    const auto st = track.state();
    if (st == TrackState::Empty || st == TrackState::Recording || st == TrackState::Armed
        || trackStateIsOverdub(st) || st == TrackState::Playing_DubPending)
    {
        return false;
    }
    const std::size_t len = track.lengthSamples();
    if (len < 2)
    {
        return false;
    }

    track.freezeAudioBuffer();
    waitUntilAudioCallbackIdle();
    track.captureOverdubUndoSnapshot();
    if (!track.reverseLiveAudio())
    {
        track.unfreezeAudioBuffer();
        return false;
    }

    const auto ti = static_cast<std::size_t>(trackIndex);
    trackPlayheads_[ti] = (len - (trackPlayheads_[ti] % len)) % len;
    if (trackWriteheads_[ti] > 0)
    {
        trackWriteheads_[ti] = (len - (trackWriteheads_[ti] % len)) % len;
    }
    resetStretchForTrack(trackIndex);
    track.unfreezeAudioBuffer();
    if (waveformEnvelopes_ != nullptr)
    {
        waveformEnvelopes_->rebuildFromTrack(trackIndex, track, 0, true);
    }
    return true;
}

const std::array<Track, LooperEngine::kMaxTracks>& LooperEngine::tracks() const noexcept
{
    return tracks_;
}

const TransportClock& LooperEngine::transport() const noexcept
{
    return transport_;
}

int LooperEngine::countInBars() const noexcept
{
    return countInBars_;
}

int LooperEngine::quantizeDivision() const noexcept
{
    return quantizeDivision_;
}

SyncMode LooperEngine::syncMode() const noexcept
{
    return syncMode_;
}

void LooperEngine::setTrackStereoBusProcessor(TrackStereoBusProcessFn fn) noexcept
{
    trackStereoBusProcessor_ = std::move(fn);
    if (trackStereoBusProcessor_)
    {
        for (auto& io : trackIo_)
        {
            io.hardwareOutputStartChannel = TrackIoRouting::kSilentOutput;
        }
    }
}

void LooperEngine::setMasterGain(float gain01) noexcept
{
    masterGain_.store(clamp01(gain01), std::memory_order_relaxed);
}

float LooperEngine::masterGain() const noexcept
{
    return masterGain_.load(std::memory_order_relaxed);
}

void LooperEngine::setDeferDeviceMasterGain(const bool on) noexcept
{
    deferDeviceMasterGain_.store(on, std::memory_order_relaxed);
}

bool LooperEngine::deferDeviceMasterGain() const noexcept
{
    return deferDeviceMasterGain_.load(std::memory_order_relaxed);
}

void LooperEngine::applyMasterGainToOutputs(float* const* output, const int numOutputChannels, const int numSamples,
    float* meterLeft, float* meterRight) noexcept
{
    if (output == nullptr || numOutputChannels <= 0 || numSamples <= 0)
    {
        lastOutputPeak_.store(0.0f, std::memory_order_relaxed);
        return;
    }
    masterGainSmooth_.setTargetValue(masterGain_.load(std::memory_order_relaxed));
    float outPeak = 0.0f;
    auto scaleSample = [&](const float mg, const int sample) noexcept
    {
        for (int channel = 0; channel < numOutputChannels; ++channel)
        {
            if (output[channel] == nullptr)
            {
                continue;
            }
            const float v = output[channel][sample] * mg;
            output[channel][sample] = v;
            outPeak = std::max(outPeak, std::abs(v));
        }
        if (meterLeft != nullptr)
        {
            meterLeft[sample] *= mg;
            outPeak = std::max(outPeak, std::abs(meterLeft[sample]));
        }
        if (meterRight != nullptr)
        {
            meterRight[sample] *= mg;
            outPeak = std::max(outPeak, std::abs(meterRight[sample]));
        }
    };
    if (masterGainSmooth_.isSmoothing())
    {
        for (int sample = 0; sample < numSamples; ++sample)
        {
            scaleSample(masterGainSmooth_.getNextValue(), sample);
        }
    }
    else
    {
        const float mg = masterGainSmooth_.getCurrentValue();
        for (int sample = 0; sample < numSamples; ++sample)
        {
            scaleSample(mg, sample);
        }
    }
    lastOutputPeak_.store(outPeak, std::memory_order_relaxed);
}

void LooperEngine::setCrossfaderPosition(const float pos01) noexcept
{
    const float v = std::clamp(pos01, 0.0f, 1.0f);
    if (!std::isfinite(v))
    {
        return;
    }
    crossfaderTarget_.store(v, std::memory_order_relaxed);
}

float LooperEngine::crossfaderPosition() const noexcept
{
    return crossfaderTarget_.load(std::memory_order_relaxed);
}

void LooperEngine::setTrackCrossfadeAssign(const int trackIndex, const CrossfadeAssign assign) noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return;
    }
    const int raw = (assign == CrossfadeAssign::A) ? 1 : (assign == CrossfadeAssign::B ? 2 : 0);
    crossfadeAssign_[static_cast<std::size_t>(trackIndex)].store(raw, std::memory_order_relaxed);
}

void LooperEngine::toggleTrackCrossfadeAssign(const int trackIndex, const CrossfadeAssign side) noexcept
{
    if (side == CrossfadeAssign::Off)
    {
        setTrackCrossfadeAssign(trackIndex, CrossfadeAssign::Off);
        return;
    }
    const auto cur = trackCrossfadeAssign(trackIndex);
    setTrackCrossfadeAssign(trackIndex, cur == side ? CrossfadeAssign::Off : side);
}

CrossfadeAssign LooperEngine::trackCrossfadeAssign(const int trackIndex) const noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return CrossfadeAssign::Off;
    }
    const int raw = crossfadeAssign_[static_cast<std::size_t>(trackIndex)].load(std::memory_order_relaxed);
    if (raw == 1)
    {
        return CrossfadeAssign::A;
    }
    if (raw == 2)
    {
        return CrossfadeAssign::B;
    }
    return CrossfadeAssign::Off;
}

bool LooperEngine::crossfaderIsEngaged() const noexcept
{
    for (int t = 0; t < kMaxTracks; ++t)
    {
        if (trackCrossfadeAssign(t) != CrossfadeAssign::Off)
        {
            return true;
        }
    }
    return false;
}

float LooperEngine::trackCrossfadeGain(const int trackIndex) const noexcept
{
    if (!crossfaderIsEngaged())
    {
        return 1.0f;
    }
    const auto assign = trackCrossfadeAssign(trackIndex);
    if (assign == CrossfadeAssign::Off)
    {
        return 1.0f;
    }
    const float x = std::clamp(crossfaderSmooth_, 0.0f, 1.0f);
    constexpr float kHalfPi = 1.57079632679f;
    if (assign == CrossfadeAssign::A)
    {
        return std::cos(x * kHalfPi);
    }
    return std::sin(x * kHalfPi);
}

void LooperEngine::setFreeMode(bool on) noexcept
{
    freeMode_ = on;
}

bool LooperEngine::freeMode() const noexcept
{
    return freeMode_;
}

void LooperEngine::setGlobalStopMode(GlobalStopMode mode) noexcept
{
    globalStopMode_ = withoutBeatTiming(mode);
    applyGlobalDefaultsToAllTracks();
}

GlobalStopMode LooperEngine::globalStopMode() const noexcept
{
    return globalStopMode_;
}

void LooperEngine::setAfterRecordMode(AfterRecordMode mode) noexcept
{
    afterRecordMode_ = mode;
}

AfterRecordMode LooperEngine::afterRecordMode() const noexcept
{
    return afterRecordMode_;
}

void LooperEngine::setCountInStyle(CountInStyle style) noexcept
{
    countInStyle_ = withoutBeatTiming(style);
}

CountInStyle LooperEngine::countInStyle() const noexcept
{
    return countInStyle_;
}

void LooperEngine::setCountOutStyle(CountOutStyle style) noexcept
{
    countOutStyle_ = withoutBeatTiming(style);
}

CountOutStyle LooperEngine::countOutStyle() const noexcept
{
    return countOutStyle_;
}

void LooperEngine::setPlaybackStartMode(PlaybackStartMode mode) noexcept
{
    playbackStartMode_ = withoutBeatTiming(mode);
    applyGlobalDefaultsToAllTracks();
}

PlaybackStartMode LooperEngine::playbackStartMode() const noexcept
{
    return playbackStartMode_;
}

void LooperEngine::setPlayPhaseLockEnabled(const bool on) noexcept
{
    playPhaseLockEnabled_ = on;
}

bool LooperEngine::playPhaseLockEnabled() const noexcept
{
    return playPhaseLockEnabled_;
}

void LooperEngine::setPlayAllMode(PlaybackStartMode mode) noexcept
{
    playAllMode_ = withoutBeatTiming(mode);
}

PlaybackStartMode LooperEngine::playAllMode() const noexcept
{
    return playAllMode_;
}

void LooperEngine::setStopAllMode(GlobalStopMode mode) noexcept
{
    stopAllMode_ = withoutBeatTiming(mode);
}

GlobalStopMode LooperEngine::stopAllMode() const noexcept
{
    return stopAllMode_;
}

void LooperEngine::setRecordStartMode(PlaybackStartMode mode) noexcept
{
    recordStartMode_ = withoutBeatTiming(mode);
    applyGlobalDefaultsToAllTracks();
}

PlaybackStartMode LooperEngine::recordStartMode() const noexcept
{
    return recordStartMode_;
}

void LooperEngine::setOverdubStartMode(PlaybackStartMode mode) noexcept
{
    overdubStartMode_ = withoutBeatTiming(mode);
    applyGlobalDefaultsToAllTracks();
}

PlaybackStartMode LooperEngine::overdubStartMode() const noexcept
{
    return overdubStartMode_;
}

void LooperEngine::setMuteStartMode(PlaybackStartMode mode) noexcept
{
    muteStartMode_ = withoutBeatTiming(mode);
    applyGlobalDefaultsToAllTracks();
}

PlaybackStartMode LooperEngine::muteStartMode() const noexcept
{
    return muteStartMode_;
}

void LooperEngine::setUndoStartMode(PlaybackStartMode mode) noexcept
{
    undoStartMode_ = withoutBeatTiming(mode);
}

PlaybackStartMode LooperEngine::undoStartMode() const noexcept
{
    return undoStartMode_;
}

void LooperEngine::setFxBypassSyncMode(const FxBypassSyncMode mode) noexcept
{
    fxBypassSyncMode_ = mode == FxBypassSyncMode::Loop ? FxBypassSyncMode::Loop
        : (mode == FxBypassSyncMode::Bar ? FxBypassSyncMode::Bar : FxBypassSyncMode::Immediate);
}

FxBypassSyncMode LooperEngine::fxBypassSyncMode() const noexcept
{
    return fxBypassSyncMode_;
}

std::int64_t LooperEngine::fxBypassExecuteSample(const FxBypassSyncMode mode, std::int64_t pos) const noexcept
{
    if (pos < 0)
    {
        pos = transport_.samplePosition();
    }
    if (mode == FxBypassSyncMode::Immediate)
    {
        return pos;
    }
    if (mode == FxBypassSyncMode::Bar)
    {
        return std::max(pos, targetSampleForTimingMode(TimingMode::Bar, -1, pos));
    }
    const auto len = static_cast<std::int64_t>(longestLoopLengthSamples());
    if (len < 2)
    {
        return pos;
    }
    const auto phMod = static_cast<std::int64_t>(loopPositionFromTransport(pos, sharedLoopPhaseOrigin_, static_cast<std::size_t>(len)));
    if (phMod == 0)
    {
        return pos + len;
    }
    return pos + (len - phMod);
}

std::int64_t LooperEngine::quantizedPlayExecuteSample(const int trackIndex, const PlaybackStartMode mode, const std::int64_t atSample) const noexcept
{
    const auto pos = atSample >= 0 ? atSample : transport_.samplePosition();
    return alignExecuteSampleForPlaybackMode(trackIndex, pos, pos, mode);
}

void LooperEngine::setGlobalBarLengthBars(int bars) noexcept
{
    if (bars == 1 || bars == 2 || bars == 4 || bars == 8)
    {
        globalBarLengthBars_ = bars;
    }
}

int LooperEngine::globalBarLengthBars() const noexcept
{
    return globalBarLengthBars_;
}

void LooperEngine::setLengthQuantizeGrid(const LengthQuantizeGrid grid) noexcept
{
    lengthQuantizeGrid_ = grid;
}

LengthQuantizeGrid LooperEngine::lengthQuantizeGrid() const noexcept
{
    return lengthQuantizeGrid_;
}

namespace
{
int normalizeTimeSigDenominator(int d) noexcept
{
    switch (d)
    {
        case 1:
        case 2:
        case 4:
        case 8:
        case 16:
            return d;
        default:
            return 4;
    }
}
} // namespace

void LooperEngine::setTimeSignature(const int numerator, const int denominator) noexcept
{
    timeSigNumerator_ = std::clamp(numerator, 1, 32);
    timeSigDenominator_ = normalizeTimeSigDenominator(denominator);
}

int LooperEngine::timeSigNumerator() const noexcept
{
    return timeSigNumerator_;
}

int LooperEngine::timeSigDenominator() const noexcept
{
    return timeSigDenominator_;
}

double LooperEngine::quartersPerNotatedBar() const noexcept
{
    return static_cast<double>(timeSigNumerator_) * 4.0 / static_cast<double>(timeSigDenominator_);
}

void LooperEngine::setBeatCountDisplay(const BeatCountDisplay mode) noexcept
{
    beatCountDisplay_ = mode;
}

BeatCountDisplay LooperEngine::beatCountDisplay() const noexcept
{
    return beatCountDisplay_;
}

void LooperEngine::setMetronomeMixToDevice(const bool on) noexcept
{
    metronomeMixToDevice_.store(on, std::memory_order_relaxed);
}

const float* LooperEngine::metronomeMixLeft() const noexcept
{
    return metroMixL_.data();
}

const float* LooperEngine::metronomeMixRight() const noexcept
{
    return metroMixR_.data();
}

int LooperEngine::metronomeMixFrames() const noexcept
{
    return metroMixValidSamples_;
}

void LooperEngine::setMetronomeEnabled(const bool on) noexcept
{
    metronomeEnabled_ = on;
    if (!on)
    {
        metroClickResidualLen_ = 0;
        metroClickResidualOff_ = 0;
        metronomeForceRunning_ = false;
    }
}

bool LooperEngine::metronomeEnabled() const noexcept
{
    return metronomeEnabled_;
}

bool LooperEngine::anyLoopPlaying() const noexcept
{
    for (int i = 0; i < kMaxTracks; ++i)
    {
        const auto& tr = tracks_[static_cast<std::size_t>(i)];
        if (tr.lengthSamples() == 0)
        {
            continue;
        }
        if (trackStateAdvancesPlayhead(tr.state()))
        {
            return true;
        }
    }
    return false;
}

bool LooperEngine::playStopAllShouldStop() const noexcept
{
    if (metronomeForceRunning_)
    {
        return true;
    }
    for (int i = 0; i < kMaxTracks; ++i)
    {
        const auto s = tracks_[static_cast<std::size_t>(i)].state();
        if (s == TrackState::Playing || s == TrackState::Recording || s == TrackState::Overdub
            || s == TrackState::Armed || trackStateAdvancesPlayhead(s))
        {
            return true;
        }
    }
    return false;
}

bool LooperEngine::hasQueuedCommandType(const CommandType type) const noexcept
{
    for (const auto& slot : scheduledCommands_)
    {
        if (slot.active && slot.command.type == type)
        {
            return true;
        }
    }
    return false;
}

void LooperEngine::forceIdleClock() noexcept
{
    metronomeForceRunning_ = false;
    for (int i = 0; i < kMaxTracks; ++i)
    {
        auto& tr = tracks_[static_cast<std::size_t>(i)];
        const auto st = tr.state();
        if (st == TrackState::Empty || st == TrackState::Stopped)
        {
            continue;
        }
        if (tr.lengthSamples() > 0)
        {
            tr.setState(TrackState::Stopped);
        }
        else
        {
            tr.setState(TrackState::Empty);
        }
    }
    sharedLoopPhaseOrigin_ = transport_.samplePosition();
}

bool LooperEngine::anyLoopHasContent() const noexcept
{
    for (int i = 0; i < kMaxTracks; ++i)
    {
        if (tracks_[static_cast<std::size_t>(i)].lengthSamples() > 0)
        {
            return true;
        }
    }
    return false;
}

void LooperEngine::refreshLoopCrossfadeSamples() noexcept
{
    const double sr = transport_.sampleRate();
    const int n = static_cast<int>(std::lround(static_cast<double>(loopCrossfadeMs_) * sr * 0.001));
    loopCrossfadeSamples_ = std::clamp(n, 0, 4096);
}

void LooperEngine::setOverdubFeedback(const float gain01) noexcept
{
    overdubFeedback_ = clamp01(gain01);
}

float LooperEngine::overdubFeedback() const noexcept
{
    return overdubFeedback_;
}

void LooperEngine::setLoopCrossfadeMilliseconds(const float ms) noexcept
{
    loopCrossfadeMs_ = std::clamp(ms, 0.0f, 40.0f);
    refreshLoopCrossfadeSamples();
}

float LooperEngine::loopCrossfadeMilliseconds() const noexcept
{
    return loopCrossfadeMs_;
}

void LooperEngine::setRecordLatencyCompensationSamples(const int samples) noexcept
{
    constexpr int kMaxComp = 192000;
    recordLatencyCompensationSamples_.store(std::clamp(samples, 0, kMaxComp), std::memory_order_relaxed);
}

int LooperEngine::recordLatencyCompensationSamples() const noexcept
{
    return recordLatencyCompensationSamples_.load(std::memory_order_relaxed);
}

void LooperEngine::reserveOverdubHistoryForKnownLoops() noexcept
{
    for (int i = 0; i < kMaxTracks; ++i)
    {
        auto& track = tracks_[static_cast<std::size_t>(i)];
        const auto n = track.lengthSamples();
        if (n >= 2)
        {
            track.reserveOverdubHistory(n);
        }
    }
}

void LooperEngine::prepareLiveRecordHeadroom() noexcept
{
    const double sr = transport_.sampleRate();
    const auto headroom = static_cast<std::size_t>(std::llround(std::max(sr, 44100.0) * kMaxFirstPassSeconds + kMetroScratchCap));
    for (int i = 0; i < kMaxTracks; ++i)
    {
        tracks_[static_cast<std::size_t>(i)].reserveRecordHeadroom(headroom);
    }
}

void LooperEngine::hintFirstPassRecordOffset(const int offsetInBlock) noexcept
{
    firstPassRecordOffsetInBlock_.store(std::max(0, offsetInBlock), std::memory_order_release);
}

int LooperEngine::effectiveRecordLatencyCompensationSamples(const int trackIndex) const noexcept
{
    const int hw = recordLatencyCompensationSamples_.load(std::memory_order_relaxed);
    if (hw <= 0 || trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return std::max(0, hw);
    }
    switch (trackIo_[static_cast<std::size_t>(trackIndex)].recordInput)
    {
        case TrackRecordInput::PixelBeat:
        case TrackRecordInput::InstrumentPlugin:
        case TrackRecordInput::MixerBus:
            return 0;
        case TrackRecordInput::MixerInput:
        case TrackRecordInput::Hardware:
        default:
            return hw;
    }
}

void LooperEngine::setPlayAllStartsClickWhenEmpty(const bool on) noexcept
{
    playAllStartsClickWhenEmpty_ = on;
    if (!on)
    {
        metronomeForceRunning_ = false;
    }
}

bool LooperEngine::playAllStartsClickWhenEmpty() const noexcept
{
    return playAllStartsClickWhenEmpty_;
}

void LooperEngine::setLinkTransportImmediate(const bool on) noexcept
{
    linkTransportImmediate_.store(on, std::memory_order_relaxed);
}

void LooperEngine::setLinkQuantumLaunchSample(const std::int64_t transportSample) noexcept
{
    linkQuantumLaunchSample_.store(transportSample, std::memory_order_relaxed);
}

bool LooperEngine::metronomeForceRunning() const noexcept
{
    return metronomeForceRunning_;
}

void LooperEngine::setTrackDirectMonitor(const int trackIndex, const bool on) noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return;
    }
    auto m = tracks_[static_cast<std::size_t>(trackIndex)].mix();
    if (m.directMonitor == on)
    {
        return;
    }
    m.directMonitor = on;
    tracks_[static_cast<std::size_t>(trackIndex)].setMix(m);
}

void LooperEngine::toggleTrackDirectMonitor(const int trackIndex) noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return;
    }
    auto m = tracks_[static_cast<std::size_t>(trackIndex)].mix();
    m.directMonitor = !m.directMonitor;
    tracks_[static_cast<std::size_t>(trackIndex)].setMix(m);
}

bool LooperEngine::trackDirectMonitor(const int trackIndex) const noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return false;
    }
    return tracks_[static_cast<std::size_t>(trackIndex)].mix().directMonitor;
}

void LooperEngine::setMetronomeGain(const float gain01) noexcept
{
    metronomeGain_ = clamp01(gain01);
}

float LooperEngine::metronomeGain() const noexcept
{
    return metronomeGain_;
}

void LooperEngine::setMetronomeOutputChannel(const int channelIndex) noexcept
{
    metronomeOutCh_ = channelIndex;
}

int LooperEngine::metronomeOutputChannel() const noexcept
{
    return metronomeOutCh_;
}

void LooperEngine::setMetronomeStereoPair(const bool on) noexcept
{
    metronomeStereoPair_ = on;
}

bool LooperEngine::metronomeStereoPair() const noexcept
{
    return metronomeStereoPair_;
}

void LooperEngine::setMetronomeNormalSound(const MetronomeSoundKind k) noexcept
{
    metroNormalKind_ = k;
}

void LooperEngine::setMetronomeAccentSound(const MetronomeSoundKind k) noexcept
{
    metroAccentKind_ = k;
}

MetronomeSoundKind LooperEngine::metronomeNormalSound() const noexcept
{
    return metroNormalKind_;
}

MetronomeSoundKind LooperEngine::metronomeAccentSound() const noexcept
{
    return metroAccentKind_;
}

void LooperEngine::setMetronomeNormalCustom(std::vector<float> samples) noexcept
{
    const int wi = 1 - metroNormalReadIndex_.load(std::memory_order_acquire);
    metroNormalCustomBuf_[static_cast<std::size_t>(wi)] = std::move(samples);
    metroNormalReadIndex_.store(wi, std::memory_order_release);
}

void LooperEngine::setMetronomeAccentCustom(std::vector<float> samples) noexcept
{
    const int wi = 1 - metroAccentReadIndex_.load(std::memory_order_acquire);
    metroAccentCustomBuf_[static_cast<std::size_t>(wi)] = std::move(samples);
    metroAccentReadIndex_.store(wi, std::memory_order_release);
}

void LooperEngine::applyGlobalDefaultsToAllTracks(const bool forceIncludingOverrides) noexcept
{
    for (int t = 0; t < kMaxTracks; ++t)
    {
        auto& tr = tracks_[static_cast<std::size_t>(t)];
        TrackLoopPrefs p = tr.loopPrefs();
        if (!forceIncludingOverrides && p.overrideGlobal)
        {
            continue;
        }
        p.stopMode = static_cast<uint8_t>(globalStopMode_);
        p.playMode = static_cast<uint8_t>(playbackStartMode_);
        p.recordMode = static_cast<uint8_t>(recordStartMode_);
        p.overdubMode = static_cast<uint8_t>(overdubStartMode_);
        p.muteMode = static_cast<uint8_t>(muteStartMode_);
        p.afterRecordMode = static_cast<uint8_t>(afterRecordMode_);
        if (forceIncludingOverrides)
        {
            p.overrideGlobal = false;
        }
        tr.setLoopPrefs(p);
    }
}

void LooperEngine::syncAllTracksToStart() noexcept
{
    (void) enqueueCommand({CommandType::SyncAllToStart, 0});
}

void LooperEngine::setTrackLoopPhaseOrigin(const int trackIndex, const std::int64_t originSamples) noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return;
    }
    sharedLoopPhaseOrigin_ = originSamples;
}

std::int64_t LooperEngine::trackLoopPhaseOrigin(const int trackIndex) const noexcept
{
    return playbackPhaseOrigin(trackIndex);
}

void LooperEngine::setMultiTrackRecord(bool on) noexcept
{
    multiTrackRecord_ = on;
}

bool LooperEngine::multiTrackRecord() const noexcept
{
    return multiTrackRecord_;
}

bool LooperEngine::canUndoOverdubAny() const noexcept
{
    for (const auto& t : tracks_)
    {
        if (t.hasOverdubUndoSnapshot())
        {
            return true;
        }
    }
    return false;
}

bool LooperEngine::canRedoOverdubAny() const noexcept
{
    for (const auto& t : tracks_)
    {
        if (t.hasOverdubRedoSnapshot())
        {
            return true;
        }
    }
    return false;
}

bool LooperEngine::trackIsSoleLoop(const int trackIndex) const noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return false;
    }
    if (tracks_[static_cast<std::size_t>(trackIndex)].lengthSamples() == 0)
    {
        return false;
    }
    for (int i = 0; i < kMaxTracks; ++i)
    {
        if (i != trackIndex && tracks_[static_cast<std::size_t>(i)].lengthSamples() > 0)
        {
            return false;
        }
    }
    return true;
}

bool LooperEngine::shouldSetTempoFromThisTrack(const int trackIndex) const noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks
        || tracks_[static_cast<std::size_t>(trackIndex)].lengthSamples() == 0)
    {
        return false;
    }
    if (tempoMasterTrack_ == trackIndex)
    {
        return true;
    }
    // The first loop with audio sets the tempo even when the master index still
    // points at a track that no longer has a loop.
    return tempoMasterVacant() && trackIsSoleLoop(trackIndex);
}

std::int64_t LooperEngine::transportSampleAtNotatedBeatFloor(const std::int64_t pos) const noexcept
{
    if (pos <= 0)
    {
        return 0;
    }
    const double spb = transport_.samplesPerBeat();
    if (spb < 1.0)
    {
        return pos;
    }
    const double beats = static_cast<double>(pos) / spb;
    const auto beatIndex = static_cast<std::int64_t>(std::floor(beats));
    return static_cast<std::int64_t>(std::floor(static_cast<double>(beatIndex) * spb));
}

bool LooperEngine::hasOtherLoopWithActivePlayback(const int exceptTrackIndex) const noexcept
{
    if (exceptTrackIndex < 0 || exceptTrackIndex >= kMaxTracks)
    {
        return false;
    }
    for (int i = 0; i < kMaxTracks; ++i)
    {
        if (i == exceptTrackIndex)
        {
            continue;
        }
        const auto& tr = tracks_[static_cast<std::size_t>(i)];
        if (tr.lengthSamples() == 0)
        {
            continue;
        }
        const auto s = tr.state();
        if (trackStateAdvancesPlayhead(s))
        {
            return true;
        }
    }
    return false;
}

bool LooperEngine::hasRecordTimingContext(const int exceptTrackIndex) const noexcept
{
    return metronomeEnabled_ || hasOtherLoopWithActivePlayback(exceptTrackIndex);
}

bool LooperEngine::clockGridIsActive() const noexcept
{
    return metronomeForceRunning_ && userTempoCommitted_;
}

bool LooperEngine::emptySessionFirstTakeIsImmediate() const noexcept
{
    for (const auto& t : tracks_)
    {
        if (t.lengthSamples() > 0)
        {
            return false;
        }
    }
    // Typed tempo plus a running clock is a real grid. Record/stop then follow
    // the track cue, or the global cue when the track is not overriding it.
    if (clockGridIsActive())
    {
        return false;
    }
    return true;
}

bool LooperEngine::trackRecordsInternalGraph(const int trackIndex) const noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return false;
    }
    switch (trackIo_[static_cast<std::size_t>(trackIndex)].recordInput)
    {
        case TrackRecordInput::PixelBeat:
        case TrackRecordInput::InstrumentPlugin:
        case TrackRecordInput::MixerBus:
            return true;
        default:
            return false;
    }
}

bool LooperEngine::idlePlaybackStartsImmediately(const EngineCommand& command) const noexcept
{
    if (anyTrackPlayingOrPendingLaunch())
    {
        return false;
    }
    // The clock is the grid. Play All still toggles it immediately; track cues wait.
    if (clockGridIsActive() && command.type != CommandType::PlayAll)
    {
        return false;
    }
    if (command.type == CommandType::PlayAll)
    {
        return true;
    }
    return commandStartsPlayback(command);
}

bool LooperEngine::anyTrackPlayingOrPendingLaunch() const noexcept
{
    for (int i = 0; i < kMaxTracks; ++i)
    {
        const auto s = tracks_[static_cast<std::size_t>(i)].state();
        if (s == TrackState::Recording || s == TrackState::Armed || trackStateAdvancesPlayhead(s))
        {
            return true;
        }
    }
    for (const auto& slot : scheduledCommands_)
    {
        if (slot.active && commandStartsPlayback(slot.command))
        {
            return true;
        }
    }
    return false;
}

bool LooperEngine::anyTrackAdvancingOrRecording() const noexcept
{
    for (int i = 0; i < kMaxTracks; ++i)
    {
        const auto s = tracks_[static_cast<std::size_t>(i)].state();
        if (s == TrackState::Recording || trackStateAdvancesPlayhead(s))
        {
            return true;
        }
    }
    return false;
}

bool LooperEngine::commandStartsPlayback(const EngineCommand& command) const noexcept
{
    if (command.type == CommandType::PlayAll)
    {
        return !playStopAllShouldStop();
    }
    if (command.trackIndex < 0 || command.trackIndex >= kMaxTracks)
    {
        return false;
    }
    const auto& tr = tracks_[static_cast<std::size_t>(command.trackIndex)];
    const auto st = tr.state();
    if (command.type == CommandType::Play || command.type == CommandType::PlayStopToggle)
    {
        return (st == TrackState::Stopped || st == TrackState::Empty) && tr.lengthSamples() > 0;
    }
    return false;
}

int LooperEngine::chooseQuarterNotesInLoopForTempo(const std::size_t len, const bool useTransportHint) const noexcept
{
    const double sr = transport_.sampleRate();
    if (len == 0 || sr <= 1.0)
    {
        return 4;
    }
    const double T = static_cast<double>(len) / sr;
    if (T <= 1.0e-9)
    {
        return 4;
    }

    const double qpb = quartersPerNotatedBar();
    if (qpb <= 1.0e-9)
    {
        return std::max(1, static_cast<int>(std::llround(std::clamp(transport_.tempoBpm(), 1.0, 400.0) * T / 60.0)));
    }

    if (!useTransportHint)
    {
        // Live / session: one loop = configured bar length of division-1 counts (4/4 → 4 quarters).
        const int g = std::max(1, globalBarLengthBars_);
        return std::max(1, static_cast<int>(std::llround(qpb * static_cast<double>(g))));
    }

    const double hint = std::clamp(transport_.tempoBpm(), 1.0, 400.0);
    const int nMin = std::max(1, static_cast<int>(std::floor(28.0 * T / 60.0)));
    const int nWide = static_cast<int>(std::ceil(265.0 * T / 60.0)) + 6;
    const int nMax = std::min(16384, std::max(nMin + 1, nWide));
    const int barSpanQuarters =
        std::max(1, static_cast<int>(std::llround(qpb * static_cast<double>(std::max(1, globalBarLengthBars_)))));

    int bestN = std::max(1, nMin);
    double bestScore = 1.0e18;
    bool any = false;
    for (int n = nMin; n <= nMax; ++n)
    {
        const double bpm = static_cast<double>(n) * 60.0 / T;
        if (bpm < 28.0 || bpm > 270.0)
        {
            continue;
        }
        any = true;
        double score = std::abs(bpm - hint);
        if ((n % barSpanQuarters) == 0)
        {
            score -= 0.35;
        }
        if (score < bestScore - 1.0e-9)
        {
            bestScore = score;
            bestN = n;
        }
    }

    if (!any)
    {
        return std::max(1, static_cast<int>(std::llround(hint * T / 60.0)));
    }

    return std::max(1, bestN);
}

double LooperEngine::tempoBpmFromCycleFrames(const std::size_t cycleFrames, const int beatsPerCycle) const noexcept
{
    if (cycleFrames == 0)
    {
        return 0.0;
    }
    const double sr = transport_.sampleRate();
    if (sr <= 1.0)
    {
        return 0.0;
    }
    const int beats = beatsPerCycle > 0 ? beatsPerCycle : std::max(1, timeSigNumerator_);
    const double raw = 60.0 * sr * static_cast<double>(beats) / static_cast<double>(cycleFrames);
    return foldMobiusTempo(raw, 40.0, 240.0);
}

void LooperEngine::applyLiveTakeDurationTempo(Track& track, const int assumedBars) noexcept
{
    const auto len = track.lengthSamples();
    const double sr = transport_.sampleRate();
    if (len == 0 || sr <= 1.0)
    {
        return;
    }
    if (assumedBars > 0)
    {
        track.setCycleCount(std::max(1, assumedBars));
        track.setBeatsPerCycle(std::max(1, timeSigNumerator_));
        return;
    }
    const int bar = std::max(1, timeSigNumerator_);
    const double seconds = static_cast<double>(len) / sr;
    if (seconds <= 1.0e-9)
    {
        return;
    }
    // One bar of time-signature beats, then 1/2 or 2× bars so BPM lands in a playable range.
    // 4/4 → 2, 4, or 8 beats (then 16 if the take is still very long).
    constexpr double kBarBpmHigh = 180.0;
    constexpr double kBarBpmLow = 70.0;
    int beatsPerCycle = bar;
    int cycles = 1;
    double bpmBar = static_cast<double>(bar) * 60.0 / seconds;
    if (bpmBar > kBarBpmHigh && (bar % 2) == 0)
    {
        beatsPerCycle = bar / 2;
    }
    else
    {
        while (bpmBar < kBarBpmLow && cycles < 8)
        {
            cycles *= 2;
            bpmBar *= 2.0;
        }
    }
    track.setCycleCount(cycles);
    track.setBeatsPerCycle(beatsPerCycle);
}

void LooperEngine::snapLiveLoopSeamToTransient(Track& track) noexcept
{
    const auto n = track.lengthSamples();
    const double sr = transport_.sampleRate();
    if (n < 64 || sr < 8000.0)
    {
        return;
    }
    const auto window = std::min(n / 4, static_cast<std::size_t>(std::llround(sr * 0.040)));
    if (window < 8)
    {
        return;
    }

    const int ch = track.channels();
    const auto midAbs = [&track, ch](const std::size_t i) noexcept
    {
        const float L = track.readSample(0, i);
        const float R = ch > 1 ? track.readSample(1, i) : L;
        return std::fabs(0.5f * (L + R));
    };
    const auto wrap = [n](std::int64_t i) noexcept -> std::size_t
    {
        const auto li = static_cast<std::int64_t>(n);
        auto r = i % li;
        if (r < 0)
        {
            r += li;
        }
        return static_cast<std::size_t>(r);
    };

    const auto w = static_cast<std::int64_t>(window);
    double sum = 0.0;
    double bestFlux = -1.0;
    std::int64_t bestOff = 0;
    int count = 0;
    float prev = midAbs(wrap(-w - 1));
    for (std::int64_t off = -w; off <= w; ++off)
    {
        const float a = midAbs(wrap(off));
        const double flux = std::max(0.0, static_cast<double>(a) - static_cast<double>(prev));
        prev = a;
        sum += flux;
        ++count;
        if (flux > bestFlux)
        {
            bestFlux = flux;
            bestOff = off;
        }
    }
    const double mean = sum / static_cast<double>(std::max(1, count));
    if (bestOff == 0 || bestFlux < 0.02 || bestFlux < 3.0 * (mean + 1.0e-12))
    {
        return;
    }
    track.rotateLeft(wrap(bestOff));
}

double LooperEngine::intrinsicTempoBpmForLoopLength(
    const std::size_t lengthSamples, const bool useTransportHintForQuarterCount) const noexcept
{
    if (lengthSamples == 0)
    {
        return 0.0;
    }
    const double sr = transport_.sampleRate();
    if (sr <= 1.0)
    {
        return 0.0;
    }
    const int nQuarters = chooseQuarterNotesInLoopForTempo(lengthSamples, useTransportHintForQuarterCount);
    return static_cast<double>(nQuarters) * 60.0 * sr / static_cast<double>(lengthSamples);
}

void LooperEngine::assignReferenceBpmForLoop(Track& track) noexcept
{
    constexpr double kMinPlausible = 15.0;
    constexpr double kMaxPlausible = 640.0;
    // Tempo-master take: lock stretch to the committed session BPM. A second cycle-math
    // estimate can disagree enough to varispeed (and pitch-shift) the first loop immediately.
    if (tempoMasterTrack_ >= 0 && tempoMasterTrack_ < kMaxTracks
        && &tracks_[static_cast<std::size_t>(tempoMasterTrack_)] == &track)
    {
        const double master = transport_.tempoBpm();
        if (master > 1.0 && std::isfinite(master))
        {
            track.setReferenceBpm(master);
            return;
        }
    }
    const double fromCycle = tempoBpmFromCycleFrames(track.cycleLengthSamples(), track.beatsPerCycle());
    if (fromCycle >= kMinPlausible && fromCycle <= kMaxPlausible)
    {
        track.setReferenceBpm(fromCycle);
        return;
    }
    const auto len = track.lengthSamples();
    const double intrinsic = intrinsicTempoBpmForLoopLength(len);
    if (intrinsic >= kMinPlausible && intrinsic <= kMaxPlausible)
    {
        track.setReferenceBpm(intrinsic);
    }
    else
    {
        track.setReferenceBpm(transport_.tempoBpm());
    }
}

void LooperEngine::alignTempoToTrackLoop(const int trackIndex) noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return;
    }
    const auto& track = tracks_[static_cast<std::size_t>(trackIndex)];
    const double bpm = tempoBpmFromCycleFrames(track.cycleLengthSamples(), track.beatsPerCycle());
    if (bpm <= 0.0)
    {
        return;
    }
    if (tempoManager_.isLocked() || !shouldSetTempoFromThisTrack(trackIndex))
    {
        return;
    }
    setDetectedTempo(std::clamp(bpm, 40.0, 240.0), false);
}

void LooperEngine::proposeTransportTempoFromDetection(const double detectedBpm, const double confidence) noexcept
{
    if (tempoManager_.isLocked())
    {
        return;
    }
    if (!std::isfinite(detectedBpm))
    {
        return;
    }
    constexpr double kMinBpm = kTempoDetectionBpmMin;
    constexpr double kMaxBpm = kTempoDetectionBpmMax;
    constexpr double kMinConfidence = 0.40;
    constexpr double kNearSemitoneRel = 0.018; // ~1.8% window to avoid churn.
    constexpr int kVotesNeededAfterFirst = 2;

    const double cand = std::clamp(detectedBpm, kMinBpm, kMaxBpm);
    if (confidence < kMinConfidence)
    {
        pendingDetectedTempoVotes_ = 0;
        pendingDetectedTempoBpm_ = 0.0;
        return;
    }

    if (pendingDetectedTempoVotes_ <= 0)
    {
        pendingDetectedTempoBpm_ = cand;
        pendingDetectedTempoVotes_ = 1;
    }
    else
    {
        const double relToPending =
            std::abs(cand - pendingDetectedTempoBpm_) / std::max(1.0, pendingDetectedTempoBpm_);
        if (relToPending <= kNearSemitoneRel)
        {
            pendingDetectedTempoBpm_ = 0.5 * (pendingDetectedTempoBpm_ + cand);
            ++pendingDetectedTempoVotes_;
        }
        else
        {
            pendingDetectedTempoBpm_ = cand;
            pendingDetectedTempoVotes_ = 1;
            return;
        }
    }

    // First high-confidence detection after a fresh session may adopt immediately.
    // Later proposals need agreeing votes so weak re-estimates cannot thrash transport.
    const bool firstAdoption = lastAcceptedDetectedTempoBpm_ <= 1.0;
    const int votesNeeded = firstAdoption ? 1 : kVotesNeededAfterFirst;
    if (pendingDetectedTempoVotes_ < votesNeeded)
    {
        return;
    }

    const double current = transport_.tempoBpm();
    const double relToCurrent = std::abs(pendingDetectedTempoBpm_ - current) / std::max(1.0, current);
    const double relToLastAccepted = (lastAcceptedDetectedTempoBpm_ > 1.0)
        ? std::abs(pendingDetectedTempoBpm_ - lastAcceptedDetectedTempoBpm_)
            / std::max(1.0, lastAcceptedDetectedTempoBpm_)
        : 0.0;

    // Hysteresis: require a meaningful delta before changing transport BPM (first adoption always qualifies).
    if (firstAdoption || relToCurrent >= 0.010 || relToLastAccepted >= 0.010)
    {
        setDetectedTempo(std::clamp(pendingDetectedTempoBpm_, kMinBpm, kMaxBpm), anyLoopHasContent());
        lastAcceptedDetectedTempoBpm_ = transport_.tempoBpm();
        lockSessionTempoFromLoop();
    }
    pendingDetectedTempoVotes_ = 0;
    pendingDetectedTempoBpm_ = 0.0;
}

void LooperEngine::applyTempoDetectionForSoleLoop(const int trackIndex, const bool assignReferenceBpm) noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return;
    }
    auto& track = tracks_[static_cast<std::size_t>(trackIndex)];
    if (tempoManager_.isLocked() || !shouldSetTempoFromThisTrack(trackIndex))
    {
        if (assignReferenceBpm)
            assignReferenceBpmForLoop(track);
        return;
    }
    const auto len = track.lengthSamples();
    const double sr = transport_.sampleRate();
    if (len == 0 || sr < 8000.0)
    {
        alignTempoToTrackLoop(trackIndex);
        lockSessionTempoFromLoop();
        if (assignReferenceBpm)
        {
            assignReferenceBpmForLoop(track);
        }
        return;
    }
    // First apply deterministic length-derived tempo (1 bar of notated counts at division 1).
    alignTempoToTrackLoop(trackIndex);
    const double lengthBpm = transport_.tempoBpm();

    // Onset estimate may only choose among 1-bar / 2-bar / 4-bar octaves of that length tempo.
    const std::size_t maxN = static_cast<std::size_t>(std::min(sr * 30.0, static_cast<double>(len)));
    if (maxN >= 4096)
    {
        std::vector<float> mono(maxN);
        const int ch = track.channels();
        for (std::size_t i = 0; i < maxN; ++i)
        {
            const float L = track.readSample(0, i);
            const float R = track.readSample(ch > 1 ? 1 : 0, i);
            mono[i] = 0.5f * (L + R);
        }
        const auto est = estimateTempoFromMonoAudio(
            mono.data(), maxN, sr, kTempoDetectionBpmMin, kTempoDetectionBpmMax, timeSigNumerator_);
        if (est.confidence >= 0.40 && est.bpm > 1.0 && lengthBpm > 1.0)
        {
            const double family[3] = {lengthBpm * 0.5, lengthBpm, lengthBpm * 2.0};
            double chosen = lengthBpm;
            double bestRel = std::abs(est.bpm - lengthBpm) / lengthBpm;
            for (const double cand : family)
            {
                if (cand < kTempoDetectionBpmMin * 0.98 || cand > kTempoDetectionBpmMax * 1.02)
                {
                    continue;
                }
                const double rel = std::abs(est.bpm - cand) / std::max(1.0, cand);
                // Prefer the 1-bar (division-1) length tempo unless another octave is clearly closer.
                const double bias = (std::abs(cand - lengthBpm) < 1.0e-6) ? 0.0 : 0.035;
                if (rel + bias < bestRel)
                {
                    bestRel = rel;
                    chosen = cand;
                }
            }
            proposeTransportTempoFromDetection(chosen, est.confidence);
        }
    }
    lockSessionTempoFromLoop();
    if (assignReferenceBpm)
    {
        assignReferenceBpmForLoop(track);
    }
}

void LooperEngine::finalizeOrDeferRecordingStop(const int trackIndex,
    const std::int64_t scheduledExecuteAtSample,
    const std::int64_t commandContextTransport) noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return;
    }
    const auto ti = static_cast<std::size_t>(trackIndex);
    if (scheduledExecuteAtSample >= 0 && scheduledExecuteAtSample > commandContextTransport)
    {
        pendingRecordEndExclusive_[ti] = scheduledExecuteAtSample;
        return;
    }
    pendingRecordEndExclusive_[ti] = -1;
    finishRecording(trackIndex, scheduledExecuteAtSample);
}

void LooperEngine::finalizeRecordingToPlaying(const int trackIndex,
    const std::int64_t scheduledExecuteAtSample,
    const std::int64_t commandContextTransport) noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return;
    }
    finishRecordingToPlaying_[static_cast<std::size_t>(trackIndex)] = true;
    finalizeOrDeferRecordingStop(trackIndex, scheduledExecuteAtSample, commandContextTransport);
    auto& track = tracks_[static_cast<std::size_t>(trackIndex)];
    if (track.lengthSamples() > 0 && track.state() != TrackState::Recording)
    {
        track.setState(TrackState::Playing);
        trackRecordInputGateTransport_[static_cast<std::size_t>(trackIndex)] = -1;
    }
}

void LooperEngine::finishRecording(int trackIndex, const std::int64_t transportBoundaryExclusive) noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return;
    }
    auto& track = tracks_[static_cast<std::size_t>(trackIndex)];
    if (track.state() != TrackState::Recording)
    {
        refreshIsRecordingFlag();
        return;
    }
    const auto ti = static_cast<std::size_t>(trackIndex);
    const bool forcePlaying = finishRecordingToPlaying_[ti];
    finishRecordingToPlaying_[ti] = false;
    // Gate is the transport time of buffer sample 0 after latency rotation below.
    const std::int64_t recordGate = trackRecordInputGateTransport_[ti];
    trackRecordInputGateTransport_[ti] = -1;
    track.setLengthSamples(track.recordedSamples());
    {
        const int comp = effectiveRecordLatencyCompensationSamples(trackIndex);
        const auto n = track.lengthSamples();
        if (comp > 0 && n > static_cast<std::size_t>(comp))
        {
            track.rotateLeft(static_cast<std::size_t>(comp));
        }
    }
    if (track.lengthSamples() > 0)
    {
        claimTempoMasterIfVacant(trackIndex, false);
        if (tempoMasterIsImport_ && !tempoManager_.isLocked())
        {
            tempoMasterTrack_ = trackIndex;
            tempoMasterIsImport_ = false;
        }
        if (shouldApplyLengthQuantize(trackIndex))
        {
            applyLengthQuantizeToTrack(track);
        }
        if (!tempoManager_.isLocked() && shouldSetTempoFromThisTrack(trackIndex))
        {
            // Duration between Rec and Stop, assumed 2/4/8 beats (time-sig family), then a short
            // seam snap to the nearest transient. Full-buffer onset BPM is imports-only.
            snapLiveLoopSeamToTransient(track);
            const int wantBars = pendingLoopBars_[ti];
            applyLiveTakeDurationTempo(track, wantBars);
            alignTempoToTrackLoop(trackIndex);
            lockSessionTempoFromLoop();
            pendingLoopBars_[ti] = 0;
        }
        else
        {
            const int master = trackSyncMasterIndex();
            if (master >= 0 && master != trackIndex)
            {
                const auto masterCycle = tracks_[static_cast<std::size_t>(master)].cycleLengthSamples();
                const auto len = track.lengthSamples();
                if (masterCycle > 0 && (len % masterCycle) == 0)
                {
                    track.setCycleCount(std::max(1, static_cast<int>(len / masterCycle)));
                }
                else
                {
                    track.setCycleCount(1);
                }
            }
            else
            {
                track.setCycleCount(1);
            }
            const int wantBars = pendingLoopBars_[ti];
            if (wantBars > 0)
            {
                pendingLoopBars_[ti] = 0;
                (void) rescaleLoopToBarCount(trackIndex, wantBars);
            }
        }
        // Live audio is already rendered at the device rate. Do not infer a
        // follower's tempo from its duration: a longer/shorter take is not a
        // differently sampled file. Preserve the tempo captured at record start.
        // The first take alone establishes a new master tempo at completion.
        if (shouldSetTempoFromThisTrack(trackIndex) || track.referenceBpm() <= 1.0)
            track.setReferenceBpm(transport_.tempoBpm());
        resetStretchForTrack(trackIndex);
        const bool peersAlreadyPlaying = hasOtherLoopWithActivePlayback(trackIndex);
        const bool trackAfterRecord = track.loopPrefs().overrideGlobal;
        const auto afterMode = trackAfterRecord
            ? static_cast<AfterRecordMode>(track.loopPrefs().afterRecordMode)
            : afterRecordMode_;
        const bool enterOverdub = !forcePlaying && afterMode == AfterRecordMode::Dub
            && (trackAfterRecord || dubModeEnabled_ || !peersAlreadyPlaying);
        if (enterOverdub)
        {
            track.setState(TrackState::Overdub);
            captureStartSerial_.fetch_add(1, std::memory_order_relaxed);
            unmuteForCapture(trackIndex);
            if (transportBoundaryExclusive >= 0)
            {
                trackRecordInputGateTransport_[ti] = transportBoundaryExclusive;
            }
            else
            {
                trackRecordInputGateTransport_[ti] = transport_.samplePosition();
            }
        }
        else
        {
            track.setState(TrackState::Playing);
        }
    }
    else
    {
        track.setState(TrackState::Stopped);
    }
    if (track.lengthSamples() > 0)
    {
        const std::int64_t phaseAt =
            transportBoundaryExclusive >= 0 ? transportBoundaryExclusive : transport_.samplePosition();
        if (trackStateAdvancesPlayhead(track.state()))
            serialFadeInEndSample_[ti] = phaseAt + static_cast<std::int64_t>(serialCrossfadeSamples_);
        bool otherLoopHasContent = false;
        for (int i = 0; i < kMaxTracks; ++i)
        {
            if (i == trackIndex)
            {
                continue;
            }
            if (tracks_[static_cast<std::size_t>(i)].lengthSamples() > 0)
            {
                otherLoopHasContent = true;
                break;
            }
        }
        const auto lenI = static_cast<std::int64_t>(track.lengthSamples());
        const bool clockOwnsGrid = clockGridIsActive();
        if (clockOwnsGrid || (otherLoopHasContent && playPhaseLockEnabled_))
        {
            // Stay on the clock that is already running. The typed-tempo click owns
            // that origin; a finished take must not move it. Phase lock does the same
            // when other loops are already playing. A private record-start anchor
            // leaves this take shifted by however late Rec began.
            if (clockOwnsGrid)
            {
                pendingTempoPhaseAdjust_.store(false, std::memory_order_release);
            }
            const int peer = longestPlayingLoopIndex(trackIndex);
            const std::int64_t lockedOrigin = peer >= 0 ? playbackPhaseOrigin(peer) : sharedLoopPhaseOrigin_;
            const std::int64_t musicalStart = recordGate >= 0 ? recordGate : (phaseAt - lenI);
            const auto lenNow = track.lengthSamples();
            if (lenNow > 1)
            {
                const auto len64 = static_cast<std::int64_t>(lenNow);
                auto rot = (lockedOrigin - musicalStart) % len64;
                if (rot < 0)
                {
                    rot += len64;
                }
                if (rot != 0)
                {
                    track.rotateLeft(static_cast<std::size_t>(rot));
                }
            }
            lockTrackPhaseToPeer(trackIndex);
            const std::int64_t origin = playbackPhaseOrigin(trackIndex);
            trackPlayheads_[ti] = loopPositionFromTransport(phaseAt, origin, track.lengthSamples());
            stretchPhaseOrigin_[ti] = origin;
            stretchSourceUnwrapped_[ti] = static_cast<double>(trackPlayheads_[ti]);
        }
        else if (!otherLoopHasContent)
        {
            // This take owns the session clock. Rec-stop is the seam: next sample is loop 0.
            // Cancel any first-take setTempo rebase so playback does not start mid-cycle.
            pendingTempoPhaseAdjust_.store(false, std::memory_order_release);
            sharedLoopPhaseOrigin_ = phaseAt - lenI;
            clearRecordPhaseAnchor(trackIndex);
            trackPlayheads_[ti] = 0;
            stretchPhaseOrigin_[ti] = sharedLoopPhaseOrigin_;
            stretchSourceUnwrapped_[ti] = 0.0;
        }
        else
        {
            // Other loops keep the shared clock. This buffer starts at sample 0, so its
            // own anchor is the record start. Using the shared clock here opens the new
            // loop in the middle whenever the take did not begin on a shared downbeat.
            trackRecordPhaseAnchor_[ti] = true;
            trackRecordPhaseOrigin_[ti] = phaseAt - lenI;
            trackPlayheads_[ti] = 0;
            stretchPhaseOrigin_[ti] = trackRecordPhaseOrigin_[ti];
            stretchSourceUnwrapped_[ti] = 0.0;
        }
    }
    else
    {
        trackPlayheads_[ti] = 0;
    }
    refreshIsRecordingFlag();
    if (waveformEnvelopes_ != nullptr)
    {
        waveformEnvelopes_->rebuildFromTrack(trackIndex, track);
    }
}

void LooperEngine::enforceSingleTrackCapture(const int exceptTrackIndex, const bool stealLiveCapture) noexcept
{
    if (multiTrackRecord_ || exceptTrackIndex < 0 || exceptTrackIndex >= kMaxTracks)
    {
        return;
    }
    for (int i = 0; i < kMaxTracks; ++i)
    {
        if (i == exceptTrackIndex)
        {
            continue;
        }
        auto& tr = tracks_[static_cast<std::size_t>(i)];
        const auto st = tr.state();
        if (st == TrackState::Armed)
        {
            cancelScheduledRecordToggleForTrack(i);
            cancelScheduledCommandsForTrack(i, CommandType::Play);
            cancelScheduledCommandsForTrack(i, CommandType::PlayStopToggle);
            tr.setState(tr.lengthSamples() > 0 ? TrackState::Stopped : TrackState::Empty);
            continue;
        }
        if (st == TrackState::Playing_DubPending)
        {
            cancelScheduledRecordToggleForTrack(i);
            cancelScheduledCommandsForTrack(i, CommandType::OverdubToggle);
            tr.setState(TrackState::Playing);
            continue;
        }
        if (!stealLiveCapture)
        {
            continue;
        }
        if (st == TrackState::Recording)
        {
            finalizeRecordingToPlaying(i, -1, -1);
            continue;
        }
        if (st == TrackState::Overdub || st == TrackState::Overdubbing_MutePending)
        {
            cancelScheduledCommandsForTrack(i, CommandType::OverdubToggle);
            cancelScheduledRecordToggleForTrack(i);
            tr.setState(st == TrackState::Overdubbing_MutePending ? TrackState::Playing_MutePending : TrackState::Playing);
            trackRecordInputGateTransport_[static_cast<std::size_t>(i)] = -1;
        }
    }
    refreshIsRecordingFlag();
}

void LooperEngine::stopPlaybackPeersForRecording(const int trackIndex, const std::int64_t originSample) noexcept
{
    if (!syncModeUsesSerialHandoff()) return;
    for (int i = 0; i < kMaxTracks; ++i)
    {
        if (i == trackIndex || (syncMode_ == SyncMode::SerialParallel && i == 0)) continue;
        const auto state = tracks_[static_cast<std::size_t>(i)].state();
        // Keep explicitly enabled simultaneous capture intact; this rule stops
        // playback peers, not another ongoing recording/overdub.
        if (!trackStateAdvancesPlayhead(state) || trackStateIsOverdub(state)) continue;
        cancelScheduledCommandsForTrack(i, CommandType::Play);
        cancelScheduledCommandsForTrack(i, CommandType::PlayStopToggle);
        cancelScheduledCommandsForTrack(i, CommandType::SerialHandoff);
        cancelScheduledCommandsForTrack(i, CommandType::OverdubToggle);
        cancelScheduledRecordToggleForTrack(i);
        cancelScheduledCommandsForTrack(i, CommandType::MuteToggle);
        enterStoppedWithSpillover(i, originSample);
    }
}

void LooperEngine::beginFirstPassRecording(const int trackIndex, const std::int64_t originSample) noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return;
    }
    enforceSingleTrackCapture(trackIndex, true);
    stopPlaybackPeersForRecording(trackIndex, originSample);
    auto& track = tracks_[static_cast<std::size_t>(trackIndex)];
    // Realtime-safe: keep reserved capacity. clearAudio() takes a mutex and can hitch the first hit.
    track.clearLiveAudioKeepHistory();
    track.setReferenceBpm(transport_.tempoBpm());
    recordLimitReached_.store(false, std::memory_order_relaxed);
    trackWriteheads_[static_cast<std::size_t>(trackIndex)] = 0;
    clearRecordPhaseAnchor(trackIndex);
    resetStretchForTrack(trackIndex);
    if (playPhaseLockEnabled_ && longestPlayingLoopIndex(trackIndex) >= 0)
    {
        lockTrackPhaseToPeer(trackIndex);
    }
    const auto tiRec = static_cast<std::size_t>(trackIndex);
    pendingRecordEndExclusive_[tiRec] = -1;
    finishRecordingToPlaying_[tiRec] = false;
    const std::int64_t anchor = originSample;
    adoptSharedPhaseOriginIfNoActivePeers(trackIndex, anchor);
    trackRecordInputGateTransport_[tiRec] = originSample;
    track.setState(TrackState::Recording);
    captureStartSerial_.fetch_add(1, std::memory_order_relaxed);
    isRecording_.store(true, std::memory_order_release);
    if (tempoManager_.state() == TempoSyncState::Empty)
    {
        liveTransientDetector_.reset();
    }
    unmuteForCapture(trackIndex);
}

void LooperEngine::enterOverdubFromPlay(const int trackIndex, const std::int64_t originSample) noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return;
    }
    auto& track = tracks_[static_cast<std::size_t>(trackIndex)];
    if (track.lengthSamples() <= 0)
    {
        return;
    }
    enforceSingleTrackCapture(trackIndex, true);
    rememberUndoSessionBpm();
    track.captureOverdubUndoSnapshot();
    adoptSharedPhaseOriginIfNoActivePeers(trackIndex, originSample);
    track.setState(TrackState::Overdub);
    captureStartSerial_.fetch_add(1, std::memory_order_relaxed);
    trackRecordInputGateTransport_[static_cast<std::size_t>(trackIndex)] = originSample;
    unmuteForCapture(trackIndex);
}

void LooperEngine::rememberUndoSessionBpm() noexcept
{
    undoSessionBpm_ = static_cast<double>(tempoManager_.masterBpm());
    undoSessionBpmValid_ = undoSessionBpm_ > 1.0;
}

void LooperEngine::restoreSessionTempoFromUndo() noexcept
{
    if (!undoSessionBpmValid_ || !(undoSessionBpm_ > 1.0))
    {
        return;
    }
    const double previous = transport_.tempoBpm();
    tempoManager_.restoreLockedTempo(static_cast<float>(undoSessionBpm_));
    transport_.setTempo(static_cast<double>(tempoManager_.masterBpm()));
    queueTempoPhasePreserveFrom(previous);
    if (tempoMasterTrack_ < 0 || tempoMasterTrack_ >= kMaxTracks
        || tracks_[static_cast<std::size_t>(tempoMasterTrack_)].lengthSamples() < 2)
    {
        for (int i = 0; i < kMaxTracks; ++i)
        {
            if (tracks_[static_cast<std::size_t>(i)].lengthSamples() >= 2)
            {
                tempoMasterTrack_ = i;
                break;
            }
        }
    }
}

float LooperEngine::lastInputPeak() const noexcept
{
    return lastInputPeak_.load(std::memory_order_relaxed);
}

float LooperEngine::lastOutputPeak() const noexcept
{
    return lastOutputPeak_.load(std::memory_order_relaxed);
}

float LooperEngine::trackOutputMeter(const int trackIndex) const noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return 0.0f;
    }
    const auto snap = trackMeterSnapshot(trackIndex);
    const auto st = tracks_[static_cast<std::size_t>(trackIndex)].state();
    if (st == TrackState::Recording || st == TrackState::Armed || st == TrackState::Empty)
    {
        return std::max(snap.inPeakL, snap.inPeakR);
    }
    return std::max(snap.outPeakL, snap.outPeakR);
}

metering::TrackMeterRtSnapshot LooperEngine::trackMeterSnapshot(const int trackIndex) const noexcept
{
    metering::TrackMeterRtSnapshot s{};
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return s;
    }
    const auto i = static_cast<std::size_t>(trackIndex);
    s.outPeakL = trackOutPeakL_[i].load(std::memory_order_relaxed);
    s.outPeakR = trackOutPeakR_[i].load(std::memory_order_relaxed);
    s.inPeakL = trackInPeakL_[i].load(std::memory_order_relaxed);
    s.inPeakR = trackInPeakR_[i].load(std::memory_order_relaxed);
    s.outRmsL = trackOutRmsL_[i].load(std::memory_order_relaxed);
    s.outRmsR = trackOutRmsR_[i].load(std::memory_order_relaxed);
    s.inRmsL = trackInRmsL_[i].load(std::memory_order_relaxed);
    s.inRmsR = trackInRmsR_[i].load(std::memory_order_relaxed);
    if (tracks_[i].channels() < 2)
    {
        s.inPeakR = s.inPeakL;
        s.inRmsR = s.inRmsL;
    }
    return s;
}

void LooperEngine::overlayTrackInputPeaks(const int trackIndex, const float peakL, const float peakR) noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return;
    }
    const auto i = static_cast<std::size_t>(trackIndex);
    auto bump = [](std::atomic<float>& slot, const float v) noexcept
    {
        float prev = slot.load(std::memory_order_relaxed);
        while (v > prev && !slot.compare_exchange_weak(prev, v, std::memory_order_relaxed, std::memory_order_relaxed))
        {
        }
    };
    bump(trackInPeakL_[i], peakL);
    bump(trackInPeakR_[i], peakR);
}

void LooperEngine::copyWaveformEnvelope(const int trackIndex, float* dest, const int n) const noexcept
{
    if (waveformEnvelopes_ != nullptr)
    {
        waveformEnvelopes_->copyPublished(trackIndex, dest, n);
    }
}

void LooperEngine::copyWaveformEnvelopeMinMax(const int trackIndex, float* mins, float* maxs, const int n) const noexcept
{
    if (waveformEnvelopes_ != nullptr)
    {
        waveformEnvelopes_->copyPublishedMinMax(trackIndex, mins, maxs, n);
    }
}

std::uint32_t LooperEngine::waveformEnvelopeEpoch(const int trackIndex) const noexcept
{
    return waveformEnvelopes_ != nullptr ? waveformEnvelopes_->epoch(trackIndex) : 0;
}

void LooperEngine::publishWaveformEnvelopes() noexcept
{
    if (waveformEnvelopes_ == nullptr)
    {
        return;
    }
    for (int t = 0; t < kMaxTracks; ++t)
    {
        const auto& tr = tracks_[static_cast<std::size_t>(t)];
        if (!waveformEnvelopes_->trackNeedsLivePublish(t, tr))
        {
            continue;
        }
        const auto st = tr.state();
        std::size_t hint = 0;
        if (st == TrackState::Recording)
        {
            int bars = pendingLoopBars_[static_cast<std::size_t>(t)];
            if (bars <= 0)
            {
                bars = std::max(1, globalBarLengthBars_);
            }
            hint = samplesPerNotatedBar() * static_cast<std::size_t>(std::max(1, bars));
        }
        waveformEnvelopes_->rebuildFromTrack(t, tr, hint);
    }
}

bool LooperEngine::popCommand(EngineCommand& out) noexcept
{
    return commandQueue_.pop(out);
}

bool LooperEngine::pushScheduledCommand(const EngineCommand& command, std::int64_t executeAtSample) noexcept
{
    for (auto& slot : scheduledCommands_)
    {
        if (!slot.active)
        {
            slot.command = command;
            slot.executeAtSample = executeAtSample;
            slot.playModeContext = (command.type == CommandType::Play || command.type == CommandType::PlayStopToggle)
                ? (dubModeEnabled_ ? 2 : muteModeEnabled_ ? 1 : 0) : -1;
            slot.active = true;
            if (command.trackIndex >= 0 && command.trackIndex < kMaxTracks)
            {
                lastTransitionError_[static_cast<std::size_t>(command.trackIndex)] = TransitionError::None;
            }
            return true;
        }
    }
    return false;
}

void LooperEngine::cancelScheduledRecordToggleForTrack(const int trackIndex) noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return;
    }
    for (auto& slot : scheduledCommands_)
    {
        if (!slot.active)
        {
            continue;
        }
        if (slot.command.type != CommandType::RecordToggle || slot.command.trackIndex != trackIndex)
        {
            continue;
        }
        slot.active = false;
    }
}

void LooperEngine::cancelScheduledCommandsForTrack(const int trackIndex, const CommandType type) noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return;
    }
    for (auto& slot : scheduledCommands_)
    {
        if (!slot.active)
        {
            continue;
        }
        if (slot.command.trackIndex != trackIndex || slot.command.type != type)
        {
            continue;
        }
        slot.active = false;
    }
}

void LooperEngine::cancelAllScheduledCommandsForTrack(const int trackIndex) noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return;
    }
    for (auto& slot : scheduledCommands_)
    {
        if (!slot.active || slot.command.trackIndex != trackIndex)
        {
            continue;
        }
        slot.active = false;
    }
}

std::int64_t LooperEngine::countInLeadSamples(int trackIndex) const noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return 0;
    }
    const double spb = transport_.samplesPerBeat();
    switch (countInStyle_)
    {
        case CountInStyle::Off:
            return 0;
        case CountInStyle::Beat:
        case CountInStyle::Bar:
        {
            const int nb = std::max(1, countInBars_);
            return static_cast<std::int64_t>(spb * quartersPerNotatedBar() * static_cast<double>(nb));
        }
        case CountInStyle::Loop:
        {
            std::size_t maxLen = 0;
            for (const auto& t : tracks_)
            {
                maxLen = std::max(maxLen, t.lengthSamples());
            }
            if (maxLen == 0)
            {
                return static_cast<std::int64_t>(spb * quartersPerNotatedBar());
            }
            return static_cast<std::int64_t>(maxLen);
        }
        default:
            return 0;
    }
}

std::int64_t LooperEngine::alignCountInExecuteSample(const std::int64_t pos) const noexcept
{
    switch (countInStyle_)
    {
        case CountInStyle::Off:
            return pos;
        case CountInStyle::Beat:
        case CountInStyle::Bar: {
            const auto bar = static_cast<std::int64_t>(samplesPerNotatedBar());
            auto t = alignToPeriodBoundaryWithOrigin(pos, bar, sharedLoopPhaseOrigin_);
            if (t <= pos)
            {
                t = pos + bar;
            }
            const int extra = std::max(1, countInBars_) - 1;
            if (extra > 0)
            {
                t += bar * static_cast<std::int64_t>(extra);
            }
            return t;
        }
        case CountInStyle::Loop:
            return alignToLongestPlayingLoopWrap(pos, pos, true);
        default:
            return pos;
    }
}

std::int64_t LooperEngine::samplesPerBeatInt() const noexcept
{
    return std::max(static_cast<std::int64_t>(1), static_cast<std::int64_t>(std::llround(transport_.samplesPerBeat())));
}

std::int64_t LooperEngine::samplesPerNotatedBeatInt() const noexcept
{
    const int den = std::max(1, timeSigDenominator_);
    const double samples = transport_.samplesPerBeat() * (4.0 / static_cast<double>(den));
    return std::max(static_cast<std::int64_t>(1), static_cast<std::int64_t>(std::llround(samples)));
}

std::int64_t LooperEngine::samplesPerGlobalBar() const noexcept
{
    const double qpb = quartersPerNotatedBar();
    const auto spb = samplesPerBeatInt();
    const auto perMeasure =
        std::max(static_cast<std::int64_t>(1), static_cast<std::int64_t>(std::llround(static_cast<double>(spb) * qpb)));
    return perMeasure * static_cast<std::int64_t>(std::max(1, globalBarLengthBars_));
}

GlobalStopMode LooperEngine::effectiveStopQuantizeMode(const Track& track) const noexcept
{
    if (track.loopPrefs().overrideGlobal)
    {
        return withoutBeatTiming(static_cast<GlobalStopMode>(track.loopPrefs().stopMode));
    }
    if (countOutStyle_ != CountOutStyle::Off)
    {
        return withoutBeatTiming(static_cast<GlobalStopMode>(static_cast<uint8_t>(countOutStyle_)));
    }
    return withoutBeatTiming(globalStopMode_);
}

std::int64_t LooperEngine::playbackPhaseOrigin(const int trackIndex) const noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return sharedLoopPhaseOrigin_;
    }
    const auto ti = static_cast<std::size_t>(trackIndex);
    if (trackPhaseFromStart_[ti])
    {
        return trackPhaseOrigin_[ti];
    }
    if (trackRecordPhaseAnchor_[ti])
    {
        return trackRecordPhaseOrigin_[ti];
    }
    return sharedLoopPhaseOrigin_;
}

std::int64_t LooperEngine::audiblePhaseOrigin() const noexcept
{
    const int master = longestPlayingLoopIndex(-1);
    if (master >= 0)
    {
        return playbackPhaseOrigin(master);
    }
    return sharedLoopPhaseOrigin_;
}

void LooperEngine::clearRecordPhaseAnchor(const int trackIndex) noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return;
    }
    const auto ti = static_cast<std::size_t>(trackIndex);
    trackRecordPhaseAnchor_[ti] = false;
    trackRecordPhaseOrigin_[ti] = sharedLoopPhaseOrigin_;
}

void LooperEngine::clearPlayStartPhase(const int trackIndex) noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return;
    }
    const auto ti = static_cast<std::size_t>(trackIndex);
    trackPhaseFromStart_[ti] = false;
    trackPhaseOrigin_[ti] = sharedLoopPhaseOrigin_;
}

void LooperEngine::preparePlayStartPhase(const int trackIndex, const std::int64_t executeSample) noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return;
    }
    const auto ti = static_cast<std::size_t>(trackIndex);
    const auto& tr = tracks_[ti];
    const auto mode = effectivePlayMode(tr);
    // Bar (and only Bar) opens at sample 0, and only on the click downbeat the cue waited for.
    // Serial used to do that at the other loop's wrap, which is not the metronome.
    const bool barFromStart = !playPhaseLockEnabled_ && mode == PlaybackStartMode::Bar;
    const bool startFromBeginning = barFromStart && tr.lengthSamples() > 0;
    if (startFromBeginning)
    {
        trackPhaseFromStart_[ti] = true;
        trackPhaseOrigin_[ti] = executeSample;
        resetStretchForTrack(trackIndex);
        trackPlayheads_[ti] = 0;
        return;
    }
    clearPlayStartPhase(trackIndex);
    if (metronomeForceRunning_)
    {
        // The click already has the grid. Drop the private record anchor so this
        // loop reads the same origin the beat clock is counting.
        clearRecordPhaseAnchor(trackIndex);
        const auto len = tr.lengthSamples();
        if (len > 0)
        {
            trackPlayheads_[ti] = loopPositionFromTransport(executeSample, sharedLoopPhaseOrigin_, len);
            stretchPhaseOrigin_[ti] = sharedLoopPhaseOrigin_;
            stretchSourceUnwrapped_[ti] = static_cast<double>(trackPlayheads_[ti]);
        }
    }
}

PlaybackStartMode LooperEngine::effectivePlayMode(const Track& track) const noexcept
{
    if (track.loopPrefs().overrideGlobal)
    {
        return withoutBeatTiming(static_cast<PlaybackStartMode>(track.loopPrefs().playMode));
    }
    return withoutBeatTiming(playbackStartMode_);
}

PlaybackStartMode LooperEngine::effectiveRecordMode(const Track& track) const noexcept
{
    if (track.loopPrefs().overrideGlobal)
    {
        return withoutBeatTiming(static_cast<PlaybackStartMode>(track.loopPrefs().recordMode));
    }
    return withoutBeatTiming(recordStartMode_);
}

PlaybackStartMode LooperEngine::effectiveOverdubMode(const Track& track) const noexcept
{
    if (dubModeEnabled_)
    {
        return withoutBeatTiming(overdubStartMode_);
    }
    if (track.loopPrefs().overrideGlobal)
    {
        return withoutBeatTiming(static_cast<PlaybackStartMode>(track.loopPrefs().overdubMode));
    }
    return withoutBeatTiming(overdubStartMode_);
}

PlaybackStartMode LooperEngine::effectiveMuteMode(const Track& track) const noexcept
{
    if (muteModeEnabled_)
    {
        return withoutBeatTiming(muteStartMode_);
    }
    if (track.loopPrefs().overrideGlobal)
    {
        return withoutBeatTiming(static_cast<PlaybackStartMode>(track.loopPrefs().muteMode));
    }
    return withoutBeatTiming(muteStartMode_);
}

std::int64_t LooperEngine::globalPhaseLengthSamples(const int trackIndex) const noexcept
{
    const int master = trackSyncMasterIndex();
    if (master >= 0 && master < kMaxTracks)
    {
        const auto n = tracks_[static_cast<std::size_t>(master)].lengthSamples();
        if (n > 1)
        {
            return static_cast<std::int64_t>(n);
        }
    }
    auto playing = static_cast<std::int64_t>(longestPlayingLoopLengthSamples(-1));
    if (playing > 1)
    {
        return playing;
    }
    if (trackIndex >= 0 && trackIndex < kMaxTracks)
    {
        const auto n = tracks_[static_cast<std::size_t>(trackIndex)].lengthSamples();
        if (n > 1)
        {
            return static_cast<std::int64_t>(n);
        }
    }
    const auto any = static_cast<std::int64_t>(longestLoopLengthSamples());
    return any > 1 ? any : 0;
}

std::int64_t LooperEngine::globalPhaseSample(const std::int64_t transportPos, const std::int64_t phaseLength) const noexcept
{
    if (phaseLength <= 0)
    {
        return 0;
    }
    return static_cast<std::int64_t>(loopPositionFromTransport(
        transportPos, sharedLoopPhaseOrigin_, static_cast<std::size_t>(phaseLength)));
}

std::int64_t LooperEngine::targetSampleForTimingMode(
    const TimingMode mode, const int trackIndex, std::int64_t pos) const noexcept
{
    if (pos < 0)
    {
        pos = transport_.samplePosition();
    }
    switch (mode)
    {
        case TimingMode::Immediate:
            return pos;
        case TimingMode::Beat:
        case TimingMode::Bar: {
            const auto bar = static_cast<std::int64_t>(samplesPerNotatedBar());
            const auto t = alignToPeriodBoundaryWithOrigin(pos, bar, sharedLoopPhaseOrigin_);
            return std::max(pos, t);
        }
        case TimingMode::LoopBegin: {
            const auto len = static_cast<std::int64_t>(longestPlayingLoopLengthSamples(-1));
            if (len <= 1)
            {
                return pos;
            }
            const auto ph = globalPhaseSample(pos, len);
            if (ph == 0)
            {
                return pos;
            }
            return pos + (len - ph);
        }
        case TimingMode::LoopEnd: {
            std::size_t loopLen = 0;
            if (trackIndex >= 0 && trackIndex < kMaxTracks)
            {
                loopLen = tracks_[static_cast<std::size_t>(trackIndex)].lengthSamples();
            }
            if (loopLen < 2)
            {
                loopLen = static_cast<std::size_t>(std::max<std::int64_t>(0, globalPhaseLengthSamples(trackIndex)));
            }
            if (loopLen < 2)
            {
                return pos;
            }
            const auto lenI = static_cast<std::int64_t>(loopLen);
            const auto last = lenI - 1;
            const auto ph = static_cast<std::int64_t>(
                loopPositionFromTransport(pos, sharedLoopPhaseOrigin_, loopLen));
            if (ph == last)
            {
                return pos + lenI;
            }
            if (ph < last)
            {
                return pos + (last - ph);
            }
            return pos + (lenI - (ph - last));
        }
        default:
            return pos;
    }
}

void LooperEngine::armPendingStateForScheduledCommand(const EngineCommand& command) noexcept
{
    if (command.trackIndex < 0 || command.trackIndex >= kMaxTracks)
    {
        return;
    }
    auto& tr = tracks_[static_cast<std::size_t>(command.trackIndex)];
    const auto cs = tr.state();
    if (command.type == CommandType::PlayStopToggle && playStopShouldOverdub(command.trackIndex)
        && cs == TrackState::Playing)
    {
        tr.setState(TrackState::Playing_DubPending);
        enforceSingleTrackCapture(command.trackIndex, false);
        return;
    }
    if (command.type == CommandType::RecordToggle)
    {
        if (cs == TrackState::Empty || (cs == TrackState::Stopped && tr.lengthSamples() == 0))
        {
            tr.setState(TrackState::Armed);
            enforceSingleTrackCapture(command.trackIndex, false);
        }
        else if (cs == TrackState::Playing)
        {
            tr.setState(TrackState::Playing_DubPending);
            enforceSingleTrackCapture(command.trackIndex, false);
        }
        return;
    }
    if ((command.type == CommandType::Play || command.type == CommandType::PlayStopToggle)
        && muteModeEnabled_ && !dubModeEnabled_
        && (cs == TrackState::Playing || cs == TrackState::Overdub || cs == TrackState::Muted
            || trackStateIsMutePending(cs)))
    {
        if (cs == TrackState::Playing)
        {
            tr.setState(TrackState::Playing_MutePending);
        }
        else if (cs == TrackState::Overdub)
        {
            tr.setState(TrackState::Overdubbing_MutePending);
        }
        else if (cs == TrackState::Muted)
        {
            tr.setState(TrackState::Muted_UnmutePending);
        }
        return;
    }
    if ((command.type == CommandType::Play || command.type == CommandType::PlayStopToggle)
        && (cs == TrackState::Empty || (cs == TrackState::Stopped && tr.lengthSamples() == 0)))
    {
        tr.setState(TrackState::Armed);
        enforceSingleTrackCapture(command.trackIndex, false);
        return;
    }
    if (command.type == CommandType::MuteToggle)
    {
        if (cs == TrackState::Playing)
        {
            tr.setState(TrackState::Playing_MutePending);
        }
        else if (cs == TrackState::Overdub)
        {
            tr.setState(TrackState::Overdubbing_MutePending);
        }
        else if (cs == TrackState::Muted)
        {
            tr.setState(TrackState::Muted_UnmutePending);
        }
    }
}

std::int64_t LooperEngine::alignExecuteSampleForPlaybackMode(
    const int trackIndex, const std::int64_t baseExecute, const std::int64_t pos, const PlaybackStartMode mode) const noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return baseExecute;
    }
    if (playbackStartModeIsLoopCycle(mode))
    {
        const auto& tr = tracks_[static_cast<std::size_t>(trackIndex)];
        if (tr.lengthSamples() >= 2)
        {
            return alignToTrackLoopWrap(trackIndex, baseExecute, pos, false);
        }
        return std::max(baseExecute, targetSampleForTimingMode(TimingMode::Bar, trackIndex, pos));
    }
    if (mode == PlaybackStartMode::LoopEnd)
    {
        return std::max(baseExecute, targetSampleForTimingMode(TimingMode::LoopEnd, trackIndex, pos));
    }
    const auto timing = timingModeFromPlaybackStart(mode, false);
    return std::max(baseExecute, targetSampleForTimingMode(timing, trackIndex, pos));
}

std::int64_t LooperEngine::alignPlayExecuteSample(int trackIndex, std::int64_t baseExecute, std::int64_t pos) const noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return baseExecute;
    }
    const auto& tr = tracks_[static_cast<std::size_t>(trackIndex)];
    if (playbackStartModeIsLoopCycle(effectivePlayMode(tr)))
    {
        return alignLoopCueStart(trackIndex, baseExecute, pos);
    }
    return alignExecuteSampleForPlaybackMode(trackIndex, baseExecute, pos, effectivePlayMode(tr));
}

std::int64_t LooperEngine::alignRecordExecuteSample(int trackIndex, std::int64_t baseExecute, std::int64_t pos) const noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return baseExecute;
    }
    const auto& tr = tracks_[static_cast<std::size_t>(trackIndex)];
    if (playbackStartModeIsLoopCycle(effectiveRecordMode(tr)))
    {
        return alignLoopCueStart(trackIndex, baseExecute, pos);
    }
    return alignExecuteSampleForPlaybackMode(trackIndex, baseExecute, pos, effectiveRecordMode(tr));
}

std::int64_t LooperEngine::alignOverdubExecuteSample(int trackIndex, std::int64_t baseExecute, std::int64_t pos) const noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return baseExecute;
    }
    const auto& tr = tracks_[static_cast<std::size_t>(trackIndex)];
    if (playbackStartModeIsLoopCycle(effectiveOverdubMode(tr)))
    {
        return alignLoopCueStart(trackIndex, baseExecute, pos);
    }
    return alignExecuteSampleForPlaybackMode(trackIndex, baseExecute, pos, effectiveOverdubMode(tr));
}

std::int64_t LooperEngine::alignOverdubCueEnd(
    const int trackIndex, const std::int64_t baseExecute, const std::int64_t pos) const noexcept
{
    if (trackIndex >= 0 && trackIndex < kMaxTracks
        && tracks_[static_cast<std::size_t>(trackIndex)].lengthSamples() >= 2)
    {
        return alignToTrackLoopWrap(trackIndex, baseExecute, pos, true);
    }
    return alignRecordedCycleEnd(trackIndex, baseExecute, pos);
}

std::int64_t LooperEngine::alignLoopCueStart(
    const int trackIndex, const std::int64_t baseExecute, const std::int64_t pos) const noexcept
{
    const int other = longestPlayingLoopIndex(trackIndex);
    if (other >= 0)
    {
        return alignToTrackLoopWrap(other, baseExecute, pos, false);
    }
    if (trackIndex >= 0 && trackIndex < kMaxTracks
        && tracks_[static_cast<std::size_t>(trackIndex)].lengthSamples() >= 2)
    {
        return alignToTrackLoopWrap(trackIndex, baseExecute, pos, false);
    }
    return std::max(baseExecute, targetSampleForTimingMode(TimingMode::Bar, trackIndex, pos));
}

std::int64_t LooperEngine::alignRecordedCycleEnd(
    const int trackIndex, const std::int64_t baseExecute, const std::int64_t pos) const noexcept
{
    if (trackIndex >= 0 && trackIndex < kMaxTracks
        && tracks_[static_cast<std::size_t>(trackIndex)].lengthSamples() >= 2)
    {
        return alignToTrackLoopWrap(trackIndex, baseExecute, pos, true);
    }
    const int other = longestPlayingLoopIndex(trackIndex);
    if (other >= 0)
    {
        return alignToTrackLoopWrap(other, baseExecute, pos, true);
    }
    return std::max(baseExecute, targetSampleForTimingMode(TimingMode::Bar, trackIndex, pos));
}

std::int64_t LooperEngine::alignStopExecuteSample(int trackIndex, std::int64_t baseExecute, std::int64_t pos) const noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return baseExecute;
    }
    const auto& tr = tracks_[static_cast<std::size_t>(trackIndex)];
    const auto mode = effectiveStopQuantizeMode(tr);
    if (mode == GlobalStopMode::LoopEnd)
    {
        const auto st = tr.state();
        if (st == TrackState::Recording || st == TrackState::Overdub || trackStateIsOverdub(st))
        {
            return alignRecordedCycleEnd(trackIndex, baseExecute, pos);
        }
        if (tr.lengthSamples() >= 2)
        {
            return alignToTrackLoopWrap(trackIndex, baseExecute, pos, true);
        }
        return std::max(baseExecute, targetSampleForTimingMode(TimingMode::Bar, trackIndex, pos));
    }
    return alignExecuteSampleForStopMode(baseExecute, pos, mode);
}

std::int64_t LooperEngine::alignExecuteSampleForStopMode(
    const std::int64_t baseExecute, const std::int64_t pos, const GlobalStopMode mode) const noexcept
{
    switch (mode)
    {
        case GlobalStopMode::Immediate:
            return baseExecute;
        case GlobalStopMode::Beat:
        case GlobalStopMode::Bar: {
            const auto bar = static_cast<std::int64_t>(samplesPerNotatedBar());
            const auto t = alignToPeriodBoundaryWithOrigin(pos, bar, sharedLoopPhaseOrigin_);
            return std::max(baseExecute, t);
        }
        case GlobalStopMode::LoopEnd:
            return alignToLongestPlayingLoopWrap(baseExecute, pos, true);
        default:
            return baseExecute;
    }
}

std::int64_t LooperEngine::alignToLongestPlayingLoopWrap(
    const std::int64_t baseExecute, const std::int64_t pos, const bool waitFullCycleIfOnBoundary) const noexcept
{
    const int idx = longestPlayingLoopIndex(-1);
    if (idx < 0)
    {
        return baseExecute;
    }
    return alignToTrackLoopWrap(idx, baseExecute, pos, waitFullCycleIfOnBoundary);
}

std::int64_t LooperEngine::alignToTrackLoopWrap(const int trackIndex, const std::int64_t baseExecute,
    const std::int64_t pos, const bool waitFullCycleIfOnBoundary) const noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return baseExecute;
    }
    const auto loopLen = tracks_[static_cast<std::size_t>(trackIndex)].lengthSamples();
    if (loopLen < 2)
    {
        return baseExecute;
    }
    const auto lenI = static_cast<std::int64_t>(loopLen);
    const auto phMod = static_cast<std::int64_t>(
        loopPositionFromTransport(pos, playbackPhaseOrigin(trackIndex), loopLen));
    if (phMod == 0)
    {
        if (waitFullCycleIfOnBoundary)
        {
            return std::max(baseExecute, pos + lenI);
        }
        return std::max(baseExecute, pos);
    }
    return std::max(baseExecute, pos + (lenI - phMod));
}

bool LooperEngine::followerRecordUsesTrackSync(const int trackIndex) const noexcept
{
    if (syncMode_ != SyncMode::Synced || trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return false;
    }
    const int master = trackSyncMasterIndex();
    if (master < 0 || master == trackIndex)
    {
        return false;
    }
    return tracks_[static_cast<std::size_t>(master)].lengthSamples() > 0;
}

std::int64_t LooperEngine::trackSyncUnitLengthSamples(const int masterIndex) const noexcept
{
    if (masterIndex < 0 || masterIndex >= kMaxTracks)
    {
        return 0;
    }
    const auto& master = tracks_[static_cast<std::size_t>(masterIndex)];
    const auto loopLen = master.lengthSamples();
    if (loopLen == 0)
    {
        return 0;
    }
    const auto cycle = master.cycleLengthSamples();
    switch (trackSyncUnit_)
    {
        case TrackSyncUnit::Loop:
            return static_cast<std::int64_t>(loopLen);
        case TrackSyncUnit::Cycle:
            return static_cast<std::int64_t>(std::max<std::size_t>(1, cycle));
        case TrackSyncUnit::Subcycle: {
            const auto div = static_cast<std::size_t>(std::max(1, subcycles_));
            return static_cast<std::int64_t>(std::max<std::size_t>(1, cycle / div));
        }
        default:
            return static_cast<std::int64_t>(loopLen);
    }
}

std::int64_t LooperEngine::alignToTrackSyncUnit(
    const std::int64_t baseExecute, const std::int64_t pos, const bool waitFullCycleIfOnBoundary) const noexcept
{
    const auto unit = trackSyncUnitLengthSamples(trackSyncMasterIndex());
    if (unit <= 0)
    {
        return baseExecute;
    }
    const auto t = alignToPeriodBoundaryWithOrigin(pos, unit, sharedLoopPhaseOrigin_);
    if (waitFullCycleIfOnBoundary && t == pos)
    {
        return std::max(baseExecute, pos + unit);
    }
    return std::max(baseExecute, t);
}

bool LooperEngine::noTrackPlayingRecordingOrOverdub() const noexcept
{
    for (const auto& t : tracks_)
    {
        const auto s = t.state();
        if (s == TrackState::Playing || s == TrackState::Recording || s == TrackState::Overdub
            || trackStateAdvancesPlayhead(s))
        {
            return false;
        }
    }
    return true;
}

bool LooperEngine::hasActivePeerTransport(const int exceptTrackIndex) const noexcept
{
    for (int i = 0; i < kMaxTracks; ++i)
    {
        if (i == exceptTrackIndex)
        {
            continue;
        }
        const auto s = tracks_[static_cast<std::size_t>(i)].state();
        if (s == TrackState::Playing || s == TrackState::Overdub || s == TrackState::Recording
            || trackStateAdvancesPlayhead(s))
        {
            return true;
        }
    }
    return false;
}

void LooperEngine::adoptSharedPhaseOriginIfNoActivePeers(const int exceptTrackIndex, const std::int64_t originSamples) noexcept
{
    if (hasActivePeerTransport(exceptTrackIndex))
    {
        return;
    }
    if (metronomeForceRunning_)
    {
        // The click is the master clock. Starting a loop must not move it.
        return;
    }
    sharedLoopPhaseOrigin_ = originSamples;
    for (int i = 0; i < kMaxTracks; ++i)
    {
        if (i == exceptTrackIndex)
        {
            continue;
        }
        const auto st = tracks_[static_cast<std::size_t>(i)].state();
        if (trackStateAdvancesPlayhead(st) || st == TrackState::Recording)
        {
            continue;
        }
        resetStretchForTrack(i);
        trackPlayheads_[static_cast<std::size_t>(i)] = 0;
    }
}

std::int64_t LooperEngine::nextLoopBoundarySample(const int trackIndex, const std::int64_t pos) const noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return pos;
    }
    const auto& tr = tracks_[static_cast<std::size_t>(trackIndex)];
    const auto len = tr.lengthSamples();
    if (len == 0)
    {
        return pos;
    }
    const std::size_t loopPos = loopPositionFromTransport(pos, playbackPhaseOrigin(trackIndex), len);
    const auto phMod = static_cast<std::int64_t>(loopPos);
    const auto lenI = static_cast<std::int64_t>(len);
    const auto rem = (phMod == 0) ? lenI : (lenI - phMod);
    return pos + rem;
}

std::size_t LooperEngine::longestLoopLengthSamples() const noexcept
{
    std::size_t maxLen = 0;
    for (const auto& tr : tracks_)
    {
        maxLen = std::max(maxLen, tr.lengthSamples());
    }
    return maxLen;
}

void LooperEngine::lockTrackPhaseToPeer(const int trackIndex) noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return;
    }
    const int peer = longestPlayingLoopIndex(trackIndex);
    const auto ti = static_cast<std::size_t>(trackIndex);
    if (peer < 0)
    {
        clearRecordPhaseAnchor(trackIndex);
        clearPlayStartPhase(trackIndex);
        return;
    }
    const auto peerTi = static_cast<std::size_t>(peer);
    const auto origin = playbackPhaseOrigin(peer);
    if (trackPhaseFromStart_[peerTi])
    {
        clearRecordPhaseAnchor(trackIndex);
        trackPhaseFromStart_[ti] = true;
        trackPhaseOrigin_[ti] = origin;
        return;
    }
    if (trackRecordPhaseAnchor_[peerTi])
    {
        clearPlayStartPhase(trackIndex);
        trackRecordPhaseAnchor_[ti] = true;
        trackRecordPhaseOrigin_[ti] = origin;
        return;
    }
    clearRecordPhaseAnchor(trackIndex);
    clearPlayStartPhase(trackIndex);
}

int LooperEngine::longestPlayingLoopIndex(const int exceptTrackIndex) const noexcept
{
    int best = -1;
    std::size_t maxLen = 0;
    int maxCycles = 0;
    for (int i = 0; i < kMaxTracks; ++i)
    {
        if (i == exceptTrackIndex)
        {
            continue;
        }
        const auto& tr = tracks_[static_cast<std::size_t>(i)];
        const auto len = tr.lengthSamples();
        if (len == 0)
        {
            continue;
        }
        const auto st = tr.state();
        if (!trackStateAdvancesPlayhead(st) && st != TrackState::Recording && st != TrackState::Overdub)
        {
            continue;
        }
        const int cycles = tr.cycleCount();
        if (len > maxLen || (len == maxLen && cycles > maxCycles))
        {
            best = i;
            maxLen = len;
            maxCycles = cycles;
        }
    }
    return best;
}

std::size_t LooperEngine::longestPlayingLoopLengthSamples(const int exceptTrackIndex) const noexcept
{
    const int idx = longestPlayingLoopIndex(exceptTrackIndex);
    if (idx < 0)
    {
        return 0;
    }
    return tracks_[static_cast<std::size_t>(idx)].lengthSamples();
}

bool LooperEngine::syncModeUsesSerialHandoff() const noexcept
{
    return syncMode_ == SyncMode::Serial || syncMode_ == SyncMode::SerialParallel;
}

bool LooperEngine::trackIsSerialExclusive(const int trackIndex) const noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return false;
    }
    if (syncMode_ == SyncMode::Serial)
    {
        return true;
    }
    if (syncMode_ == SyncMode::SerialParallel)
    {
        return trackIndex != 0;
    }
    return false;
}

bool LooperEngine::hasSerialCompetitor(const int playTrackIndex) const noexcept
{
    for (int i = 0; i < kMaxTracks; ++i)
    {
        if (i == playTrackIndex || !trackIsSerialExclusive(i))
        {
            continue;
        }
        const auto& tr = tracks_[static_cast<std::size_t>(i)];
        const auto s = tr.state();
        if (tr.lengthSamples() == 0)
        {
            continue;
        }
        if (trackStateAdvancesPlayhead(s))
        {
            return true;
        }
    }
    return false;
}

std::int64_t LooperEngine::computeSerialHandoffExecuteSample(const int playTrackIndex, const std::int64_t pos) const noexcept
{
    std::int64_t maxT = pos;
    for (int i = 0; i < kMaxTracks; ++i)
    {
        if (i == playTrackIndex || !trackIsSerialExclusive(i))
        {
            continue;
        }
        const auto& tr = tracks_[static_cast<std::size_t>(i)];
        const auto s = tr.state();
        if (tr.lengthSamples() == 0)
        {
            continue;
        }
        if (trackStateAdvancesPlayhead(s))
        {
            maxT = std::max(maxT, nextLoopBoundarySample(i, pos));
        }
    }
    return maxT;
}

bool LooperEngine::tryScheduleSerialHandoff(const EngineCommand& command, const std::int64_t blockStartSample) noexcept
{
    if (!syncModeUsesSerialHandoff())
    {
        return false;
    }
    if (dubModeEnabled_
        && (command.type == CommandType::Play || command.type == CommandType::PlayStopToggle))
    {
        return false;
    }

    const int playTrack = command.trackIndex;
    if (playTrack < 0 || playTrack >= kMaxTracks || !trackIsSerialExclusive(playTrack))
    {
        return false;
    }
    if (muteModeEnabled_ && (command.type == CommandType::Play || command.type == CommandType::PlayStopToggle))
    {
        const auto st = tracks_[static_cast<std::size_t>(playTrack)].state();
        if (trackStateAdvancesPlayhead(st))
        {
            return false;
        }
    }

    const auto& target = tracks_[static_cast<std::size_t>(playTrack)];

    bool wantStart = false;
    if (command.type == CommandType::Play || command.type == CommandType::PlayStopToggle)
    {
        wantStart = (target.state() == TrackState::Stopped || target.state() == TrackState::Empty)
            && target.lengthSamples() > 0;
    }
    else
    {
        return false;
    }

    if (!wantStart)
    {
        return false;
    }

    if (!hasSerialCompetitor(playTrack))
    {
        return false;
    }

    maybeStartSerialParallelBed(transport_.samplePosition());

    const auto pos = transport_.samplePosition();
    std::int64_t execAt = pos;
    if (!freeMode_)
    {
        // None: now. Bar: next click bar. Loop: this track's own cycle.
        // alignPlayExecuteSample would wait for the other loop instead.
        execAt = alignExecuteSampleForPlaybackMode(playTrack, pos, pos, effectivePlayMode(target));
    }

    EngineCommand sh{};
    sh.type = CommandType::SerialHandoff;
    sh.trackIndex = playTrack;

    for (int t = 0; t < kMaxTracks; ++t)
    {
        cancelScheduledCommandsForTrack(t, CommandType::SerialHandoff);
    }

    if (execAt <= blockStartSample)
    {
        applyCommand(sh, execAt, execAt);
    }
    else
    {
        (void) pushScheduledCommand(sh, execAt);
    }
    return true;
}

void LooperEngine::maybeStartSerialParallelBed(const std::int64_t origin) noexcept
{
    if (syncMode_ != SyncMode::SerialParallel)
    {
        return;
    }
    auto& bed = tracks_[0];
    if (bed.lengthSamples() == 0)
    {
        return;
    }
    const auto st = bed.state();
    if (st != TrackState::Stopped && st != TrackState::Empty)
    {
        return;
    }
    adoptSharedPhaseOriginIfNoActivePeers(0, origin);
    preparePlayStartPhase(0, origin);
    if (bed.mix().muted)
    {
        bed.setState(TrackState::Muted);
        setMuteAudible(0, false);
        return;
    }
    bed.setState(TrackState::Playing);
    setMuteAudible(0, true);
    serialFadeInEndSample_[0] = origin + static_cast<std::int64_t>(serialCrossfadeSamples_);
}

void LooperEngine::maybeStartSerialParallelBedAfterExclusiveStart(const int exclusiveIndex, const TrackState prior,
    const std::int64_t origin) noexcept
{
    if (!trackIsSerialExclusive(exclusiveIndex))
    {
        return;
    }
    const auto& exclusive = tracks_[static_cast<std::size_t>(exclusiveIndex)];
    const auto st = exclusive.state();
    const bool launched = st == TrackState::Playing || st == TrackState::Overdub || st == TrackState::Muted
        || st == TrackState::Recording || st == TrackState::Armed || st == TrackState::Playing_DubPending
        || ((prior == TrackState::Stopped || prior == TrackState::Empty) && exclusive.lengthSamples() > 0
            && trackStateAdvancesPlayhead(st));
    if (!launched)
    {
        return;
    }
    maybeStartSerialParallelBed(origin);
}

bool LooperEngine::exclusiveLaunchPullsSerialParallelBed(const EngineCommand& command) const noexcept
{
    if (syncMode_ != SyncMode::SerialParallel || !trackIsSerialExclusive(command.trackIndex))
    {
        return false;
    }
    const auto& tr = tracks_[static_cast<std::size_t>(command.trackIndex)];
    const auto st = tr.state();
    if (command.type == CommandType::Play || command.type == CommandType::PlayStopToggle)
    {
        return st == TrackState::Stopped || st == TrackState::Empty || st == TrackState::Armed;
    }
    if (command.type == CommandType::RecordToggle)
    {
        return st == TrackState::Empty || st == TrackState::Stopped || st == TrackState::Armed
            || st == TrackState::Playing;
    }
    if (command.type == CommandType::OverdubToggle)
    {
        return st == TrackState::Empty || st == TrackState::Stopped || st == TrackState::Playing;
    }
    return false;
}

void LooperEngine::capturePlayAllResumeMaskIfSerial() noexcept
{
    if (!syncModeUsesSerialHandoff())
    {
        return;
    }
    std::uint8_t mask = 0;
    for (int i = 0; i < kMaxTracks; ++i)
    {
        const auto s = tracks_[static_cast<std::size_t>(i)].state();
        if (s == TrackState::Recording || s == TrackState::Armed || trackStateAdvancesPlayhead(s))
        {
            mask = static_cast<std::uint8_t>(mask | (1u << i));
        }
    }
    if (mask == 0)
    {
        return;
    }
    playAllResumeMask_ = mask;
    playAllResumeMaskValid_ = true;
}

bool LooperEngine::playAllShouldStartTrack(const int trackIndex) const noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return false;
    }
    if (!syncModeUsesSerialHandoff() || !playAllResumeMaskValid_)
    {
        return true;
    }
    return (playAllResumeMask_ & static_cast<std::uint8_t>(1u << trackIndex)) != 0;
}

void LooperEngine::captureSerialOutTail(const int trackIndex, const std::int64_t handoffSample) noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return;
    }
    const auto ti = static_cast<std::size_t>(trackIndex);
    auto& track = tracks_[ti];
    const auto len = track.lengthSamples();
    if (len == 0)
    {
        serialTailLen_[ti] = 0;
        return;
    }
    const int K = std::min(serialCrossfadeSamples_, static_cast<int>(len));
    if (K <= 0)
    {
        serialTailLen_[ti] = 0;
        return;
    }
    if (serialTailL_[ti].size() < static_cast<std::size_t>(K)
        || serialTailR_[ti].size() < static_cast<std::size_t>(K))
    {
        serialTailLen_[ti] = 0;
        return;
    }
    const auto mix = track.mix();
    float gl = 1.0f;
    float gr = 1.0f;
    stereoPanGains(trackStereoBusProcessor_ ? 0.0f : mix.pan, gl, gr);
    const std::int64_t phaseOrigin = sharedLoopPhaseOrigin_;
    for (int j = 0; j < K; ++j)
    {
        const std::int64_t ts = handoffSample + static_cast<std::int64_t>(j);
        const auto loopPos = loopPositionFromTransport(ts, phaseOrigin, len);
        const float rawL = track.readSample(0, loopPos) * mix.volume;
        const float rawR = track.readSample(track.channels() > 1 ? 1 : 0, loopPos) * mix.volume;
        serialTailL_[ti][static_cast<std::size_t>(j)] = rawL * gl;
        serialTailR_[ti][static_cast<std::size_t>(j)] = rawR * gr;
    }
    serialTailLen_[ti] = K;
    serialTailHandoffSample_[ti] = handoffSample;
}

float LooperEngine::serialFadeInMultiplier(const int trackIndex, const std::int64_t transportSample) const noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return 1.f;
    }
    const auto ti = static_cast<std::size_t>(trackIndex);
    const std::int64_t fadeEnd = serialFadeInEndSample_[ti];
    if (fadeEnd <= 0)
    {
        return 1.f;
    }
    const int K = serialCrossfadeSamples_;
    const std::int64_t fadeStart = fadeEnd - static_cast<std::int64_t>(K);
    if (transportSample < fadeStart)
    {
        return 1.f;
    }
    if (transportSample >= fadeEnd)
    {
        return 1.f;
    }
    return std::min(1.f, (static_cast<float>(transportSample - fadeStart) + 1.f) / static_cast<float>(K));
}

std::int64_t LooperEngine::computeExecuteSampleForCommand(const EngineCommand& command) const noexcept
{
    if (command.type == CommandType::SyncAllToStart || command.type == CommandType::SerialHandoff)
    {
        return transport_.samplePosition();
    }
    if (freeMode_)
    {
        return transport_.samplePosition();
    }

    const auto pos = transport_.samplePosition();
    std::int64_t executeAt = pos;

    if (command.executeAtSample >= 0)
    {
        return command.executeAtSample;
    }
    if (command.immediate
        || (linkTransportImmediate_.load(std::memory_order_relaxed) && command.type == CommandType::StopAll))
    {
        return pos;
    }
    if (linkTransportImmediate_.load(std::memory_order_relaxed) && command.type == CommandType::PlayAll)
    {
        const auto at = linkQuantumLaunchSample_.load(std::memory_order_relaxed);
        if (at >= pos)
        {
            return at;
        }
        return pos;
    }
    if (idlePlaybackStartsImmediately(command))
    {
        return pos;
    }
    if (!clockGridIsActive() && clusteredPlayExecuteAt_ >= 0 && commandStartsPlayback(command))
    {
        return clusteredPlayExecuteAt_;
    }
    if (!clockGridIsActive() && syncMode_ == SyncMode::Synced && pos < idleLaunchGroupEndsAt_
        && commandStartsPlayback(command))
    {
        return pos;
    }

    // Empty session with no typed tempo: Rec / Play / Stop now so the first take can set tempo.
    if (emptySessionFirstTakeIsImmediate()
        && (command.type == CommandType::RecordToggle || command.type == CommandType::Play
            || command.type == CommandType::PlayAll || command.type == CommandType::PlayStopToggle
            || command.type == CommandType::Stop))
    {
        if (command.type == CommandType::RecordToggle
            && trackRecordsInternalGraph(command.trackIndex))
        {
            const int off = firstPassRecordOffsetInBlock_.load(std::memory_order_acquire);
            if (off > 0)
            {
                return pos + static_cast<std::int64_t>(off);
            }
        }
        return pos;
    }

    if (command.type == CommandType::PlayAll)
    {
        return alignExecuteSampleForPlaybackMode(0, pos, pos, playAllMode_);
    }
    if (command.type == CommandType::StopAll)
    {
        return alignExecuteSampleForStopMode(pos, pos, stopAllMode_);
    }
    if (command.trackIndex >= 0 && command.trackIndex < kMaxTracks
        && (command.type == CommandType::Play || command.type == CommandType::PlayStopToggle))
    {
        const auto& trMode = tracks_[static_cast<std::size_t>(command.trackIndex)];
        const auto stMode = trMode.state();
        if (muteModeEnabled_ && !dubModeEnabled_
            && (stMode == TrackState::Playing || stMode == TrackState::Overdub || stMode == TrackState::Muted
                || trackStateIsMutePending(stMode)))
        {
            return alignExecuteSampleForPlaybackMode(
                command.trackIndex, pos, pos, effectiveMuteMode(trMode));
        }
        if (dubModeEnabled_ && (stMode == TrackState::Stopped || stMode == TrackState::Empty)
            && trMode.lengthSamples() > 0)
        {
            return alignOverdubExecuteSample(command.trackIndex, pos, pos);
        }
    }
    if (command.type == CommandType::UndoOverdub || command.type == CommandType::RedoOverdub)
    {
        int t = command.trackIndex;
        if (t < 0 || t >= kMaxTracks)
        {
            t = 0;
            for (int i = 0; i < kMaxTracks; ++i)
            {
                const auto& tr = tracks_[static_cast<std::size_t>(i)];
                if (tr.hasOverdubUndoSnapshot() || tr.hasOverdubRedoSnapshot())
                {
                    t = i;
                    break;
                }
            }
        }
        return alignExecuteSampleForPlaybackMode(t, pos, pos, undoStartMode_);
    }
    if (command.type == CommandType::MuteToggle && command.trackIndex >= 0 && command.trackIndex < kMaxTracks)
    {
        const auto& trMute = tracks_[static_cast<std::size_t>(command.trackIndex)];
        return alignExecuteSampleForPlaybackMode(command.trackIndex, pos, pos, effectiveMuteMode(trMute));
    }

    if (command.type == CommandType::PlayStopToggle && command.trackIndex >= 0 && command.trackIndex < kMaxTracks)
    {
        const auto& trPad = tracks_[static_cast<std::size_t>(command.trackIndex)];
        const auto st = trPad.state();
        // Enter capture on overdub timing; leaving capture is a Play action.
        if (playStopShouldOverdub(command.trackIndex))
            return alignOverdubExecuteSample(command.trackIndex, pos, pos);
        if (st == TrackState::Armed)
        {
            return pos;
        }
        if (st == TrackState::Overdub || trackStateIsOverdub(st))
        {
            if (playbackStartModeIsLoopCycle(effectiveOverdubMode(trPad)))
            {
                return alignOverdubCueEnd(command.trackIndex, pos, pos);
            }
            if (effectiveStopQuantizeMode(trPad) == GlobalStopMode::LoopEnd)
            {
                return alignRecordedCycleEnd(command.trackIndex, pos, pos);
            }
            return alignPlayExecuteSample(command.trackIndex, pos, pos);
        }
        if (st == TrackState::Recording)
        {
            if (effectiveStopQuantizeMode(trPad) == GlobalStopMode::LoopEnd)
            {
                return alignRecordedCycleEnd(command.trackIndex, pos, pos);
            }
            return alignPlayExecuteSample(command.trackIndex, pos, pos);
        }
        if (st == TrackState::Playing || st == TrackState::Playing_MutePending || st == TrackState::Playing_DubPending
            || st == TrackState::Muted || st == TrackState::Muted_UnmutePending)
        {
            return alignStopExecuteSample(command.trackIndex, pos, pos);
        }
        if (st == TrackState::Stopped || st == TrackState::Empty)
        {
            if (trPad.lengthSamples() == 0)
            {
                auto recAt = alignRecordExecuteSample(command.trackIndex, pos, pos);
                if (!emptySessionFirstTakeIsImmediate())
                {
                    recAt = std::max(recAt, alignCountInExecuteSample(pos));
                }
                return recAt;
            }
            return alignPlayExecuteSample(command.trackIndex, pos, pos);
        }
        return pos;
    }

    if (command.type == CommandType::RecordToggle && command.trackIndex >= 0 && command.trackIndex < kMaxTracks)
    {
        const auto& trRec = tracks_[static_cast<std::size_t>(command.trackIndex)];
        if (trRec.state() == TrackState::Armed)
        {
            return pos;
        }
        if (trRec.state() == TrackState::Recording)
        {
            return alignStopExecuteSample(command.trackIndex, pos, pos);
        }
        if (trackStateIsOverdub(trRec.state()))
        {
            if (playbackStartModeIsLoopCycle(effectiveOverdubMode(trRec)))
            {
                return alignOverdubCueEnd(command.trackIndex, pos, pos);
            }
            if (effectiveStopQuantizeMode(trRec) == GlobalStopMode::LoopEnd)
            {
                return alignRecordedCycleEnd(command.trackIndex, pos, pos);
            }
            return alignOverdubExecuteSample(command.trackIndex, pos, pos);
        }
        const auto recMode = effectiveRecordMode(trRec);
        if (playbackStartModeIsLoopCycle(recMode))
        {
            executeAt = alignLoopCueStart(command.trackIndex, pos, pos);
            if (!emptySessionFirstTakeIsImmediate())
            {
                executeAt = std::max(executeAt, alignCountInExecuteSample(pos));
            }
            return executeAt;
        }
        const bool startingFirstTake =
            (trRec.state() == TrackState::Empty || trRec.state() == TrackState::Stopped)
            && trRec.lengthSamples() == 0;
        if (startingFirstTake && followerRecordUsesTrackSync(command.trackIndex))
        {
            if (effectiveRecordMode(trRec) == PlaybackStartMode::Immediate)
            {
                executeAt = pos;
            }
            else
            {
                executeAt = alignRecordExecuteSample(command.trackIndex, pos, pos);
            }
            if (!emptySessionFirstTakeIsImmediate())
            {
                executeAt = std::max(executeAt, alignCountInExecuteSample(pos));
            }
            return executeAt;
        }
        if (startingFirstTake)
        {
            executeAt = alignRecordExecuteSample(command.trackIndex, pos, pos);
        }
        else if (trRec.state() == TrackState::Playing || trRec.state() == TrackState::Muted
            || trRec.state() == TrackState::Playing_DubPending || trRec.state() == TrackState::Playing_MutePending)
        {
            executeAt = alignOverdubExecuteSample(command.trackIndex, pos, pos);
        }
        else if (trRec.state() == TrackState::Stopped && trRec.lengthSamples() > 0)
        {
            executeAt = alignRecordExecuteSample(command.trackIndex, pos, pos);
        }
        else
        {
            executeAt = alignPlayExecuteSample(command.trackIndex, pos, pos);
        }
        if (!emptySessionFirstTakeIsImmediate())
        {
            executeAt = std::max(executeAt, alignCountInExecuteSample(pos));
        }
    }
    else if (command.type == CommandType::Play && command.trackIndex >= 0 && command.trackIndex < kMaxTracks)
    {
        const auto& trPlay = tracks_[static_cast<std::size_t>(command.trackIndex)];
        if ((trPlay.state() == TrackState::Overdub || trackStateIsOverdub(trPlay.state()))
            && playbackStartModeIsLoopCycle(effectiveOverdubMode(trPlay)))
        {
            executeAt = alignOverdubCueEnd(command.trackIndex, pos, pos);
        }
        else if ((trPlay.state() == TrackState::Recording || trPlay.state() == TrackState::Overdub
                || trackStateIsOverdub(trPlay.state()))
            && effectiveStopQuantizeMode(trPlay) == GlobalStopMode::LoopEnd)
        {
            executeAt = alignRecordedCycleEnd(command.trackIndex, pos, pos);
        }
        else if (trPlay.lengthSamples() == 0)
        {
            executeAt = alignRecordExecuteSample(command.trackIndex, pos, pos);
            if (!emptySessionFirstTakeIsImmediate())
            {
                executeAt = std::max(executeAt, alignCountInExecuteSample(pos));
            }
        }
        else
        {
            executeAt = alignPlayExecuteSample(command.trackIndex, pos, pos);
        }
    }
    else if (command.type == CommandType::Stop && command.trackIndex >= 0 && command.trackIndex < kMaxTracks)
    {
        executeAt = alignStopExecuteSample(command.trackIndex, pos, pos);
    }
    else if (command.type == CommandType::OverdubToggle && command.trackIndex >= 0 && command.trackIndex < kMaxTracks)
    {
        const auto& trDub = tracks_[static_cast<std::size_t>(command.trackIndex)];
        if ((trDub.state() == TrackState::Overdub || trackStateIsOverdub(trDub.state()))
            && playbackStartModeIsLoopCycle(effectiveOverdubMode(trDub)))
        {
            executeAt = alignOverdubCueEnd(command.trackIndex, pos, pos);
        }
        else if ((trDub.state() == TrackState::Overdub || trackStateIsOverdub(trDub.state()))
            && effectiveStopQuantizeMode(trDub) == GlobalStopMode::LoopEnd)
        {
            executeAt = alignRecordedCycleEnd(command.trackIndex, pos, pos);
        }
        else
        {
            executeAt = alignOverdubExecuteSample(command.trackIndex, pos, pos);
        }
        if (trDub.state() == TrackState::Playing)
        {
            executeAt = std::max(executeAt, alignCountInExecuteSample(pos));
        }
    }

    return executeAt;
}

void LooperEngine::applyCommand(const EngineCommand& command,
    const std::int64_t scheduledExecuteAtSample,
    const std::int64_t commandContextTransport)
{
    const std::int64_t ctx =
        commandContextTransport >= 0 ? commandContextTransport : transport_.samplePosition();

    if (command.type == CommandType::MuteAllToggle)
    {
        const int count=command.trackCount>0?std::min(command.trackCount,kMaxTracks):kMaxTracks;
        bool allMuted=true;for(int t=0;t<count;++t)allMuted=allMuted && trackMuted(t);
        for(int t=0;t<count;++t)setTrackMuted(t,!allMuted);
        return;
    }
    if (command.type == CommandType::ClearAll)
    {
        const int count=command.trackCount>0?std::min(command.trackCount,kMaxTracks):kMaxTracks;
        for (int t = 0; t < count; ++t)
        {
            cancelAllScheduledCommandsForTrack(t);
            applyCommand({CommandType::Clear, t, true}, scheduledExecuteAtSample, ctx);
        }
        return;
    }
    if (command.type == CommandType::UndoOverdub || command.type == CommandType::RedoOverdub)
    {
        bool restoredClearedLoop = false;
        const auto restoreOne = [this, &command, &restoredClearedLoop](int t)
        {
            if (t < 0 || t >= kMaxTracks)
            {
                return;
            }
            auto& tr = tracks_[static_cast<std::size_t>(t)];
            const bool wasEmpty = tr.lengthSamples() < 2;
            if (command.type == CommandType::UndoOverdub)
            {
                if (!tr.hasOverdubUndoSnapshot())
                {
                    return;
                }
                tr.restoreOverdubUndoSnapshot();
            }
            else
            {
                if (!tr.hasOverdubRedoSnapshot())
                {
                    return;
                }
                tr.restoreOverdubRedoSnapshot();
            }
            if (wasEmpty && tr.lengthSamples() >= 2)
            {
                restoredClearedLoop = true;
                tr.setState(TrackState::Stopped);
                trackPlayheads_[static_cast<std::size_t>(t)] = 0;
                trackWriteheads_[static_cast<std::size_t>(t)] = 0;
                resetStretchForTrack(t);
            }
            else if (!wasEmpty && tr.lengthSamples() < 2)
            {
                tr.setState(TrackState::Empty);
                trackPlayheads_[static_cast<std::size_t>(t)] = 0;
                trackWriteheads_[static_cast<std::size_t>(t)] = 0;
                clearRecordPhaseAnchor(t);
                resetStretchForTrack(t);
                reassignTrackSyncMasterAfterClear(t);
            }
            else if (tr.state() == TrackState::Overdub || tr.state() == TrackState::Playing_DubPending
                || tr.state() == TrackState::Overdubbing_MutePending)
            {
                tr.setState(TrackState::Playing);
                trackRecordInputGateTransport_[static_cast<std::size_t>(t)] = -1;
            }
            if (waveformEnvelopes_ != nullptr)
            {
                // Same-length overdub undo keeps the loop period. Without force,
                // the envelope publish treats that as unchanged and the plot stays
                // on the layer that was just removed.
                waveformEnvelopes_->rebuildFromTrack(t, tr, 0, true);
            }
        };
        if (command.trackIndex < 0)
        {
            for (int t = 0; t < kMaxTracks; ++t)
            {
                restoreOne(t);
            }
        }
        else
        {
            restoreOne(command.trackIndex);
        }
        if (restoredClearedLoop && !tempoManager_.isLocked())
        {
            restoreSessionTempoFromUndo();
        }
        return;
    }

    if (command.type == CommandType::SyncAllToStart)
    {
        const std::int64_t now = transport_.samplePosition();
        sharedLoopPhaseOrigin_ = now;
        for (int i = 0; i < kMaxTracks; ++i)
        {
            clearPlayStartPhase(i);
            clearRecordPhaseAnchor(i);
            if (tracks_[static_cast<std::size_t>(i)].lengthSamples() == 0)
            {
                continue;
            }
            resetStretchForTrack(i);
        }
        return;
    }

    if (command.type == CommandType::SerialHandoff)
    {
        if (command.trackIndex < 0 || command.trackIndex >= kMaxTracks)
        {
            return;
        }
        const int playIdx = command.trackIndex;
        const std::int64_t S = scheduledExecuteAtSample >= 0 ? scheduledExecuteAtSample : ctx;

        for (int i = 0; i < kMaxTracks; ++i)
        {
            if (i == playIdx || !trackIsSerialExclusive(i))
            {
                continue;
            }
            auto& tr = tracks_[static_cast<std::size_t>(i)];
            if (tr.state() == TrackState::Playing || tr.state() == TrackState::Overdub)
            {
                if (tr.lengthSamples() > 0)
                {
                    captureSerialOutTail(i, S);
                }
                tr.setState(TrackState::Stopped);
                resetLoopPlaybackToBeginning(i, scheduledExecuteAtSample);
            }
        }

        for (int j = 0; j < kMaxTracks; ++j)
        {
            serialFadeInEndSample_[static_cast<std::size_t>(j)] = 0;
        }

        auto& playTrack = tracks_[static_cast<std::size_t>(playIdx)];
        if (playTrack.lengthSamples() > 0)
        {
            preparePlayStartPhase(playIdx, S);
            // A stopped loop's mute ramp is already at zero. Leaving that in place
            // shows Playing and outputs silence after the handoff.
            if (playTrack.mix().muted)
            {
                playTrack.setState(TrackState::Muted);
                setMuteAudible(playIdx, false);
            }
            else
            {
                playTrack.setState(TrackState::Playing);
                setMuteAudible(playIdx, true);
            }
            serialFadeInEndSample_[static_cast<std::size_t>(playIdx)] =
                S + static_cast<std::int64_t>(serialCrossfadeSamples_);
        }
        maybeStartSerialParallelBed(S);
        return;
    }

    if (command.type == CommandType::PlayAll)
    {
        const std::int64_t origin = scheduledExecuteAtSample >= 0 ? scheduledExecuteAtSample : ctx;
        // With no loop content, a typed tempo makes Play All start the clock.
        // Once loops exist, the same transport must start them, including after
        // Stop All and session restore. A committed tempo must not bypass them.
        if (userTempoCommitted_ && !anyLoopHasContent() && !anyTrackPlayingOrPendingLaunch())
        {
            metronomeEnabled_ = true;
            metronomeForceRunning_ = true;
            if (sharedLoopPhaseOrigin_ == 0)
            {
                bool foundLoop = false;
                std::int64_t adopted = 0;
                for (int i = 0; i < kMaxTracks; ++i)
                {
                    if (tracks_[static_cast<std::size_t>(i)].lengthSamples() == 0)
                    {
                        continue;
                    }
                    adopted = playbackPhaseOrigin(i);
                    foundLoop = true;
                    break;
                }
                if (!foundLoop)
                {
                    sharedLoopPhaseOrigin_ = origin;
                }
                else
                {
                    sharedLoopPhaseOrigin_ = adopted;
                }
            }
            return;
        }
        if (playAllStartsClickWhenEmpty_ && metronomeEnabled_ && !anyLoopHasContent())
        {
            metronomeForceRunning_ = true;
            if (sharedLoopPhaseOrigin_ == 0)
            {
                sharedLoopPhaseOrigin_ = origin;
            }
        }
        bool started = false;
        for (int i = 0; i < kMaxTracks; ++i)
        {
            auto& tr = tracks_[static_cast<std::size_t>(i)];
            if (tr.lengthSamples() == 0 || !playAllShouldStartTrack(i))
            {
                continue;
            }
            const auto st = tr.state();
            if (st != TrackState::Stopped && st != TrackState::Empty)
            {
                continue;
            }
            if (!started)
            {
                // One origin for every loop. The first recorded take owns the shared
                // clock; later takes keep a private anchor so they open at sample 0.
                // Moving only the shared clock left those takes on the old anchor.
                if (!linkTransportImmediate_.load(std::memory_order_relaxed))
                {
                    adoptSharedPhaseOriginIfNoActivePeers(-1, origin);
                }
                else
                {
                    sharedLoopPhaseOrigin_ = origin;
                }
                started = true;
            }
            const auto ti = static_cast<std::size_t>(i);
            clearRecordPhaseAnchor(i);
            clearPlayStartPhase(i);
            trackPhaseFromStart_[ti] = true;
            trackPhaseOrigin_[ti] = origin;
            resetStretchForTrack(i);
            trackPlayheads_[ti] = 0;
            stretchPhaseOrigin_[ti] = origin;
            tr.setState(TrackState::Playing);
            setMuteAudible(i, true);
        }
        return;
    }

    if (command.type == CommandType::StopAll)
    {
        capturePlayAllResumeMaskIfSerial();
        for (int i = 0; i < kMaxTracks; ++i)
        {
            auto& tr = tracks_[static_cast<std::size_t>(i)];
            const auto st = tr.state();
            if (st == TrackState::Armed)
            {
                cancelScheduledRecordToggleForTrack(i);
                tr.setState(TrackState::Empty);
                continue;
            }
            if (st == TrackState::Recording)
            {
                finishRecordingToPlaying_[static_cast<std::size_t>(i)] = false;
                finalizeOrDeferRecordingStop(i, scheduledExecuteAtSample, ctx);
                tr.setState(TrackState::Stopped);
                resetLoopPlaybackToBeginning(i, scheduledExecuteAtSample);
                continue;
            }
            if (st == TrackState::Playing || st == TrackState::Overdub || trackStateAdvancesPlayhead(st))
            {
                enterStoppedWithSpillover(i, scheduledExecuteAtSample);
            }
        }
        metronomeForceRunning_ = false;
        {
            const std::int64_t origin = scheduledExecuteAtSample >= 0 ? scheduledExecuteAtSample : ctx;
            adoptSharedPhaseOriginIfNoActivePeers(-1, origin);
        }
        return;
    }

    if (command.trackIndex < 0 || command.trackIndex >= kMaxTracks)
    {
        return;
    }

    auto& track = tracks_[static_cast<std::size_t>(command.trackIndex)];
    auto& lastErr = lastTransitionError_[static_cast<std::size_t>(command.trackIndex)];
    lastErr = TransitionError::None;
    const auto current = track.state();

    switch (command.type)
    {
        case CommandType::RecordToggle:
        {
            const auto runtime = runtimeStateForTrack(track);
            const auto trn = resolveLoopTrackTransition(runtime,
                LoopTrackCommand::Record,
                track.lengthSamples() > 0,
                trackBehaviorMode_,
                afterRecordMode_,
                dubModeEnabled_);
            if (runtime == LoopTrackRuntimeState::Armed && scheduledExecuteAtSample < 0)
            {
                cancelScheduledRecordToggleForTrack(command.trackIndex);
                track.setState(TrackState::Empty);
                break;
            }
            if (current == TrackState::Playing_DubPending && scheduledExecuteAtSample < 0)
            {
                track.setState(TrackState::Playing);
                break;
            }
            if (trn.noOp)
            {
                lastErr = trn.error;
                break;
            }
            if (trn.finalizeFirstPass)
            {
                finalizeOrDeferRecordingStop(command.trackIndex, scheduledExecuteAtSample, ctx);
                break;
            }
            if (trn.nextTrackState == TrackState::Recording)
            {
                const std::int64_t origin = scheduledExecuteAtSample >= 0 ? scheduledExecuteAtSample : ctx;
                if (track.lengthSamples() > 0)
                {
                    enterOverdubFromPlay(command.trackIndex, origin);
                    break;
                }
                beginFirstPassRecording(command.trackIndex, origin);
                break;
            }
            if (trn.nextTrackState == TrackState::Overdub)
            {
                const std::int64_t origin = scheduledExecuteAtSample >= 0 ? scheduledExecuteAtSample : ctx;
                enterOverdubFromPlay(command.trackIndex, origin);
                break;
            }
            if (trn.shouldCaptureOverdubUndo)
            {
                rememberUndoSessionBpm();
                track.captureOverdubUndoSnapshot();
            }
            track.setState(trn.nextTrackState);
            if (trn.nextTrackState == TrackState::Overdub || trn.nextTrackState == TrackState::Recording)
            {
                unmuteForCapture(command.trackIndex);
            }
            break;
        }
        case CommandType::OverdubToggle:
        {
            const auto trn = resolveLoopTrackTransition(runtimeStateForTrack(track),
                LoopTrackCommand::Overdub,
                track.lengthSamples() > 0,
                trackBehaviorMode_,
                afterRecordMode_,
                dubModeEnabled_);
            if (trn.noOp)
            {
                lastErr = trn.error;
                break;
            }
            if (trn.nextTrackState == TrackState::Overdub)
            {
                const std::int64_t origin = scheduledExecuteAtSample >= 0 ? scheduledExecuteAtSample : ctx;
                enterOverdubFromPlay(command.trackIndex, origin);
                break;
            }
            if (trn.nextTrackState == TrackState::Recording)
            {
                const std::int64_t origin = scheduledExecuteAtSample >= 0 ? scheduledExecuteAtSample : ctx;
                beginFirstPassRecording(command.trackIndex, origin);
                break;
            }
            if (trn.shouldCaptureOverdubUndo)
            {
                rememberUndoSessionBpm();
                track.captureOverdubUndoSnapshot();
            }
            track.setState(trn.nextTrackState);
            if (trn.nextTrackState == TrackState::Overdub || trn.nextTrackState == TrackState::Recording)
            {
                unmuteForCapture(command.trackIndex);
            }
            const auto tio = static_cast<std::size_t>(command.trackIndex);
            if (track.state() == TrackState::Overdub)
            {
                trackRecordInputGateTransport_[tio] =
                    scheduledExecuteAtSample >= 0 ? scheduledExecuteAtSample : transport_.samplePosition();
            }
            else
            {
                trackRecordInputGateTransport_[tio] = -1;
            }
            break;
        }
        case CommandType::Stop:
        {
            if (current == TrackState::Armed)
            {
                cancelScheduledRecordToggleForTrack(command.trackIndex);
                track.setState(TrackState::Empty);
                break;
            }
            const auto trn = resolveLoopTrackTransition(runtimeStateForTrack(track),
                LoopTrackCommand::Stop,
                track.lengthSamples() > 0,
                trackBehaviorMode_,
                afterRecordMode_,
                dubModeEnabled_);
            if (trn.noOp)
            {
                lastErr = trn.error;
                break;
            }
            if (trn.finalizeFirstPass)
            {
                finalizeOrDeferRecordingStop(command.trackIndex, scheduledExecuteAtSample, ctx);
                enterStoppedWithSpillover(command.trackIndex, scheduledExecuteAtSample);
            }
            else
            {
                track.setState(trn.nextTrackState);
                if (trn.nextTrackState == TrackState::Stopped)
                {
                    enterStoppedWithSpillover(command.trackIndex, scheduledExecuteAtSample);
                }
            }
            break;
        }
        case CommandType::MuteToggle:
            applyMuteToggle(command.trackIndex, scheduledExecuteAtSample);
            break;
        case CommandType::Play:
        {
            if (muteModeEnabled_ && !dubModeEnabled_
                && (current == TrackState::Playing || current == TrackState::Overdub || current == TrackState::Muted
                    || trackStateIsMutePending(current)))
            {
                applyMuteToggle(command.trackIndex, scheduledExecuteAtSample);
                break;
            }
            if (current == TrackState::Armed)
            {
                if (scheduledExecuteAtSample < 0)
                {
                    cancelScheduledRecordToggleForTrack(command.trackIndex);
                    track.setState(TrackState::Empty);
                    break;
                }
                const std::int64_t origin = scheduledExecuteAtSample >= 0 ? scheduledExecuteAtSample : ctx;
                beginFirstPassRecording(command.trackIndex, origin);
                break;
            }
            if (dubModeEnabled_ && (current == TrackState::Stopped || current == TrackState::Empty))
            {
                const std::int64_t origin = scheduledExecuteAtSample >= 0 ? scheduledExecuteAtSample : ctx;
                if (track.lengthSamples() == 0)
                {
                    beginFirstPassRecording(command.trackIndex, origin);
                }
                else
                {
                    enterOverdubFromPlay(command.trackIndex, origin);
                }
                break;
            }
            if ((current == TrackState::Empty || current == TrackState::Stopped) && track.lengthSamples() == 0)
            {
                const std::int64_t origin = scheduledExecuteAtSample >= 0 ? scheduledExecuteAtSample : ctx;
                beginFirstPassRecording(command.trackIndex, origin);
                break;
            }
            const auto trn = resolveLoopTrackTransition(runtimeStateForTrack(track),
                LoopTrackCommand::Play,
                track.lengthSamples() > 0,
                trackBehaviorMode_,
                afterRecordMode_,
                dubModeEnabled_);
            if (trn.finalizeFirstPass)
            {
                finalizeRecordingToPlaying(command.trackIndex, scheduledExecuteAtSample, ctx);
                break;
            }
            if (!trn.noOp)
            {
                if ((current == TrackState::Stopped || current == TrackState::Empty) && track.lengthSamples() > 0)
                {
                    const std::int64_t origin =
                        scheduledExecuteAtSample >= 0 ? scheduledExecuteAtSample : ctx;
                    adoptSharedPhaseOriginIfNoActivePeers(command.trackIndex, origin);
                    if (command.startFromLoopBeginning)
                    {
                        const auto ti = static_cast<std::size_t>(command.trackIndex);
                        trackPhaseFromStart_[ti] = true;
                        trackPhaseOrigin_[ti] = origin;
                        resetStretchForTrack(command.trackIndex);
                        trackPlayheads_[ti] = 0;
                    }
                    else
                    {
                        preparePlayStartPhase(command.trackIndex, origin);
                    }
                }
                track.setState(trn.nextTrackState);
                if (trn.nextTrackState == TrackState::Playing)
                {
                    trackRecordInputGateTransport_[static_cast<std::size_t>(command.trackIndex)] = -1;
                    if (current == TrackState::Stopped || current == TrackState::Empty)
                    {
                        if (track.mix().muted)
                        {
                            track.setState(TrackState::Muted);
                            setMuteAudible(command.trackIndex, false);
                        }
                        else
                        {
                            setMuteAudible(command.trackIndex, true);
                        }
                    }
                }
            }
            else
            {
                lastErr = trn.error;
            }
            break;
        }
        case CommandType::Clear:
        {
            const auto trn = resolveLoopTrackTransition(runtimeStateForTrack(track),
                LoopTrackCommand::Clear,
                track.lengthSamples() > 0,
                trackBehaviorMode_,
                afterRecordMode_,
                dubModeEnabled_);
            if (current == TrackState::Armed)
            {
                cancelScheduledRecordToggleForTrack(command.trackIndex);
            }
            finishRecordingToPlaying_[static_cast<std::size_t>(command.trackIndex)] = false;
            pendingRecordEndExclusive_[static_cast<std::size_t>(command.trackIndex)] = -1;
            track.setState(trn.nextTrackState);
            if (trn.clearLoop)
            {
                if (track.lengthSamples() > 1)
                {
                    rememberUndoSessionBpm();
                    track.captureOverdubUndoSnapshot();
                    track.clearLiveAudioKeepHistory();
                }
                else
                {
                    track.clearAudio();
                }
            }
            trackWriteheads_[static_cast<std::size_t>(command.trackIndex)] = 0;
            trackPlayheads_[static_cast<std::size_t>(command.trackIndex)] = 0;
            clearRecordPhaseAnchor(command.trackIndex);
            resetStretchForTrack(command.trackIndex);
            track.setLengthSamples(0);
            pendingLoopBars_[static_cast<std::size_t>(command.trackIndex)] = 0;
            reassignTrackSyncMasterAfterClear(command.trackIndex);
            {
                const auto mi = static_cast<std::size_t>(command.trackIndex);
                trackOutPeakL_[mi].store(0.0f, std::memory_order_relaxed);
                trackOutPeakR_[mi].store(0.0f, std::memory_order_relaxed);
                trackInPeakL_[mi].store(0.0f, std::memory_order_relaxed);
                trackInPeakR_[mi].store(0.0f, std::memory_order_relaxed);
                trackOutRmsL_[mi].store(0.0f, std::memory_order_relaxed);
                trackOutRmsR_[mi].store(0.0f, std::memory_order_relaxed);
                trackInRmsL_[mi].store(0.0f, std::memory_order_relaxed);
                trackInRmsR_[mi].store(0.0f, std::memory_order_relaxed);
            }
            if (waveformEnvelopes_ != nullptr)
            {
                waveformEnvelopes_->rebuildFromTrack(command.trackIndex, track);
            }
            break;
        }
        case CommandType::PlayStopToggle:
            if (playStopShouldOverdub(command.trackIndex))
            {
                if (current == TrackState::Playing_DubPending && scheduledExecuteAtSample < 0)
                    track.setState(TrackState::Playing);
                else
                    enterOverdubFromPlay(command.trackIndex,
                        scheduledExecuteAtSample >= 0 ? scheduledExecuteAtSample : ctx);
                break;
            }
            if (muteModeEnabled_ && !dubModeEnabled_
                && (current == TrackState::Playing || current == TrackState::Overdub || current == TrackState::Muted
                    || trackStateIsMutePending(current)))
            {
                applyMuteToggle(command.trackIndex, scheduledExecuteAtSample);
                break;
            }
            if (current == TrackState::Armed)
            {
                if (scheduledExecuteAtSample < 0)
                {
                    cancelScheduledRecordToggleForTrack(command.trackIndex);
                    track.setState(TrackState::Empty);
                    break;
                }
                const std::int64_t origin = scheduledExecuteAtSample >= 0 ? scheduledExecuteAtSample : ctx;
                beginFirstPassRecording(command.trackIndex, origin);
                break;
            }
            else if (current == TrackState::Recording)
            {
                finalizeRecordingToPlaying(command.trackIndex, scheduledExecuteAtSample, ctx);
            }
            else if (current == TrackState::Playing || current == TrackState::Playing_DubPending
                || current == TrackState::Playing_MutePending || current == TrackState::Muted
                || current == TrackState::Muted_UnmutePending)
            {
                enterStoppedWithSpillover(command.trackIndex, scheduledExecuteAtSample);
            }
            else if (current == TrackState::Overdub || current == TrackState::Overdubbing_MutePending)
            {
                track.setState(TrackState::Playing);
                trackRecordInputGateTransport_[static_cast<std::size_t>(command.trackIndex)] = -1;
            }
            else if (current == TrackState::Stopped || current == TrackState::Empty)
            {
                const std::int64_t origin =
                    scheduledExecuteAtSample >= 0 ? scheduledExecuteAtSample : ctx;
                if (dubModeEnabled_)
                {
                    if (track.lengthSamples() == 0)
                    {
                        beginFirstPassRecording(command.trackIndex, origin);
                    }
                    else
                    {
                        enterOverdubFromPlay(command.trackIndex, origin);
                    }
                }
                else if (track.lengthSamples() == 0)
                {
                    beginFirstPassRecording(command.trackIndex, origin);
                }
                else if ((current == TrackState::Stopped || current == TrackState::Empty)
                    && track.lengthSamples() > 0)
                {
                    adoptSharedPhaseOriginIfNoActivePeers(command.trackIndex, origin);
                    preparePlayStartPhase(command.trackIndex, origin);
                    if (track.mix().muted)
                    {
                        track.setState(TrackState::Muted);
                        setMuteAudible(command.trackIndex, false);
                    }
                    else
                    {
                        track.setState(TrackState::Playing);
                        setMuteAudible(command.trackIndex, true);
                    }
                }
            }
            break;
        case CommandType::PlayAll:
        case CommandType::StopAll:
        case CommandType::UndoOverdub:
        case CommandType::RedoOverdub:
        case CommandType::SyncAllToStart:
        case CommandType::SerialHandoff:
            break;
    }

    if (trackIsSerialExclusive(command.trackIndex)
        && (command.type == CommandType::Play || command.type == CommandType::PlayStopToggle
            || command.type == CommandType::OverdubToggle || command.type == CommandType::RecordToggle))
    {
        const std::int64_t origin = scheduledExecuteAtSample >= 0 ? scheduledExecuteAtSample : ctx;
        maybeStartSerialParallelBedAfterExclusiveStart(command.trackIndex, current, origin);
    }
}

void LooperEngine::setTrackBehaviorMode(const TrackBehaviorMode mode) noexcept
{
    trackBehaviorMode_ = mode;
}

TrackBehaviorMode LooperEngine::trackBehaviorMode() const noexcept
{
    return trackBehaviorMode_;
}

void LooperEngine::setDubModeEnabled(const bool enabled) noexcept
{
    dubModeEnabled_ = enabled;
}

bool LooperEngine::dubModeEnabled() const noexcept
{
    return dubModeEnabled_;
}

void LooperEngine::setMuteModeEnabled(const bool enabled) noexcept
{
    if (muteModeEnabled_ && !enabled)
    {
        releaseMutedOnMuteModeExit_.store(true, std::memory_order_release);
    }
    muteModeEnabled_ = enabled;
}

bool LooperEngine::muteModeEnabled() const noexcept
{
    return muteModeEnabled_;
}

void LooperEngine::setMuteAudible(const int trackIndex, const bool audible) noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return;
    }
    const auto ti = static_cast<std::size_t>(trackIndex);
    muteGainTarget_[ti] = audible ? 1.0f : 0.0f;
    auto m = tracks_[ti].mix();
    m.muted = !audible;
    tracks_[ti].setMix(m);
}

float LooperEngine::nextMuteGain(const std::size_t trackIndex) noexcept
{
    float g = muteGain_[trackIndex];
    const float tgt = muteGainTarget_[trackIndex];
    if (muteRampSamples_ <= 1)
    {
        muteGain_[trackIndex] = tgt;
        return tgt;
    }
    const float step = 1.0f / static_cast<float>(muteRampSamples_);
    if (g < tgt)
    {
        g = std::min(tgt, g + step);
    }
    else if (g > tgt)
    {
        g = std::max(tgt, g - step);
    }
    muteGain_[trackIndex] = g;
    return g;
}

void LooperEngine::releaseMutedTracksOnMuteModeExit() noexcept
{
    const auto origin = transport_.samplePosition();
    for (int i = 0; i < kMaxTracks; ++i)
    {
        auto& track = tracks_[static_cast<std::size_t>(i)];
        const auto st = track.state();
        if (trackStateIsMutePending(st) && !trackStateIsSilent(st))
        {
            cancelScheduledCommandsForTrack(i, CommandType::MuteToggle);
            track.setState(trackStateCancelMutePending(st));
            continue;
        }
        if (!trackMuted(i))
        {
            continue;
        }
        cancelScheduledCommandsForTrack(i, CommandType::MuteToggle);
        const bool stillLooping = trackStateIsSilent(st) || trackStateAdvancesPlayhead(st);
        setMuteAudible(i, true);
        if (stillLooping)
        {
            enterStoppedWithSpillover(i, origin);
        }
    }
}

void LooperEngine::applyMuteToggle(const int trackIndex, const std::int64_t scheduledExecuteAtSample) noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return;
    }
    auto& track = tracks_[static_cast<std::size_t>(trackIndex)];
    const auto current = track.state();
    const bool cancelPending = scheduledExecuteAtSample < 0 && trackStateIsMutePending(current);
    if (cancelPending)
    {
        track.setState(trackStateCancelMutePending(current));
        return;
    }
    if (current == TrackState::Playing || current == TrackState::Playing_MutePending
        || current == TrackState::Playing_DubPending || current == TrackState::Overdub
        || current == TrackState::Overdubbing_MutePending)
    {
        track.setState(TrackState::Muted);
        setMuteAudible(trackIndex, false);
        trackRecordInputGateTransport_[static_cast<std::size_t>(trackIndex)] = -1;
        return;
    }
    if (current == TrackState::Muted || current == TrackState::Muted_UnmutePending)
    {
        track.setState(TrackState::Playing);
        setMuteAudible(trackIndex, true);
        return;
    }
    if (current == TrackState::Stopped || current == TrackState::Empty)
    {
        setMuteAudible(trackIndex, track.mix().muted);
        return;
    }
}

void LooperEngine::setTrackMuted(const int trackIndex, const bool muted) noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return;
    }
    auto& tr = tracks_[static_cast<std::size_t>(trackIndex)];
    auto m = tr.mix();
    if (m.muted == muted && trackStateIsSilent(tr.state()) == muted)
    {
        return;
    }
    if (muted)
    {
        if (trackStateAdvancesPlayhead(tr.state()) && !trackStateIsSilent(tr.state()))
        {
            tr.setState(TrackState::Muted);
        }
        setMuteAudible(trackIndex, false);
    }
    else
    {
        if (trackStateIsSilent(tr.state()) || tr.state() == TrackState::Playing_MutePending)
        {
            tr.setState(TrackState::Playing);
        }
        setMuteAudible(trackIndex, true);
    }
}

void LooperEngine::toggleTrackMuted(const int trackIndex) noexcept
{
    applyMuteToggle(trackIndex, -1);
}

bool LooperEngine::handleMuteAction(const int trackIndex) noexcept
{
    return enqueueCommand({CommandType::MuteToggle, trackIndex});
}

bool LooperEngine::trackMuted(const int trackIndex) const noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return false;
    }
    return tracks_[static_cast<std::size_t>(trackIndex)].mix().muted
        || trackStateIsSilent(tracks_[static_cast<std::size_t>(trackIndex)].state());
}

bool LooperEngine::playStopShouldOverdub(const int trackIndex) const noexcept
{
    if (!dubModeEnabled_ || trackIndex < 0 || trackIndex >= kMaxTracks)
        return false;
    const auto& track = tracks_[static_cast<std::size_t>(trackIndex)];
    const auto st = track.state();
    return track.lengthSamples() > 0 && (st == TrackState::Playing || st == TrackState::Playing_DubPending
        || st == TrackState::Playing_MutePending || st == TrackState::Muted
        || st == TrackState::Muted_UnmutePending);
}

bool LooperEngine::playStopShouldMute(const int trackIndex) const noexcept
{
    if (!muteModeEnabled_ || dubModeEnabled_ || trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return false;
    }
    const auto st = tracks_[static_cast<std::size_t>(trackIndex)].state();
    return trackStateAdvancesPlayhead(st);
}

void LooperEngine::unmuteForCapture(const int trackIndex) noexcept
{
    setMuteAudible(trackIndex, true);
}

void LooperEngine::enterStoppedWithSpillover(const int trackIndex, const std::int64_t boundarySample) noexcept
{
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        return;
    }
    const auto ti = static_cast<std::size_t>(trackIndex);
    auto& track = tracks_[ti];
    stopFadeReadPos_[ti] = trackPlayheads_[ti];
    muteGainTarget_[ti] = 0.0f; // 5 ms pre-FX ramp; mix.muted is the mixer strip, left unchanged
    track.setState(TrackState::Stopped);
    trackRecordInputGateTransport_[ti] = -1;
    clearPlayStartPhase(trackIndex);
    resetStretchForTrack(trackIndex);
    const std::int64_t origin = boundarySample >= 0 ? boundarySample : transport_.samplePosition();
    adoptSharedPhaseOriginIfNoActivePeers(trackIndex, origin);
}

void LooperEngine::setSpilloverTailSeconds(const double seconds) noexcept
{
    const double clamped = seconds < 0.0 ? 0.0 : seconds;
    spilloverTailSeconds_.store(clamped, std::memory_order_relaxed);
}

double LooperEngine::spilloverTailSeconds() const noexcept
{
    const double fade = static_cast<double>(kMuteRampMilliseconds) * 0.001;
    return std::max(fade, spilloverTailSeconds_.load(std::memory_order_relaxed));
}

bool LooperEngine::anyPlayingTrackUnmuted() const noexcept
{
    for (int i = 0; i < kMaxTracks; ++i)
    {
        const auto& tr = tracks_[static_cast<std::size_t>(i)];
        const auto st = tr.state();
        if (trackStateAdvancesPlayhead(st) && !tr.mix().muted && !trackStateIsSilent(st))
        {
            return true;
        }
    }
    return false;
}

void LooperEngine::setAllPlayingTracksMuted(const bool muted) noexcept
{
    for (int i = 0; i < kMaxTracks; ++i)
    {
        if (!trackStateAdvancesPlayhead(tracks_[static_cast<std::size_t>(i)].state()))
        {
            continue;
        }
        setTrackMuted(i, muted);
    }
}

void LooperEngine::setAllTracksMuted(const bool muted) noexcept
{
    for (int i = 0; i < kMaxTracks; ++i)
    {
        setTrackMuted(i, muted);
    }
}

bool LooperEngine::allTracksMuted() const noexcept
{
    for (int i = 0; i < kMaxTracks; ++i)
    {
        if (!trackMuted(i))
        {
            return false;
        }
    }
    return true;
}

void LooperEngine::setPerTrackIsolationLock(const bool enabled) noexcept
{
    perTrackIsolationLock_ = enabled;
}

bool LooperEngine::perTrackIsolationLock() const noexcept
{
    return perTrackIsolationLock_;
}

LoopTrackDiagnostics LooperEngine::trackDiagnostics(const int trackIndex) const noexcept
{
    LoopTrackDiagnostics out{};
    if (trackIndex < 0 || trackIndex >= kMaxTracks)
    {
        out.runtimeState = LoopTrackRuntimeState::Error;
        out.lastTransitionError = TransitionError::IllegalCommand;
        return out;
    }
    const auto ti = static_cast<std::size_t>(trackIndex);
    out.runtimeState = runtimeStateForTrack(tracks_[ti]);
    out.lastTransitionError = lastTransitionError_[ti];
    for (const auto& slot : scheduledCommands_)
    {
        if (!slot.active)
        {
            continue;
        }
        if (slot.command.trackIndex == trackIndex
            || (slot.command.trackIndex < 0
                && (slot.command.type == CommandType::UndoOverdub || slot.command.type == CommandType::RedoOverdub)))
        {
            out.hasQueuedAction = true;
            out.queuedAction = slot.command.type;
            break;
        }
        if (slot.command.type == CommandType::SerialHandoff && slot.command.trackIndex != trackIndex
            && trackIsSerialExclusive(trackIndex) && trackIsSerialExclusive(slot.command.trackIndex))
        {
            const auto st = tracks_[ti].state();
            if (st == TrackState::Playing || st == TrackState::Overdub)
            {
                out.hasQueuedAction = true;
                out.queuedAction = CommandType::SerialHandoff;
                break;
            }
        }
        if (syncMode_ == SyncMode::SerialParallel && trackIndex == 0
            && trackIsSerialExclusive(slot.command.trackIndex)
            && (slot.command.type == CommandType::Play || slot.command.type == CommandType::PlayStopToggle
                || slot.command.type == CommandType::SerialHandoff))
        {
            const auto bedSt = tracks_[0].state();
            if ((bedSt == TrackState::Stopped || bedSt == TrackState::Empty) && tracks_[0].lengthSamples() > 0)
            {
                out.hasQueuedAction = true;
                out.queuedAction = CommandType::Play;
                break;
            }
        }
        if (slot.command.type == CommandType::PlayAll)
        {
            const auto st = tracks_[ti].state();
            if (tracks_[ti].lengthSamples() > 0 && (st == TrackState::Stopped || st == TrackState::Empty))
            {
                out.hasQueuedAction = true;
                out.queuedAction = CommandType::PlayAll;
                break;
            }
        }
        else if (slot.command.type == CommandType::StopAll)
        {
            const auto st = tracks_[ti].state();
            if (trackStateAdvancesPlayhead(st) || st == TrackState::Recording || st == TrackState::Armed)
            {
                out.hasQueuedAction = true;
                out.queuedAction = CommandType::StopAll;
                break;
            }
        }
    }
    return out;
}

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr int kMetroKernelCap = 512;

void mixMetroSample(const float s,
    const int sample,
    float* const* const output,
    const int numOutputChannels,
    const bool stereoRoute,
    const int routeCh,
    const bool pair) noexcept
{
    if (stereoRoute)
    {
        if (output[0] != nullptr)
        {
            output[0][sample] += s;
        }
        if (numOutputChannels >= 2 && output[1] != nullptr)
        {
            output[1][sample] += s;
        }
        return;
    }
    if (routeCh < 0 || routeCh >= numOutputChannels || output[routeCh] == nullptr)
    {
        return;
    }
    output[routeCh][sample] += s;
    if (pair && routeCh + 1 < numOutputChannels && output[routeCh + 1] != nullptr)
    {
        output[routeCh + 1][sample] += s;
    }
}

int fillBuiltinClick(const MetronomeSoundKind kind,
    const bool accent,
    float* const dst,
    const int maxCap,
    const double sampleRate) noexcept
{
    if (maxCap < 32 || sampleRate <= 1.0)
    {
        return 0;
    }

    const int n = std::clamp(static_cast<int>(std::lround(sampleRate * 0.006)), 32, maxCap);
    for (int i = 0; i < n; ++i)
    {
        const double t = static_cast<double>(i) / sampleRate;
        const double env = std::exp(-t * (accent ? 180.0 : 140.0));
        float v = 0.f;
        switch (kind)
        {
            case MetronomeSoundKind::Click:
                v = static_cast<float>(std::sin(2.0 * kPi * 1850.0 * t) * env * (accent ? 0.72f : 0.5f));
                break;
            case MetronomeSoundKind::Wood:
                v = static_cast<float>(
                    (0.62 * std::sin(2.0 * kPi * 360.0 * t) + 0.38 * std::sin(2.0 * kPi * 920.0 * t)) * env
                    * (accent ? 0.66f : 0.46f));
                break;
            case MetronomeSoundKind::Sine:
            default:
                v = static_cast<float>(
                    std::sin(2.0 * kPi * (accent ? 1050.0 : 820.0) * t) * env * (accent ? 0.58f : 0.4f));
                break;
        }
        dst[i] = v;
    }
    return n;
}
} // namespace

void LooperEngine::renderMetronome(
    const std::int64_t blockStartSample,
    const int numSamples,
    float* const* const output,
    const int numOutputChannels) noexcept
{
    if (!metronomeEnabled_ || output == nullptr || numSamples <= 0 || numOutputChannels <= 0)
    {
        metroClickResidualLen_ = 0;
        metroClickResidualOff_ = 0;
        return;
    }
    if (!anyLoopPlaying() && !metronomeForceRunning_)
    {
        metroClickResidualLen_ = 0;
        metroClickResidualOff_ = 0;
        return;
    }

    const double sr = transport_.sampleRate();
    if (sr <= 1.0)
    {
        return;
    }

    const double bpm = (blockMasterBpm_ > 1.0) ? blockMasterBpm_ : transport_.tempoBpm();
    const double spb = sr * 60.0 / bpm;
    if (spb < 1.0)
    {
        return;
    }

    const int den = timeSigDenominator_;
    const int num = timeSigNumerator_;
    const double samplesPerNotatedBeat = spb * (4.0 / static_cast<double>(den));
    if (samplesPerNotatedBeat < 2.0)
    {
        return;
    }

    const int riN = metroNormalReadIndex_.load(std::memory_order_acquire);
    const int riA = metroAccentReadIndex_.load(std::memory_order_acquire);
    const auto& custN = metroNormalCustomBuf_[static_cast<std::size_t>(riN)];
    const auto& custA = metroAccentCustomBuf_[static_cast<std::size_t>(riA)];

    float kernelBuf[kMetroKernelCap];
    const bool mixerOwned = !metronomeMixToDevice_.load(std::memory_order_relaxed);
    const bool stereoRoute = mixerOwned || metronomeOutCh_ < 0;
    const int routeCh = mixerOwned ? -1 : metronomeOutCh_;
    const bool pair = !stereoRoute && metronomeStereoPair_ && routeCh >= 0 && (routeCh + 1 < numOutputChannels)
        && output[routeCh + 1] != nullptr;

    int residualOut = 0;
    while (metroClickResidualOff_ < metroClickResidualLen_ && residualOut < numSamples)
    {
        mixMetroSample(metroClickResidual_[static_cast<std::size_t>(metroClickResidualOff_)],
            residualOut,
            output,
            numOutputChannels,
            stereoRoute,
            routeCh,
            pair);
        ++metroClickResidualOff_;
        ++residualOut;
    }
    if (metroClickResidualOff_ >= metroClickResidualLen_)
    {
        metroClickResidualLen_ = 0;
        metroClickResidualOff_ = 0;
    }

    for (int i = 0; i < numSamples; ++i)
    {
        const std::int64_t absPos = blockStartSample + static_cast<std::int64_t>(i);
        const std::int64_t relPos = absPos - sharedLoopPhaseOrigin_;
        if (relPos < 0)
        {
            continue;
        }

        const double prevBeat = static_cast<double>(relPos - 1) / samplesPerNotatedBeat;
        const double curBeat = static_cast<double>(relPos) / samplesPerNotatedBeat;
        const auto prevIdx = static_cast<std::int64_t>(std::floor(prevBeat));
        const auto curIdx = static_cast<std::int64_t>(std::floor(curBeat));
        if (curIdx <= prevIdx)
        {
            continue;
        }

        const int mod = static_cast<int>((curIdx % static_cast<std::int64_t>(num) + num) % num);
        const bool accent = mod == 0;
        const float amp = metronomeGain_ * (accent ? 1.0f : 0.85f);

        int klen = 0;
        if (accent && !custA.empty())
        {
            klen = static_cast<int>(std::min(custA.size(), static_cast<std::size_t>(kMetroKernelCap)));
            std::memcpy(kernelBuf, custA.data(), static_cast<std::size_t>(klen) * sizeof(float));
        }
        else if (!accent && !custN.empty())
        {
            klen = static_cast<int>(std::min(custN.size(), static_cast<std::size_t>(kMetroKernelCap)));
            std::memcpy(kernelBuf, custN.data(), static_cast<std::size_t>(klen) * sizeof(float));
        }
        else
        {
            klen = fillBuiltinClick(accent ? metroAccentKind_ : metroNormalKind_, accent, kernelBuf, kMetroKernelCap, sr);
        }

        if (klen <= 0)
        {
            continue;
        }

        const int rem = numSamples - i;
        const int use = std::min(klen, rem);
        for (int j = 0; j < use; ++j)
        {
            mixMetroSample(kernelBuf[j] * amp, i + j, output, numOutputChannels, stereoRoute, routeCh, pair);
        }

        const int leftover = klen - use;
        if (leftover > 0)
        {
            std::memcpy(metroClickResidual_.data(), kernelBuf + use, static_cast<std::size_t>(leftover) * sizeof(float));
            for (int j = 0; j < leftover; ++j)
            {
                metroClickResidual_[static_cast<std::size_t>(j)] *= amp;
            }
            metroClickResidualLen_ = leftover;
            metroClickResidualOff_ = 0;
        }
    }
}
} // namespace retrolooper
