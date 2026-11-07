// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "EngineTypes.h"
#include "BoundedCommandQueue.h"
#include "LoopTrackStateMachine.h"
#include "metering/PeakMeter.h"
#include "QuantizeScheduler.h"
#include "RealtimeTransientDetector.h"
#include "TempoManager.h"
#include "Track.h"
#include "TransportClock.h"
#include "WaveformEnvelopeBank.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace retrolooper
{
struct TrackTimeStretcher;
enum class SyncMode
{
    Free,
    Serial,
    Synced,
    /** Serial handoff for tracks 2–4; track 1 keeps playing with the active serial track. */
    SerialParallel
};

enum class CommandType
{
    RecordToggle,
    OverdubToggle,
    Stop,
    Play,
    Clear,
    /** `trackIndex` -1 restores all tracks that have an overdub snapshot; else one track. */
    UndoOverdub,
    /** One control: play if stopped (with loop), stop if playing/overdub; end record if recording. */
    PlayStopToggle,
    /** Redo the last undone overdub snapshot for one/all tracks (trackIndex < 0 means all). */
    RedoOverdub,
    /** Align all loop phases to the current transport instant (audio-thread execution). */
    SyncAllToStart,
    /**
     * Serial / Serial-Parallel: stop competing Playing/Overdub tracks at
     * {@code scheduledExecuteAtSample}, then start {@code trackIndex} if it has loop audio.
     * Internal use (scheduled from Play / PlayStopToggle). Serial-Parallel leaves track 1 running.
     */
    SerialHandoff,
    /** Start stopped tracks that have loop audio (Play All quantize). Serial / Serial-Parallel
        resume the last Stop All playing set when one was captured. If anything is already
        playing, recording, overdubbing, or armed, this is treated as StopAll. */
    PlayAll,
    /** Stop every playing, recording, overdubbing, or armed track (Stop All quantize). */
    StopAll,
    /** Toggle track mute at mute quantize (Immediate / Beat / Bar / Loop end). */
    MuteToggle,
    MuteAllToggle,
    ClearAll
};

struct EngineCommand
{
    CommandType type = CommandType::Play;
    int trackIndex = 0;
    /** Skip Play All / Stop All quantize (and Play All → Stop All toggle). Used by Link start/stop. */
    bool immediate = false;
    /** If >= 0, execute at this transport sample instead of computing quantize. */
    std::int64_t executeAtSample = -1;
    /** Play from sample 0 at execute time (crate autostart). Leaves global play-start mode alone. */
    bool startFromLoopBeginning = false;
    /** Global mute/clear scope: 0 means every engine track; plug-in uses its four visible tracks. */
    int trackCount = 0;
};

struct ScheduledCommand
{
    int playModeContext{-1}; // -1: explicit action; 0: transport, 1: mute, 2: record.
    EngineCommand command{};
    std::int64_t executeAtSample = 0;
    bool active = false;
};

struct LoopTrackDiagnostics
{
    LoopTrackRuntimeState runtimeState = LoopTrackRuntimeState::Empty;
    bool hasQueuedAction = false;
    CommandType queuedAction = CommandType::Play;
    TransitionError lastTransitionError = TransitionError::None;
};

class LooperEngine
{
  public:
    static constexpr int kMaxTracks = 8;
    static constexpr int kCommandQueueSize = 256;

    LooperEngine();
    ~LooperEngine();

    /** @param resetTransport when false, only updates sample rate (e.g. device reconfigured mid-session). */
    void prepare(double sampleRate, bool resetTransport = true);
    /** Grow realtime scratch buffers off the audio thread (device start / block-size change). */
    void ensureAudioScratchCapacity(int maxSamples) noexcept;
    /**
     * @param hardwareInput device capture channels (may be nullptr if count 0).
     * @param instrumentInputLeft/right optional stereo (or mono: right may equal left) from a hosted instrument;
     *        {@code numInstrumentChannels} 0 skips, 1 uses left only, 2 uses L+R.
     * @param trackRecordInput optional 8-channel table (track t uses channels 2t / 2t+1) resolved by the host
     *        from hardware, mixer inputs, busses, or instruments. When present, it overrides hw/inst per track.
     */
    void processBlock(const float* const* hardwareInput,
        int numHardwareInputChannels,
        const float* instrumentInputLeft,
        const float* instrumentInputRight,
        int numInstrumentChannels,
        float* const* output,
        int numSamples,
        int numOutputChannels,
        const float* const* trackRecordInput = nullptr,
        int numTrackRecordChannels = 0);

    void processBlock(const float* const* input, float* const* output, int numSamples, int numInputChannels, int numOutputChannels)
    {
        processBlock(input, numInputChannels, nullptr, nullptr, 0, output, numSamples, numOutputChannels);
    }

    void processBlock(int numSamples);

    bool enqueueCommand(const EngineCommand& command) noexcept;
    [[nodiscard]] std::uint64_t rejectedCommandCount() const noexcept { return commandQueue_.rejected(); }
    /** Message thread: clear UI command queue and quantize scheduler slots (safe before audio starts). */
    void resetStartupCommandState() noexcept;
    /**
     * Set master tempo. When {@code preservePhase} is true (default), musical beat
     * phase is preserved on the next audio callback so live time-stretch can change
     * ratio without seeking. First-take tempo (the take that defines session BPM)
     * must pass false — rebasing origin as if the take happened at the old BPM
     * leaves playback mid-loop.
     */
    void setTempo(double bpm, bool preservePhase = true) noexcept;
    /**
     * Tempo the user typed, tapped, or moved. Sets the BPM and locks the session
     * so the readout keeps that number and a later loop detection cannot replace it.
     */
    void commitUserTempo(double bpm, bool preservePhase = true) noexcept;
    /** Live-loop length/onset tempo — fractions kept. Manual/MIDI/tap still use setTempo. */
    void setDetectedTempo(double bpm, bool preservePhase = true) noexcept;
    /** Restore saved BPM and its ownership after loading track metadata, off the audio callback. */
    void restoreTempoState(double bpm, bool locked, bool userCommitted) noexcept;
    /**
     * Slave looping transport to an external timeline (Ableton Link).
     * Always applies tempo (with 0.2 BPM hysteresis). Only rewrites the shared loop
     * phase origin when {@code snapPhaseOrigin} is true (Link enable, incoming start,
     * or after a stop) so stretch is not forced to seek every audio block.
     */
    void applyExternalBeatClock(double tempoBpm, double phaseBeats, double quantumBeats,
        bool snapPhaseOrigin = false) noexcept;
    /** Audio thread only: host start/stop or locate. Cancels obsolete cues and aligns playback
        to absolute quarter notes, including multi-bar loops and negative pre-roll. */
    /** Exact host BPM (no integer rounding or network-clock hysteresis), audio thread only. */
    void commitExternalTempo(double bpm) noexcept;
    void followExternalTransport(bool playing,double quarterNotePosition,bool transportEdge) noexcept;
    [[nodiscard]] TempoSyncState tempoSyncState() const noexcept;
    [[nodiscard]] bool tempoIsLocked() const noexcept;
    /** True after the user typed, tapped, or moved the tempo. A loop detection does not set this. */
    [[nodiscard]] bool tempoWasUserCommitted() const noexcept { return userTempoCommitted_; }
    /** True while any track is in first-pass Recording (audio-thread atomic). */
    [[nodiscard]] bool isRecording() const noexcept;
    /** Monotonic audio-thread event: a new recording/overdub actually began. */
    [[nodiscard]] std::uint64_t captureStartSerial() const noexcept { return captureStartSerial_.load(std::memory_order_relaxed); }
    /**
     * True once a loop length is locked (not first-take Recording). Audio thread publishes;
     * UI reads without locking.
     */
    [[nodiscard]] bool loopIsClosed() const noexcept;
    [[nodiscard]] const std::atomic<bool>& loopIsClosedFlag() const noexcept { return loopIsClosed_; }
    [[nodiscard]] const std::atomic<std::uint64_t>& viewLoopSamplesFlag() const noexcept { return viewLoopSamples_; }
    [[nodiscard]] const std::atomic<std::uint64_t>& viewPlayheadSamplesFlag() const noexcept
    {
        return viewPlayheadSamples_;
    }
    void setCountInBars(int bars) noexcept;
    void setSyncMode(SyncMode mode) noexcept;
    /** Follower first-take record start/stop snap to this unit of the tempo-master loop (Synced mode). */
    void setTrackSyncUnit(TrackSyncUnit unit) noexcept;
    [[nodiscard]] TrackSyncUnit trackSyncUnit() const noexcept;
    /** Subcycle divisions of one master cycle when {@link TrackSyncUnit::Subcycle} is selected (1–32). */
    void setSubcycles(int n) noexcept;
    [[nodiscard]] int subcycles() const noexcept;
    /** Track whose loop defines session tempo and Track Sync; -1 if none. */
    [[nodiscard]] int trackSyncMasterIndex() const noexcept;
    void setQuantizeDivision(int beatsPerAction) noexcept;
    bool restoreTrack(int trackIndex, TrackState state, std::size_t lengthSamples) noexcept;

    /**
     * Sets global BPM from this track's cycle length and beats-per-cycle (duration math, octave-folded to 40–240).
     * Called for the session tempo master (first live loop).
     */
    void alignTempoToTrackLoop(int trackIndex) noexcept;

    /**
     * Replace track audio from interleaved file data (1 or 2 channels). Sets {@code Stopped} if successful.
     * Unless {@code deferTempoAssignment} is true, applies tempo / reference BPM like a finished recording.
     */
    bool importAudioFile(int trackIndex,
        const float* const* channelData,
        int numChannels,
        std::size_t numFrames,
        bool deferTempoAssignment = false) noexcept;

    /**
     * Copy the locked loop ({@link Track::lengthSamples}, not allocated capacity) for stem export.
     * Safe from a background thread: waits out {@link Track::clearAudio} / resize on that track.
     */
    bool copyTrackStem(int trackIndex, std::vector<float>& left, std::vector<float>& right) const;

    /** Inspect imported audio for intrinsic / estimated BPM (call after {@code importAudioFile(..., true)}). */
    [[nodiscard]] TempoDetectionResult analyzeImportTempo(int trackIndex) const noexcept;

    /**
     * Apply user choices after import: optionally match session tempo, and/or set reference BPM for time-stretch.
     * If both flags are false, reference BPM follows the current master tempo (no stretch at current BPM).
     * {@code skipOnsetDetection}: do not run {@link analyzeImportTempo}; use override BPM or loop-length intrinsic tempo.
     */
    void applyImportTempoDecisions(int trackIndex,
        bool alignSessionTempoToDetected,
        bool enableStretchToMaster,
        double overrideContentBpm = 0.0,
        bool treatAsOneShot = false,
        bool skipOnsetDetection = false) noexcept;

    /**
     * Shorten or restore the loop by whole notated bars (session tempo / time signature).
     * Negative delta trims the end; positive delta restores previously trimmed audio still in the buffer.
     * @return true if the loop length changed.
     */
    bool trimLoopByBars(int trackIndex, int deltaBars) noexcept;
    /**
     * Whole notated bars for this loop (rounded; always >= 1 when the track has audio).
     * Empty tracks return a typed pending bar count, or 0.
     */
    [[nodiscard]] int loopWholeBarCount(int trackIndex) const noexcept;
    /**
     * Set loop length to an integer bar count at the session tempo / time signature.
     * Growing repeats existing audio to fill; shrinking keeps the start of the loop.
     * On an empty track, stores the bar count until the next take (used when tempo is detected).
     * @return true if the loop length or pending bar count changed.
     */
    bool rescaleLoopToBarCount(int trackIndex, int newBars) noexcept;
    /**
     * Reverse the locked loop on the message thread. Captures an undo snapshot.
     * No-op while recording, armed, or overdubbing.
     * @return true if the audio changed.
     */
    bool reverseTrack(int trackIndex) noexcept;
    [[nodiscard]] int pendingLoopBars(int trackIndex) const noexcept;
    [[nodiscard]] std::size_t samplesPerNotatedBar() const noexcept;

    /** True when this track is the only one with loop audio. */
    [[nodiscard]] bool trackIsSoleLoop(int trackIndex) const noexcept;
    /** True when this track is the session tempo master (first loop that defined BPM). */
    [[nodiscard]] bool shouldSetTempoFromThisTrack(int trackIndex) const noexcept;

    void setTrackMix(int trackIndex, const TrackMix& mix) noexcept;
    /** Change playback levels without altering transport mute/stop fades. */
    void setTrackPlaybackLevels(int trackIndex, float volume, float pan) noexcept;
    void setTrackName(int trackIndex, std::string name);
    /** Restore cycle metadata alongside imported host-session audio. Caller excludes processing. */
    void restoreTrackCycleMetadata(int trackIndex, int cycles, int beats) noexcept;
    [[nodiscard]] std::string trackName(int trackIndex) const;
    void setTrackLoopPrefs(int trackIndex, TrackLoopPrefs prefs) noexcept;
    /** BPM stored with the loop for master-tempo time-stretch (typically set when recording/import finishes). */
    void setTrackReferenceBpm(int trackIndex, double bpm) noexcept;
    void setTrackIoRouting(int trackIndex, TrackIoRouting routing) noexcept;
    [[nodiscard]] TrackIoRouting trackIoRouting(int trackIndex) const noexcept;
    /** Visual / UI playhead. Follows this track's playback origin while advancing;
        parked at 0 when Stopped so waveform cursors do not keep crawling. */
    [[nodiscard]] std::size_t trackLoopPositionSamples(int trackIndex) const noexcept;

    void setMasterGain(float gain01) noexcept;
    [[nodiscard]] float masterGain() const noexcept;
    /** When true, processBlock leaves device gain alone so the mixer can apply it after the graph. */
    void setDeferDeviceMasterGain(bool on) noexcept;
    [[nodiscard]] bool deferDeviceMasterGain() const noexcept;
    /** Smoothed master fader onto device (and optional meter) buffers. Audio thread only. */
    void applyMasterGainToOutputs(float* const* output, int numOutputChannels, int numSamples,
        float* meterLeft = nullptr, float* meterRight = nullptr) noexcept;

    /** 0 = full A, 1 = full B. Ignored until at least one track is assigned A or B. */
    void setCrossfaderPosition(float pos01) noexcept;
    [[nodiscard]] float crossfaderPosition() const noexcept;
    void setTrackCrossfadeAssign(int trackIndex, CrossfadeAssign assign) noexcept;
    void toggleTrackCrossfadeAssign(int trackIndex, CrossfadeAssign side) noexcept;
    [[nodiscard]] CrossfadeAssign trackCrossfadeAssign(int trackIndex) const noexcept;
    [[nodiscard]] bool crossfaderIsEngaged() const noexcept;
    /** Equal-power gain for this track (1 if the crossfader is bypassed). */
    [[nodiscard]] float trackCrossfadeGain(int trackIndex) const noexcept;

    void setFreeMode(bool on) noexcept;
    [[nodiscard]] bool freeMode() const noexcept;

    void setGlobalStopMode(GlobalStopMode mode) noexcept;
    [[nodiscard]] GlobalStopMode globalStopMode() const noexcept;
    void setAfterRecordMode(AfterRecordMode mode) noexcept;
    [[nodiscard]] AfterRecordMode afterRecordMode() const noexcept;
    void setCountInStyle(CountInStyle style) noexcept;
    [[nodiscard]] CountInStyle countInStyle() const noexcept;
    void setCountOutStyle(CountOutStyle style) noexcept;
    [[nodiscard]] CountOutStyle countOutStyle() const noexcept;
    void setPlaybackStartMode(PlaybackStartMode mode) noexcept;
    [[nodiscard]] PlaybackStartMode playbackStartMode() const noexcept;
    /**
     * Play only: when on, a stopped loop joins the shared clock (current phase).
     * When off and Play Start is Bar, it waits for the next bar and starts at sample 0.
     */
    void setPlayPhaseLockEnabled(bool on) noexcept;
    [[nodiscard]] bool playPhaseLockEnabled() const noexcept;
    void setPlayAllMode(PlaybackStartMode mode) noexcept;
    [[nodiscard]] PlaybackStartMode playAllMode() const noexcept;
    void setStopAllMode(GlobalStopMode mode) noexcept;
    [[nodiscard]] GlobalStopMode stopAllMode() const noexcept;
    void setRecordStartMode(PlaybackStartMode mode) noexcept;
    [[nodiscard]] PlaybackStartMode recordStartMode() const noexcept;
    void setOverdubStartMode(PlaybackStartMode mode) noexcept;
    [[nodiscard]] PlaybackStartMode overdubStartMode() const noexcept;
    void setMuteStartMode(PlaybackStartMode mode) noexcept;
    [[nodiscard]] PlaybackStartMode muteStartMode() const noexcept;
    void setUndoStartMode(PlaybackStartMode mode) noexcept;
    [[nodiscard]] PlaybackStartMode undoStartMode() const noexcept;
    void setFxBypassSyncMode(FxBypassSyncMode mode) noexcept;
    [[nodiscard]] FxBypassSyncMode fxBypassSyncMode() const noexcept;
    [[nodiscard]] std::int64_t fxBypassExecuteSample(FxBypassSyncMode mode, std::int64_t pos = -1) const noexcept;
    [[nodiscard]] std::int64_t quantizedPlayExecuteSample(int trackIndex, PlaybackStartMode mode, std::int64_t atSample = -1) const noexcept;
    void setGlobalBarLengthBars(int bars) noexcept;
    [[nodiscard]] int globalBarLengthBars() const noexcept;
    void setLengthQuantizeGrid(LengthQuantizeGrid grid) noexcept;
    [[nodiscard]] LengthQuantizeGrid lengthQuantizeGrid() const noexcept;

    /** Notated time signature (denominator is a power of two: 1, 2, 4, 8, 16). BPM remains quarter-note based. */
    void setTimeSignature(int numerator, int denominator) noexcept;
    [[nodiscard]] int timeSigNumerator() const noexcept;
    [[nodiscard]] int timeSigDenominator() const noexcept;
    /** Length of one notated measure in quarter notes (e.g. 4/4 → 4.0, 7/8 → 3.5). */
    [[nodiscard]] double quartersPerNotatedBar() const noexcept;

    void setBeatCountDisplay(BeatCountDisplay mode) noexcept;
    [[nodiscard]] BeatCountDisplay beatCountDisplay() const noexcept;

    void setMetronomeEnabled(bool on) noexcept;
    [[nodiscard]] bool metronomeEnabled() const noexcept;
    /** When false, click is rendered to {@code metronomeMixLeft/Right} instead of hardware outputs. */
    void setMetronomeMixToDevice(bool on) noexcept;
    [[nodiscard]] const float* metronomeMixLeft() const noexcept;
    [[nodiscard]] const float* metronomeMixRight() const noexcept;
    [[nodiscard]] int metronomeMixFrames() const noexcept;
    /** True if any track is Playing or Overdub with a loop loaded. */
    [[nodiscard]] bool anyLoopPlaying() const noexcept;
    /** True if Play All / Stop All should stop: any Playing, Recording, Overdub, Armed, or empty click. */
    [[nodiscard]] bool playStopAllShouldStop() const noexcept;
    /** True if a scheduled (quantized) command of this type is waiting to fire. */
    [[nodiscard]] bool hasQueuedCommandType(CommandType type) const noexcept;
    /** Stop all tracks and the empty-session click without changing Click on/off. */
    void forceIdleClock() noexcept;
    /** True if any track has loop audio loaded. */
    [[nodiscard]] bool anyLoopHasContent() const noexcept;
    /** Overdub keep-gain for existing loop audio (0 = replace, 1 = full additive). */
    void setOverdubFeedback(float gain01) noexcept;
    [[nodiscard]] float overdubFeedback() const noexcept;
    /** Playback wrap crossfade length in milliseconds (0 = off). No extra I/O latency. */
    void setLoopCrossfadeMilliseconds(float ms) noexcept;
    [[nodiscard]] float loopCrossfadeMilliseconds() const noexcept;
    /** Shift recorded/overdubbed audio earlier by this many samples so it lines up with the heard click. */
    /** Samples to rotate a finished take earlier (positive) so it lines up with the click.
        Negative values rotate later (Ableton-style driver-error trim). */
    void setRecordLatencyCompensationSamples(int samples) noexcept;
    [[nodiscard]] int recordLatencyCompensationSamples() const noexcept;
    /** Pre-reserve live record slabs on the message / device-start thread so Rec does not allocate. */
    static constexpr int kMaxFirstPassSeconds = 600;
    void prepareLiveRecordHeadroom() noexcept;
    void setRealtimeActive(bool active) noexcept { for(auto& track:tracks_) track.setRealtimeActive(active); }
    // Caller briefly excludes processing while pinning page tables, then copies off-thread.
    bool beginStateSnapshot() noexcept {
        for(std::size_t i=0;i<tracks_.size();++i) if(!tracks_[i].beginStateSnapshot()) {
            while(i>0) tracks_[--i].endStateSnapshot(); return false;
        }
        return true;
    }
    std::size_t stateSnapshotFrames(int track) const noexcept { return tracks_[static_cast<std::size_t>(track)].stateSnapshotFrames(); }
    bool copyStateSnapshot(int track,std::vector<float>& left,std::vector<float>& right) const {
        return tracks_[static_cast<std::size_t>(track)].copyStateSnapshot(left,right);
    }
    void endStateSnapshot() const noexcept { for(const auto& track:tracks_)track.endStateSnapshot(); }
    bool consumeRecordLimitReached() noexcept { return recordLimitReached_.exchange(false); }
    /** Message thread. Reserve undo copies for loops that already have a length. */
    void reserveOverdubHistoryForKnownLoops() noexcept;
    /** Same-block Rec + Pixel Beat pad: first-pass punch-in waits until this offset so the hit is sample 0. */
    void hintFirstPassRecordOffset(int offsetInBlock) noexcept;
    /** When Play All is pressed with every track empty, start the click if it is enabled. */
    void setPlayAllStartsClickWhenEmpty(bool on) noexcept;
    [[nodiscard]] bool playAllStartsClickWhenEmpty() const noexcept;
    /** When on, Play All / Stop All execute in the current audio block (Link start/stop). */
    void setLinkTransportImmediate(bool on) noexcept;
    void setLinkQuantumLaunchSample(std::int64_t transportSample) noexcept;
    /** Click is running from Play All while no loops are playing. */
    [[nodiscard]] bool metronomeForceRunning() const noexcept;
    void setTrackDirectMonitor(int trackIndex, bool on) noexcept;
    void toggleTrackDirectMonitor(int trackIndex) noexcept;
    [[nodiscard]] bool trackDirectMonitor(int trackIndex) const noexcept;
    void setMetronomeGain(float gain01) noexcept;
    [[nodiscard]] float metronomeGain() const noexcept;
    /** -1: mix into stereo bus (channels 0–1). Else click on physical outputs (see {@code setMetronomeStereoPair}). */
    void setMetronomeOutputChannel(int channelIndex) noexcept;
    [[nodiscard]] int metronomeOutputChannel() const noexcept;
    /** When true and {@code metronomeOutputChannel()} >= 0, click is sent to that channel and the next (stereo pair). */
    void setMetronomeStereoPair(bool on) noexcept;
    [[nodiscard]] bool metronomeStereoPair() const noexcept;
    void setMetronomeNormalSound(MetronomeSoundKind k) noexcept;
    void setMetronomeAccentSound(MetronomeSoundKind k) noexcept;
    [[nodiscard]] MetronomeSoundKind metronomeNormalSound() const noexcept;
    [[nodiscard]] MetronomeSoundKind metronomeAccentSound() const noexcept;
    /** Mono samples played for a normal-beat click when non-empty (overrides built-in for that voice). */
    void setMetronomeNormalCustom(std::vector<float> samples) noexcept;
    /** Mono samples for accented (down) beats when non-empty. */
    void setMetronomeAccentCustom(std::vector<float> samples) noexcept;

    void applyGlobalDefaultsToAllTracks(bool forceIncludingOverrides = false) noexcept;
    void syncAllTracksToStart() noexcept;

    /** Sets the shared loop phase anchor for all tracks (index ignored; bounds-checked). */
    void setTrackLoopPhaseOrigin(int trackIndex, std::int64_t originSamples) noexcept;
    [[nodiscard]] std::int64_t trackLoopPhaseOrigin(int trackIndex) const noexcept;

    void setMultiTrackRecord(bool on) noexcept;
    [[nodiscard]] bool multiTrackRecord() const noexcept;
    [[nodiscard]] bool canUndoOverdubAny() const noexcept;
    [[nodiscard]] bool canRedoOverdubAny() const noexcept;

    [[nodiscard]] float lastInputPeak() const noexcept;
    [[nodiscard]] float lastOutputPeak() const noexcept;
    /**
     * Raw per-block sample-peak (max abs) for UI: input while recording/overdub, loop output while playing.
     * Ballistics / dB conversion belong on the message thread (see ui/metering/TrackMeterBallistics.h).
     */
    [[nodiscard]] float trackOutputMeter(int trackIndex) const noexcept;
    /** Full per-channel snapshot (linear peaks + block RMS). Audio thread publishes once per block. */
    [[nodiscard]] metering::TrackMeterRtSnapshot trackMeterSnapshot(int trackIndex) const noexcept;
    /** Raise input peaks to at least these values (pre-fader mixer source on the audio thread). */
    void overlayTrackInputPeaks(int trackIndex, float peakL, float peakR) noexcept;
    /** Lock-free 256-bin loop envelope published on the audio thread (UI copies on the timer). */
    void copyWaveformEnvelope(int trackIndex, float* dest, int n) const noexcept;
    void copyWaveformEnvelopeMinMax(int trackIndex, float* mins, float* maxs, int n) const noexcept;
    [[nodiscard]] std::uint32_t waveformEnvelopeEpoch(int trackIndex) const noexcept;

    /**
     * Optional stereo insert after fader, pan, and looper mute/stop gain, before the hardware bus.
     * Time-based FX (delay/reverb) belong here so Mute/Stop can feed silence while tails ring.
     * Only invoked for track indices 0–3 when set. Runs on the audio thread; keep realtime-safe.
     */
    using TrackStereoBusProcessFn = std::function<void(int trackIndex, float* l, float* r, int numSamples)>;
    void setTrackStereoBusProcessor(TrackStereoBusProcessFn fn) noexcept;

    [[nodiscard]] const std::array<Track, kMaxTracks>& tracks() const noexcept;
    [[nodiscard]] const TransportClock& transport() const noexcept;
    [[nodiscard]] std::int64_t sharedLoopPhaseOrigin() const noexcept { return sharedLoopPhaseOrigin_; }
    /**
     * Downbeat the playing loops are actually on. The longest running loop when
     * one is playing, otherwise the shared clock.
     */
    [[nodiscard]] std::int64_t audiblePhaseOrigin() const noexcept;
    /**
     * Message-thread musical clock: last published shared origin plus transport sample,
     * extrapolated while the master clock is running so beat-synced UI stays smooth.
     */
    [[nodiscard]] std::int64_t uiSharedLoopPhaseOrigin() const noexcept;
    [[nodiscard]] std::int64_t uiMusicalTransportSample() const noexcept;
    [[nodiscard]] int countInBars() const noexcept;
    [[nodiscard]] int quantizeDivision() const noexcept;
    [[nodiscard]] SyncMode syncMode() const noexcept;
    void setTrackBehaviorMode(TrackBehaviorMode mode) noexcept;
    [[nodiscard]] TrackBehaviorMode trackBehaviorMode() const noexcept;
    void setDubModeEnabled(bool enabled) noexcept;
    [[nodiscard]] bool dubModeEnabled() const noexcept;
    void setMuteModeEnabled(bool enabled) noexcept;
    [[nodiscard]] bool muteModeEnabled() const noexcept;
    void setTrackMuted(int trackIndex, bool muted) noexcept;
    void toggleTrackMuted(int trackIndex) noexcept;
    /** Message-thread entry: quantized mute/unmute using the same scheduler as Play/Stop. */
    bool handleMuteAction(int trackIndex) noexcept;
    [[nodiscard]] bool trackMuted(int trackIndex) const noexcept;
    /**
     * Deferred-action sample for {@link TimingMode}. {@code pos < 0} uses the current transport sample.
     */
    [[nodiscard]] std::int64_t targetSampleForTimingMode(TimingMode mode, int trackIndex, std::int64_t pos = -1) const noexcept;
    /**
     * Seconds of FX spillover the host should keep pulling after the dry loop is silent
     * (mute / stop tails). Used by {@code AudioProcessor::getTailLengthSeconds()}.
     */
    void setSpilloverTailSeconds(double seconds) noexcept;
    [[nodiscard]] double spilloverTailSeconds() const noexcept;
    /** True when mute mode should treat Play/Stop as mute/unmute (track is playing or overdubbing).
     *  Rec/Dub mode overrides this so Play stays play/stop/record. */
    [[nodiscard]] bool playStopShouldMute(int trackIndex) const noexcept;
    [[nodiscard]] bool playStopShouldOverdub(int trackIndex) const noexcept;
    [[nodiscard]] bool anyPlayingTrackUnmuted() const noexcept;
    void setAllPlayingTracksMuted(bool muted) noexcept;
    void setAllTracksMuted(bool muted) noexcept;
    [[nodiscard]] bool allTracksMuted() const noexcept;
    void setPerTrackIsolationLock(bool enabled) noexcept;
    [[nodiscard]] bool perTrackIsolationLock() const noexcept;
    [[nodiscard]] LoopTrackDiagnostics trackDiagnostics(int trackIndex) const noexcept;

  private:
    void waitUntilAudioCallbackIdle() const noexcept;
    bool popCommand(EngineCommand& out) noexcept;
    bool pushScheduledCommand(const EngineCommand& command, std::int64_t executeAtSample) noexcept;
    void cancelScheduledRecordToggleForTrack(int trackIndex) noexcept;
    void cancelScheduledCommandsForTrack(int trackIndex, CommandType type) noexcept;
    void cancelAllScheduledCommandsForTrack(int trackIndex) noexcept;
    std::int64_t computeExecuteSampleForCommand(const EngineCommand& command) const noexcept;
    /** No track is Playing / Recording / Overdub (e.g. full stop before PLAY ALL). */
    [[nodiscard]] bool noTrackPlayingRecordingOrOverdub() const noexcept;
    /** Another track is Playing, Overdub, or Recording (clock already running). */
    [[nodiscard]] bool hasActivePeerTransport(int exceptTrackIndex) const noexcept;
    /** Set the shared loop phase origin when no other track is already in motion.
        Idle / all-stopped sessions rewind to the start so the next Play begins at sample 0. */
    void adoptSharedPhaseOriginIfNoActivePeers(int exceptTrackIndex, std::int64_t originSamples) noexcept;
    /** @param commandContextTransport Transport sample used for ctx when not -1 (segment boundaries / immediate). */
    void applyCommand(const EngineCommand& command,
        std::int64_t scheduledExecuteAtSample = -1,
        std::int64_t commandContextTransport = -1);
    /**
     * @param transportBoundaryExclusive if >= 0: exclusive transport sample for end of recording, overdub gate, and loop
     *   phase alignment (quantized stop). If -1, uses current transport position (immediate / block-start stops).
     */
    void finishRecording(int trackIndex, std::int64_t transportBoundaryExclusive = -1) noexcept;
    /** Hardware/mic takes use the driver round-trip. Pixel Beat, instruments, and mixer buses are already
        sample-aligned with the click in this callback, so they use 0. */
    [[nodiscard]] int effectiveRecordLatencyCompensationSamples(int trackIndex) const noexcept;
    void beginFirstPassRecording(int trackIndex, std::int64_t originSample) noexcept;
    void stopPlaybackPeersForRecording(int trackIndex, std::int64_t originSample) noexcept;
    void enterOverdubFromPlay(int trackIndex, std::int64_t originSample) noexcept;
    void rememberUndoSessionBpm() noexcept;
    void restoreSessionTempoFromUndo() noexcept;
    /** Default: only one track may record/overdub. Cancels other rec/dub cues; optionally ends live capture. */
    void enforceSingleTrackCapture(int exceptTrackIndex, bool stealLiveCapture) noexcept;
    void unmuteForCapture(int trackIndex) noexcept;
    /**
     * Halt the loop read pointer, 5 ms pre-FX fade, keep the insert in-circuit for tails.
     * Does not rewind the playhead or bypass delay/reverb.
     */
    void enterStoppedWithSpillover(int trackIndex, std::int64_t boundarySample) noexcept;
    void applyMuteToggle(int trackIndex, std::int64_t scheduledExecuteAtSample) noexcept;
    /** Leaving mute mode: cancel pending mutes, stop looping muted tracks, and clear the mute latch. */
    void releaseMutedTracksOnMuteModeExit() noexcept;
    void setMuteAudible(int trackIndex, bool audible) noexcept;
    [[nodiscard]] float nextMuteGain(std::size_t trackIndex) noexcept;
    void armPendingStateForScheduledCommand(const EngineCommand& command) noexcept;
    [[nodiscard]] std::int64_t globalPhaseLengthSamples(int trackIndex) const noexcept;
    [[nodiscard]] std::int64_t globalPhaseSample(std::int64_t transportPos, std::int64_t phaseLength) const noexcept;
    /** Quantized record-stop: either schedules end-of-block sample or calls {@code finishRecording} immediately. */
    void finalizeOrDeferRecordingStop(
        int trackIndex, std::int64_t scheduledExecuteAtSample, std::int64_t commandContextTransport) noexcept;
    /** Finish the first pass (now or at the cued sample) and land in Playing, ignoring after-record Dub. */
    void finalizeRecordingToPlaying(
        int trackIndex, std::int64_t scheduledExecuteAtSample, std::int64_t commandContextTransport) noexcept;
    [[nodiscard]] std::int64_t countInLeadSamples(int trackIndex) const noexcept;
    /** Next count-in grid sample (Beat/Bar/Loop). Does not stack on top of Rec/Play/Dub quantize. */
    [[nodiscard]] std::int64_t alignCountInExecuteSample(std::int64_t pos) const noexcept;
    [[nodiscard]] std::int64_t samplesPerBeatInt() const noexcept;
    /** One metronome count (notated beat from the time signature). */
    [[nodiscard]] std::int64_t samplesPerNotatedBeatInt() const noexcept;
    /** Samples for one quantize "bar" unit (notated measures × global bar length setting). */
    [[nodiscard]] std::int64_t samplesPerGlobalBar() const noexcept;
    [[nodiscard]] std::size_t lengthQuantizeUnitSamples() const noexcept;
    [[nodiscard]] std::size_t quantizeLengthSamples(std::size_t lengthSamples) const noexcept;
    void applyLengthQuantizeToTrack(Track& track) noexcept;
    [[nodiscard]] bool shouldApplyLengthQuantize(int trackIndex) const noexcept;
    [[nodiscard]] GlobalStopMode effectiveStopQuantizeMode(const Track& track) const noexcept;
    [[nodiscard]] PlaybackStartMode effectivePlayMode(const Track& track) const noexcept;
    [[nodiscard]] std::int64_t playbackPhaseOrigin(int trackIndex) const noexcept;
    void preparePlayStartPhase(int trackIndex, std::int64_t executeSample) noexcept;
    void clearPlayStartPhase(int trackIndex) noexcept;
    void clearRecordPhaseAnchor(int trackIndex) noexcept;
    [[nodiscard]] PlaybackStartMode effectiveRecordMode(const Track& track) const noexcept;
    [[nodiscard]] PlaybackStartMode effectiveOverdubMode(const Track& track) const noexcept;
    [[nodiscard]] PlaybackStartMode effectiveMuteMode(const Track& track) const noexcept;
    [[nodiscard]] std::int64_t alignPlayExecuteSample(int trackIndex, std::int64_t baseExecute, std::int64_t pos) const noexcept;
    [[nodiscard]] std::int64_t alignRecordExecuteSample(int trackIndex, std::int64_t baseExecute, std::int64_t pos) const noexcept;
    [[nodiscard]] std::int64_t alignOverdubExecuteSample(int trackIndex, std::int64_t baseExecute, std::int64_t pos) const noexcept;
    /** Leave overdub on Loop cue: the end of the loop being overdubbed. */
    [[nodiscard]] std::int64_t alignOverdubCueEnd(int trackIndex, std::int64_t baseExecute, std::int64_t pos) const noexcept;
    /** Loop cue in: next cycle of the longest other playing loop, else this track's own loop. */
    [[nodiscard]] std::int64_t alignLoopCueStart(int trackIndex, std::int64_t baseExecute, std::int64_t pos) const noexcept;
    /** Loop cue out while capturing: end of this take's cycle. */
    [[nodiscard]] std::int64_t alignRecordedCycleEnd(int trackIndex, std::int64_t baseExecute, std::int64_t pos) const noexcept;
    [[nodiscard]] std::int64_t alignExecuteSampleForPlaybackMode(
        int trackIndex, std::int64_t baseExecute, std::int64_t pos, PlaybackStartMode mode) const noexcept;
    [[nodiscard]] std::int64_t alignStopExecuteSample(int trackIndex, std::int64_t baseExecute, std::int64_t pos) const noexcept;
    [[nodiscard]] std::int64_t alignExecuteSampleForStopMode(
        std::int64_t baseExecute, std::int64_t pos, GlobalStopMode mode) const noexcept;
    /** Wrap of the longest Playing / Recording / Overdub loop. Immediate if none are running. */
    [[nodiscard]] std::int64_t alignToLongestPlayingLoopWrap(
        std::int64_t baseExecute, std::int64_t pos, bool waitFullCycleIfOnBoundary) const noexcept;
    /** Next wrap of this track's own loop length. Immediate if the track has no loop yet. */
    [[nodiscard]] std::int64_t alignToTrackLoopWrap(int trackIndex, std::int64_t baseExecute, std::int64_t pos,
        bool waitFullCycleIfOnBoundary) const noexcept;
    /** Next Track Sync boundary of the tempo-master loop (Synced follower record). */
    [[nodiscard]] std::int64_t alignToTrackSyncUnit(
        std::int64_t baseExecute, std::int64_t pos, bool waitFullCycleIfOnBoundary) const noexcept;
    [[nodiscard]] bool followerRecordUsesTrackSync(int trackIndex) const noexcept;
    [[nodiscard]] std::int64_t trackSyncUnitLengthSamples(int masterIndex) const noexcept;
    void reassignTrackSyncMasterAfterClear(int clearedTrack) noexcept;
    [[nodiscard]] bool tempoMasterVacant() const noexcept;
    void claimTempoMasterIfVacant(int trackIndex, bool fromImport) noexcept;
    void lockSessionTempoFromLoop() noexcept;
    /**
     * BPM from one cycle: {@code beatsPerCycle} quarter-note beats (0 = time-signature numerator),
     * octave-folded into 40–240.
     */
    [[nodiscard]] double tempoBpmFromCycleFrames(std::size_t cycleFrames, int beatsPerCycle = 0) const noexcept;
    /** Record-to-stop duration math: pick 2/4/8 (time-sig family) beats, set cycle count, then align tempo. */
    void applyLiveTakeDurationTempo(Track& track, int assumedBars = 0) noexcept;
    /** Rotate the seam to the nearest strong transient within ~40 ms. No-op without a clear peak (allocation-free). */
    void snapLiveLoopSeamToTransient(Track& track) noexcept;
    void refreshIsRecordingFlag() noexcept;
    /** Next transport sample where {@code trackIndex}'s loop wraps (LoopEnd-style), from {@code pos}. */
    [[nodiscard]] std::int64_t nextLoopBoundarySample(int trackIndex, std::int64_t pos) const noexcept;
    [[nodiscard]] std::size_t longestLoopLengthSamples() const noexcept;
    [[nodiscard]] std::size_t longestPlayingLoopLengthSamples(int exceptTrackIndex = -1) const noexcept;
    [[nodiscard]] int longestPlayingLoopIndex(int exceptTrackIndex) const noexcept;
    /** Copy the playing peer's phase origin so this track reads the same clock. */
    void lockTrackPhaseToPeer(int trackIndex) noexcept;
    [[nodiscard]] bool syncModeUsesSerialHandoff() const noexcept;
    /** Exclusive serial slot: every track in Serial; tracks 2–4 only in Serial-Parallel. */
    [[nodiscard]] bool trackIsSerialExclusive(int trackIndex) const noexcept;
    /** True if another exclusive track is Playing or Overdub with loop audio (serial switch competes). */
    [[nodiscard]] bool hasSerialCompetitor(int playTrackIndex) const noexcept;
    [[nodiscard]] std::int64_t computeSerialHandoffExecuteSample(int playTrackIndex, std::int64_t pos) const noexcept;
    /** Serial / Serial-Parallel: schedule or apply stop-competitors + play-target. Returns true if consumed. */
    bool tryScheduleSerialHandoff(const EngineCommand& command, std::int64_t blockStartSample) noexcept;
    /** Serial-Parallel: start track 1 if it has a loop and is sitting idle. */
    void maybeStartSerialParallelBed(std::int64_t origin) noexcept;
    void maybeStartSerialParallelBedAfterExclusiveStart(int exclusiveIndex, TrackState prior,
        std::int64_t origin) noexcept;
    [[nodiscard]] bool exclusiveLaunchPullsSerialParallelBed(const EngineCommand& command) const noexcept;
    void capturePlayAllResumeMaskIfSerial() noexcept;
    [[nodiscard]] bool playAllShouldStartTrack(int trackIndex) const noexcept;
    void captureSerialOutTail(int trackIndex, std::int64_t handoffSample) noexcept;
    [[nodiscard]] float serialFadeInMultiplier(int trackIndex, std::int64_t transportSample) const noexcept;
    /**
     * BPM implied by loop length: integer quarter count × 60 / duration.
     * @param useTransportHintForQuarterCount when true (import analysis), disambiguate with {@code transport_.tempoBpm()};
     *        when false (recording / session tempo), follow the global bar-length ladder only (metronome pre-roll may be wrong).
     */
    [[nodiscard]] double intrinsicTempoBpmForLoopLength(std::size_t lengthSamples,
        bool useTransportHintForQuarterCount = false) const noexcept;
    /**
     * How many quarter notes fit {@code lengthSamples}: either the global bar-length ladder + BPM band (default),
     * or a wider integer search biased toward {@code transport_.tempoBpm()} (import / odd phrase lengths).
     */
    [[nodiscard]] int chooseQuarterNotesInLoopForTempo(std::size_t lengthSamples, bool useTransportHint) const noexcept;
    [[nodiscard]] std::int64_t transportSampleAtNotatedBeatFloor(std::int64_t pos) const noexcept;
    [[nodiscard]] bool hasOtherLoopWithActivePlayback(int exceptTrackIndex) const noexcept;
    /** Metronome on or another track playing/overdubbing with loop audio (first-record quantize context). */
    [[nodiscard]] bool hasRecordTimingContext(int exceptTrackIndex) const noexcept;
    /** No closed loops yet, and no typed tempo with the clock running — first take Rec/Play/Stop is immediate so duration can set tempo. */
    [[nodiscard]] bool emptySessionFirstTakeIsImmediate() const noexcept;
    [[nodiscard]] bool trackRecordsInternalGraph(int trackIndex) const noexcept;
    /** Typed tempo plus a running Play All clock. Loop cues use track timing instead of starting now. */
    [[nodiscard]] bool clockGridIsActive() const noexcept;
    /** Nothing is playing/recording and no launch is queued — Play / Play All starts now. */
    [[nodiscard]] bool idlePlaybackStartsImmediately(const EngineCommand& command) const noexcept;
    [[nodiscard]] bool anyTrackPlayingOrPendingLaunch() const noexcept;
    [[nodiscard]] bool anyTrackAdvancingOrRecording() const noexcept;
    [[nodiscard]] bool commandStartsPlayback(const EngineCommand& command) const noexcept;
    /** Sets track reference BPM: intrinsic from loop when plausible, else transport tempo (short/degenerate loops). */
    void assignReferenceBpmForLoop(Track& track) noexcept;
    /** First-loop / sole-track tempo: length-derived session BPM. Optionally assigns a stretch reference. */
    void applyTempoDetectionForSoleLoop(int trackIndex, bool assignReferenceBpm = true) noexcept;
    /** Rewrite loop origins so (pos - origin) / samplesPerBeat is unchanged after a tempo change. */
    void applyPendingTempoPhasePreserve() noexcept;
    void queueTempoPhasePreserveFrom(double previousBpm) noexcept;
    void publishMusicalClockUi() noexcept;
    /** Confidence-gated + hysteretic tempo adoption to prevent transport jitter from weak detections. */
    void proposeTransportTempoFromDetection(double detectedBpm, double confidence) noexcept;
    void resetStretchForTrack(int trackIndex) noexcept;
    /** After stop: this track's playhead at loop start. Shared phase origin is only moved if no other
        track is Playing / Recording / Overdub, so importing or stopping one loop cannot rewind the rest. */
    void resetLoopPlaybackToBeginning(int trackIndex, std::int64_t phaseOriginSample = -1) noexcept;
    void ensureStretchEngine(int trackIndex);
    /** Pitch-preserving time-stretch; seeks on cold start, then streams without jumping the STFT read position. */
    void renderTimeStretchBlock(int trackIndex,
        const Track& track,
        double tempoRatio,
        int numSamples,
        float* outL,
        float* outR,
        double musicalSourceStart,
        double& phaseEndUnwrappedOut);

    std::array<Track, kMaxTracks> tracks_{};
    TransportClock transport_{};
    QuantizeScheduler quantizer_{};
    SyncMode syncMode_ = SyncMode::Synced;
    TrackSyncUnit trackSyncUnit_{TrackSyncUnit::Loop};
    int subcycles_{4};
    int countInBars_ = 1;
    int quantizeDivision_ = 1;
    std::atomic<float> masterGain_{1.0f};
    std::atomic<bool> deferDeviceMasterGain_{false};
    std::atomic<float> crossfaderTarget_{0.5f};
    float crossfaderSmooth_{0.5f};
    std::array<std::atomic<int>, kMaxTracks> crossfadeAssign_{};
    SmoothedValue<float> masterGainSmooth_{};
    bool freeMode_{false};
    GlobalStopMode globalStopMode_{GlobalStopMode::Bar};
    AfterRecordMode afterRecordMode_{AfterRecordMode::Play};
    CountInStyle countInStyle_{CountInStyle::Bar};
    CountOutStyle countOutStyle_{CountOutStyle::Off};
    PlaybackStartMode playbackStartMode_{PlaybackStartMode::Bar};
    bool playPhaseLockEnabled_{false};
    PlaybackStartMode playAllMode_{PlaybackStartMode::Immediate};
    GlobalStopMode stopAllMode_{GlobalStopMode::Immediate};
    PlaybackStartMode recordStartMode_{PlaybackStartMode::Bar};
    PlaybackStartMode overdubStartMode_{PlaybackStartMode::Bar};
    PlaybackStartMode muteStartMode_{PlaybackStartMode::Bar};
    PlaybackStartMode undoStartMode_{PlaybackStartMode::Immediate};
    FxBypassSyncMode fxBypassSyncMode_{FxBypassSyncMode::Immediate};
    double undoSessionBpm_{0.0};
    bool undoSessionBpmValid_{false};
    /** Default 1 notated measure per loop for tempo + quantize bar unit; use 2/4/8 for multi-measure phrases. */
    int globalBarLengthBars_{1};
    LengthQuantizeGrid lengthQuantizeGrid_{LengthQuantizeGrid::Bar};
    int timeSigNumerator_{4};
    int timeSigDenominator_{4};
    BeatCountDisplay beatCountDisplay_{BeatCountDisplay::AllBeats};
    bool metronomeEnabled_{false};
    std::atomic<bool> metronomeMixToDevice_{true};
    static constexpr int kMetroScratchCap = 8192;
    std::vector<float> metroMixL_{};
    std::vector<float> metroMixR_{};
    int metroMixValidSamples_{0};
    bool playAllStartsClickWhenEmpty_{true};
    std::atomic<bool> linkTransportImmediate_{false};
    std::atomic<std::int64_t> linkQuantumLaunchSample_{-1};
    bool metronomeForceRunning_{false};
    float overdubFeedback_{1.0f};
    float loopCrossfadeMs_{3.0f};
    int loopCrossfadeSamples_{144};
    std::atomic<int> recordLatencyCompensationSamples_{0};
    std::atomic<int> firstPassRecordOffsetInBlock_{-1};
    std::atomic<bool> recordLimitReached_{false};
    std::atomic<std::uint64_t> captureStartSerial_{0};
    float metronomeGain_{0.55f};
    int metronomeOutCh_{-1};
    bool metronomeStereoPair_{false};
    /** Leftover click kernel that did not fit in the previous audio block. */
    std::array<float, 512> metroClickResidual_{};
    int metroClickResidualLen_{0};
    int metroClickResidualOff_{0};
    MetronomeSoundKind metroNormalKind_{MetronomeSoundKind::Click};
    MetronomeSoundKind metroAccentKind_{MetronomeSoundKind::Wood};
    /** Double-buffered custom click samples: audio thread reads active index; setters write the other slot. */
    std::array<std::vector<float>, 2> metroNormalCustomBuf_{};
    std::array<std::vector<float>, 2> metroAccentCustomBuf_{};
    std::atomic<int> metroNormalReadIndex_{0};
    std::atomic<int> metroAccentReadIndex_{0};
    bool multiTrackRecord_{false};
    std::atomic<float> lastInputPeak_{0.f};
    std::atomic<float> lastOutputPeak_{0.f};
    std::array<std::atomic<float>, kMaxTracks> trackOutPeakL_{};
    std::array<std::atomic<float>, kMaxTracks> trackOutPeakR_{};
    std::array<std::atomic<float>, kMaxTracks> trackInPeakL_{};
    std::array<std::atomic<float>, kMaxTracks> trackInPeakR_{};
    std::array<std::atomic<float>, kMaxTracks> trackOutRmsL_{};
    std::array<std::atomic<float>, kMaxTracks> trackOutRmsR_{};
    std::array<std::atomic<float>, kMaxTracks> trackInRmsL_{};
    std::array<std::atomic<float>, kMaxTracks> trackInRmsR_{};
    std::unique_ptr<WaveformEnvelopeBank> waveformEnvelopes_;
    std::array<int, kMaxTracks> pendingLoopBars_{};
    std::array<TrackIoRouting, kMaxTracks> trackIo_{};
    std::array<std::size_t, kMaxTracks> trackPlayheads_{};
    std::array<std::size_t, kMaxTracks> trackWriteheads_{};
    /**
     * When >= 0, record/overdub input for this track is ignored until this transport sample (quantized
     * start/stop in the middle of a process block). -1 = no gate.
     */
    std::array<std::int64_t, kMaxTracks> trackRecordInputGateTransport_{};
    /**
     * Exclusive transport sample where recording must end (-1 = none). Quantized in-block stops write input only while
     * transport sample is below this value, then finishRecording runs before processing the rest of the block.
     */
    std::array<std::int64_t, kMaxTracks> pendingRecordEndExclusive_{};
    /** When true, the pending/next {@code finishRecording} for this track lands in Playing (Play during record). */
    std::array<bool, kMaxTracks> finishRecordingToPlaying_{};
    /**
     * Single transport anchor for all loop playback: read position is (transportSample - origin) % length.
     * Stays put while tracks stop/start so every loop stays phase-locked to the master clock.
     */
    std::int64_t sharedLoopPhaseOrigin_{0};
    /** When true, this track reads from {@link trackPhaseOrigin_} instead of the shared clock. */
    std::array<bool, kMaxTracks> trackPhaseFromStart_{};
    std::array<std::int64_t, kMaxTracks> trackPhaseOrigin_{};
    /** Recorded loop stays aligned to its own start while other tracks keep the shared clock. */
    std::array<bool, kMaxTracks> trackRecordPhaseAnchor_{};
    std::array<std::int64_t, kMaxTracks> trackRecordPhaseOrigin_{};
    /** Last transport position (for UI-thread import anchoring). Updated at end of each process block. */
    std::atomic<std::int64_t> transportSamplePositionUi_{0};
    std::atomic<std::int64_t> sharedLoopPhaseOriginUi_{0};
    std::atomic<std::int64_t> transportUiSteadyNs_{0};
    /** Next source-sample index to feed the stretcher (unwrapped along the loop). */
    std::array<double, kMaxTracks> stretchSourceUnwrapped_{};
    std::array<double, kMaxTracks> stretchFrac_{};
    std::array<bool, kMaxTracks> stretchWarm_{};
    std::array<std::int64_t, kMaxTracks> stretchPhaseOrigin_{};
    /** First BPM of a burst of setTempo calls; applied on the audio thread. */
    std::atomic<double> tempoPhaseAdjustFromBpm_{120.0};
    std::atomic<bool> pendingTempoPhaseAdjust_{false};
    /** BPM used for stretch / click for this callback only, so a mid-block setTempo cannot desync origin. */
    double blockMasterBpm_{120.0};
    std::array<std::unique_ptr<TrackTimeStretcher>, kMaxTracks> stretchEngines_;
    std::vector<float> stretchInL_;
    std::vector<float> stretchInR_;
    std::vector<float> stretchOutL_;
    std::vector<float> stretchOutR_;
    std::vector<float> stereoBusScratchL_;
    std::vector<float> stereoBusScratchR_;
    std::vector<float> mixVolScratch_;
    std::vector<float> mixPanScratch_;
    std::vector<float> mixInScratch_;
    std::vector<float> mixFxScratch_;
    std::vector<float> mixDryLScratch_;
    std::vector<float> mixDryRScratch_;
    TrackStereoBusProcessFn trackStereoBusProcessor_{};
    /** ~8 ms at current rate; used for serial handoff crossfade + incoming fade-in. */
    int serialCrossfadeSamples_{384};
    std::array<std::vector<float>, kMaxTracks> serialTailL_{};
    std::array<std::vector<float>, kMaxTracks> serialTailR_{};
    std::array<int, kMaxTracks> serialTailLen_{};
    std::array<std::int64_t, kMaxTracks> serialTailHandoffSample_{};
    /** Transport sample after which serial fade-in reaches unity (0 = inactive). */
    std::array<std::int64_t, kMaxTracks> serialFadeInEndSample_{};
    static constexpr float kMuteRampMilliseconds = 5.0f;
    static constexpr double kDefaultSpilloverTailSeconds = 6.0;
    int muteRampSamples_{240};
    std::array<float, kMaxTracks> muteGain_{};
    std::array<float, kMaxTracks> muteGainTarget_{};
    std::array<std::size_t, kMaxTracks> stopFadeReadPos_{};
    std::atomic<double> spilloverTailSeconds_{kDefaultSpilloverTailSeconds};
    std::array<ScheduledCommand, kCommandQueueSize> scheduledCommands_{};
    TrackBehaviorMode trackBehaviorMode_{TrackBehaviorMode::RecordPlayOverdubPlay};
    bool dubModeEnabled_{false};
    bool muteModeEnabled_{false};
    std::atomic<bool> releaseMutedOnMuteModeExit_{false};
    bool perTrackIsolationLock_{true};
    std::array<TransitionError, kMaxTracks> lastTransitionError_{};
    int tempoMasterTrack_{-1};
    /** An imported loop is not a recorded take, so it must not keep the session tempo. */
    bool tempoMasterIsImport_{false};
    /** Typed, tapped, or fader tempo. Survives clearing loops. A detected loop tempo does not. */
    bool userTempoCommitted_{false};
    TempoManager tempoManager_{};
    std::atomic<bool> isRecording_{false};
    std::atomic<bool> loopIsClosed_{false};
    std::atomic<std::uint64_t> viewLoopSamples_{0};
    std::atomic<std::uint64_t> viewPlayheadSamples_{0};
    bool liveTransientCapture_{false};
    RealtimeTransientDetector liveTransientDetector_{};
    /** Hysteresis state for tempo-adoption proposals (UI/control thread). */
    double pendingDetectedTempoBpm_{0.0};
    int pendingDetectedTempoVotes_{0};
    double lastAcceptedDetectedTempoBpm_{0.0};

    void refreshLoopCrossfadeSamples() noexcept;
    void renderMetronome(std::int64_t blockStartSample, int numSamples, float* const* output, int numOutputChannels) noexcept;
    /** Per-track audio for one contiguous slice of the host block (quantized command boundaries). */
    void processTrackAudioSegment(std::int64_t fullBlockTransportStart,
        int offsetInBlock,
        int segSamples,
        const float* const* hardwareInput,
        int numHardwareInputChannels,
        const float* instrumentInputLeft,
        const float* instrumentInputRight,
        int numInstrumentChannels,
        float* const* output,
        int numOutputChannels,
        float& inputPeakInOut,
        metering::TrackMeterBlockAccum& meterAccum,
        const float* const* trackRecordInput = nullptr,
        int numTrackRecordChannels = 0) noexcept;
    void publishWaveformEnvelopes() noexcept;
    void publishLoopViewAtomics() noexcept;

    BoundedCommandQueue<EngineCommand, kCommandQueueSize> commandQueue_{};
    std::int64_t clusteredPlayExecuteAt_{-1};
    std::int64_t idleLaunchGroupEndsAt_{-1};
    std::uint8_t playAllResumeMask_{0};
    bool playAllResumeMaskValid_{false};
    std::atomic<int> audioCallbackDepth_{0};
};
} // namespace retrolooper
