// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <vector>

namespace retrolooper
{
enum class GlobalStopMode : uint8_t
{
    Immediate = 0,
    Beat,
    Bar,
    LoopEnd
};

enum class AfterRecordMode : uint8_t
{
    Play = 0,
    Dub
};

enum class CountInStyle : uint8_t
{
    Off = 0,
    Beat,
    Bar,
    Loop
};

enum class CountOutStyle : uint8_t
{
    Off = 0,
    Beat,
    Bar,
    Loop
};

enum class PlaybackStartMode : uint8_t
{
    Immediate = 0,
    Beat,
    Bar,
    /** Legacy UI "Loop" / "Loop End". Mute maps this to {@link TimingMode::LoopEnd}. */
    Loop,
    LoopBegin,
    LoopEnd
};

/** Quantize grid shared by Rec / Play / Stop / Mute deferred actions. */
enum class TimingMode : uint8_t
{
    /** Execute at sample 0 of the current audio block. */
    Immediate = 0,
    /** Count 1 of the next notated beat (metronome count). */
    Beat,
    /** Count 1 of the next notated bar (downbeat). */
    Bar,
    /** Next sample where {@code globalPhaseSample == 0}. */
    LoopBegin,
    /** Next sample where this track's read pointer is {@code length - 1}. */
    LoopEnd
};

[[nodiscard]] inline TimingMode timingModeFromPlaybackStart(const PlaybackStartMode mode, const bool loopMeansEnd) noexcept
{
    switch (mode)
    {
        case PlaybackStartMode::Immediate: return TimingMode::Immediate;
        case PlaybackStartMode::Beat: return TimingMode::Bar;
        case PlaybackStartMode::Bar: return TimingMode::Bar;
        case PlaybackStartMode::LoopBegin: return TimingMode::LoopBegin;
        case PlaybackStartMode::LoopEnd: return TimingMode::LoopEnd;
        case PlaybackStartMode::Loop: return loopMeansEnd ? TimingMode::LoopEnd : TimingMode::LoopBegin;
        default: return TimingMode::Immediate;
    }
}

/** Play / Rec / Dub / Mute "Loop": wait for the next wrap of this track's own loop. */
[[nodiscard]] inline bool playbackStartModeIsLoopCycle(const PlaybackStartMode mode) noexcept
{
    return mode == PlaybackStartMode::Loop || mode == PlaybackStartMode::LoopBegin;
}

/** Quantize unit for follower record start/stop relative to the tempo-master loop (Mobius Track Sync). */
enum class TrackSyncUnit : uint8_t
{
    Loop = 0,
    Cycle = 1,
    Subcycle = 2
};

/** Snap recorded loop length to this fraction of a notated bar (Off leaves the take as-is). */
enum class LengthQuantizeGrid : uint8_t
{
    Off = 0,
    Eighth = 1,
    Quarter = 2,
    Half = 3,
    Bar = 4
};

/** Track assignment for the master A/B crossfader. Off = always fully audible. */
enum class CrossfadeAssign : uint8_t
{
    Off = 0,
    A = 1,
    B = 2
};

/** When mixer FX power/bypass takes effect. Loop follows the longest recorded loop. */
enum class FxBypassSyncMode : uint8_t
{
    Immediate = 0,
    Bar,
    Loop
};

/** After a Sound Crate quick-import (T1–T4), whether that track starts playing. */
enum class SoundCrateImportAutostart : uint8_t
{
    None = 0,
    Bar,
    Loop
};

[[nodiscard]] inline SoundCrateImportAutostart soundCrateImportAutostartFromInt(const int v) noexcept
{
    if (v == static_cast<int>(SoundCrateImportAutostart::Bar))
    {
        return SoundCrateImportAutostart::Bar;
    }
    if (v == static_cast<int>(SoundCrateImportAutostart::Loop))
    {
        return SoundCrateImportAutostart::Loop;
    }
    return SoundCrateImportAutostart::None;
}

[[nodiscard]] inline PlaybackStartMode withoutBeatTiming(const PlaybackStartMode mode) noexcept
{
    return mode == PlaybackStartMode::Beat ? PlaybackStartMode::Bar : mode;
}

[[nodiscard]] inline GlobalStopMode withoutBeatTiming(const GlobalStopMode mode) noexcept
{
    return mode == GlobalStopMode::Beat ? GlobalStopMode::Bar : mode;
}

[[nodiscard]] inline CountInStyle withoutBeatTiming(const CountInStyle style) noexcept
{
    return style == CountInStyle::Beat ? CountInStyle::Bar : style;
}

[[nodiscard]] inline CountOutStyle withoutBeatTiming(const CountOutStyle style) noexcept
{
    return style == CountOutStyle::Beat ? CountOutStyle::Bar : style;
}

/** How the transport beat counter is shown in the UI. */
enum class BeatCountDisplay : uint8_t
{
    /** 1 … N within each bar (N = time signature numerator). */
    AllBeats = 0,
    /** Show beat number only on downbeats; weak beats show ".". */
    DownbeatsOnly,
    /** Eighth-note subdivision labels (1 & 2 & …) when denominator allows; else same as AllBeats. */
    EighthSubdivision
};

/** Built-in metronome timbre when no custom sample is loaded. */
enum class MetronomeSoundKind : uint8_t
{
    Click = 0,
    Wood,
    Sine
};

struct TrackLoopPrefs
{
    uint8_t stopMode = static_cast<uint8_t>(GlobalStopMode::Bar);
    uint8_t playMode = static_cast<uint8_t>(PlaybackStartMode::Bar);
    /** When to begin first-pass record relative to transport (same values as {@link PlaybackStartMode}). Default Bar. */
    uint8_t recordMode = static_cast<uint8_t>(PlaybackStartMode::Bar);
    /** When to begin overdub relative to transport (same values as {@link PlaybackStartMode}). Default Bar. */
    uint8_t overdubMode = static_cast<uint8_t>(PlaybackStartMode::Bar);
    /** When to mute/unmute relative to transport (same values as {@link PlaybackStartMode}). Default Bar. */
    uint8_t muteMode = static_cast<uint8_t>(PlaybackStartMode::Bar);
    /** First pass ends in Play or Overdub when {@link overrideGlobal} is set. */
    uint8_t afterRecordMode = static_cast<uint8_t>(AfterRecordMode::Play);
    /** When true, this track ignores live global stop/play/rec/overdub quantize and uses the fields above. */
    bool overrideGlobal = false;
    /** 0–7 explicit swatch; 255 = theme default for this track index. */
    uint8_t colorSwatch = 255;
};

inline void withoutBeatTiming(TrackLoopPrefs& prefs) noexcept
{
    prefs.stopMode = static_cast<uint8_t>(withoutBeatTiming(static_cast<GlobalStopMode>(prefs.stopMode)));
    prefs.playMode = static_cast<uint8_t>(withoutBeatTiming(static_cast<PlaybackStartMode>(prefs.playMode)));
    prefs.recordMode = static_cast<uint8_t>(withoutBeatTiming(static_cast<PlaybackStartMode>(prefs.recordMode)));
    prefs.overdubMode = static_cast<uint8_t>(withoutBeatTiming(static_cast<PlaybackStartMode>(prefs.overdubMode)));
    prefs.muteMode = static_cast<uint8_t>(withoutBeatTiming(static_cast<PlaybackStartMode>(prefs.muteMode)));
}

/**
 * Per-track loop display (view-only; same engine for all modes).
 * Persisted as 0…2. Append new modes so saved sessions keep their value.
 */
enum class TrackVisualMode : std::uint8_t
{
    Waveform = 0,
    OutputMeter = 1,
    Decks = 2
};

/** What feeds the recorder for this track. */
enum class TrackRecordInput : uint8_t
{
    /** Live audio device channels (see {@code TrackIoRouting::hardwareInputStartChannel}). */
    Hardware = 0,
    /** Hosted VST instrument strip ({@code TrackIoRouting::recordSourceIndex}). */
    InstrumentPlugin = 1,
    /** Mixer input strip after its channel processing. */
    MixerInput = 2,
    /** Mixer bus (inputs/instruments already sent to that bus this block; sample-aligned with the click). */
    MixerBus = 3,
    /** Built-in Pixel Beat instrument strip (same callback as the click; no hardware round-trip). */
    PixelBeat = 4,
    /** Nothing feeds this track's recorder. */
    None = 5
};

/** One or two device channels for hardware I/O, or summed mono for a single channel. */
enum class TrackIoChannelMode : uint8_t
{
    Mono = 0,
    Stereo = 1
};

/** Per-track hardware / instrument routing (UI maps combos to these fields). */
struct TrackIoRouting
{
    TrackRecordInput recordInput = TrackRecordInput::Hardware;
    TrackIoChannelMode recordMode = TrackIoChannelMode::Stereo;
    /** First hardware input channel (0-based). Stereo uses {@code start} and {@code start+1}. */
    std::uint16_t hardwareInputStartChannel = 0;
    /** Instrument / mixer-input / mixer-bus strip index (0-based). */
    std::uint16_t recordSourceIndex = 0;

    TrackIoChannelMode playbackMode = TrackIoChannelMode::Stereo;
    /** First hardware output channel; {@code kUseMainStereoBus} is unused when the mixer graph is driving playback. */
    static constexpr std::uint16_t kUseMainStereoBus = 0xFFFF;
    /** Mixer OUTPUT = None: do not write this track to any device channels. */
    static constexpr std::uint16_t kSilentOutput = 0xFFFE;
    std::uint16_t hardwareOutputStartChannel = kUseMainStereoBus;
};

enum class ImportContentType : uint8_t
{
    Unknown = 0,
    RhythmicLoop,
    StronglyPercussiveLoop,
    MelodicHarmonicLoop,
    AmbientTexture,
    OneShotOrNonLoop
};

/** Result of loop-aware tempo analysis for imported audio. */
struct TempoDetectionResult
{
    /** BPM implied by sample length and musical bar assumptions. */
    double intrinsicBpm = 0.0;
    /** Main detected BPM candidate after ranking + normalization. */
    double detectedBpm = 0.0;
    /** Ranked alternatives (first element should match detectedBpm when present). */
    std::vector<double> candidateBpms{};
    /** 0..1 confidence of detection reliability. */
    double confidence = 0.0;
    /** Optional bar count hypotheses (1/2/4/8 etc). */
    std::vector<int> estimatedBars{};
    double estimatedLoopLengthSeconds = 0.0;
    bool isLikelyLoop = false;
    ImportContentType contentType = ImportContentType::Unknown;
    double halfTimeCandidate = 0.0;
    double doubleTimeCandidate = 0.0;
    /** Trimmed analysis boundaries in source time (seconds). */
    double detectedStartSeconds = 0.0;
    double detectedEndSeconds = 0.0;
    /** Aggregate rhythmic quality metric for UI messaging. */
    double loopQuality = 0.0;
    /** Keep compatibility with existing call sites that expect a bool gate. */
    bool estimateUsable = false;
};
} // namespace retrolooper
