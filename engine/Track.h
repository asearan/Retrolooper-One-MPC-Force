// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "EngineTypes.h"
#include "SmoothedValue.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>
#include <new>
#include <utility>

namespace retrolooper
{
// std::vector normally zero-initialises appended floats. Recording writes every
// exposed sample itself; skipping that redundant pass keeps growth within an
// already reserved block constant-time, including quantized silence padding.
template<class T> struct AudioStorageAllocator : std::allocator<T>
{
    using value_type=T;
    template<class U> struct rebind { using other=AudioStorageAllocator<U>; };
    AudioStorageAllocator()=default;
    template<class U> AudioStorageAllocator(const AudioStorageAllocator<U>&) noexcept {}
    template<class U,class... Args> void construct(U* pointer,Args&&... args) {
        if constexpr(sizeof...(Args)==0) ::new(static_cast<void*>(pointer)) U;
        else ::new(static_cast<void*>(pointer)) U(std::forward<Args>(args)...);
    }
};

enum class TrackState
{
    Empty,
    Recording,
    Playing,
    Overdub,
    Stopped,
    /** Record armed: waiting for quantized / count-in boundary (scheduled RecordToggle pending). */
    Armed,
    /** Playing silently; read pointer stays locked to the shared loop phase. */
    Muted,
    Playing_MutePending,
    Overdubbing_MutePending,
    Muted_UnmutePending,
    Playing_DubPending
};

[[nodiscard]] inline bool trackStateAdvancesPlayhead(const TrackState s) noexcept
{
    switch (s)
    {
        case TrackState::Playing:
        case TrackState::Overdub:
        case TrackState::Muted:
        case TrackState::Playing_MutePending:
        case TrackState::Overdubbing_MutePending:
        case TrackState::Muted_UnmutePending:
        case TrackState::Playing_DubPending:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] inline bool trackStateIsOverdub(const TrackState s) noexcept
{
    return s == TrackState::Overdub || s == TrackState::Overdubbing_MutePending;
}

[[nodiscard]] inline bool trackStateIsSilent(const TrackState s) noexcept
{
    return s == TrackState::Muted || s == TrackState::Muted_UnmutePending;
}

/** Audible looping states that should follow master tempo via time-stretch. */
[[nodiscard]] inline bool trackStateShouldTimeStretch(const TrackState s) noexcept
{
    return trackStateAdvancesPlayhead(s) && !trackStateIsSilent(s);
}

[[nodiscard]] inline bool trackStateIsMutePending(const TrackState s) noexcept
{
    return s == TrackState::Playing_MutePending || s == TrackState::Overdubbing_MutePending
        || s == TrackState::Muted_UnmutePending;
}

[[nodiscard]] inline TrackState trackStateCancelMutePending(const TrackState s) noexcept
{
    switch (s)
    {
        case TrackState::Playing_MutePending: return TrackState::Playing;
        case TrackState::Overdubbing_MutePending: return TrackState::Overdub;
        case TrackState::Muted_UnmutePending: return TrackState::Muted;
        default: return s;
    }
}

[[nodiscard]] inline bool trackStateIsPending(const TrackState s) noexcept
{
    switch (s)
    {
        case TrackState::Armed:
        case TrackState::Playing_MutePending:
        case TrackState::Playing_DubPending:
        case TrackState::Overdubbing_MutePending:
        case TrackState::Muted_UnmutePending:
            return true;
        default:
            return false;
    }
}

struct TrackMix
{
    float volume = 1.0f;
    /** Linear gain applied to the record/overdub input for this track (0–1). */
    float inputGain = 1.0f;
    float pan = 0.0f;
    /** Reserved mix control (smoothed). Mixer channel-strip / inserts stay 100% in-line. */
    float fxSend = 1.0f;
    bool muted = false;
    bool solo = false;
    /** Hear the routed input on this track's output without recording it. */
    bool directMonitor = false;
};

class Track
{
  public:
    Track();
    explicit Track(int channels);

    void setState(TrackState nextState) noexcept;
    [[nodiscard]] TrackState state() const noexcept;

    void setMix(const TrackMix& nextMix) noexcept;
    [[nodiscard]] TrackMix mix() const noexcept;

    /** 20 ms linear ramp for volume / pan / input / FX send (JUCE SmoothedValue equivalent). */
    void prepareMixSmoothing(double sampleRate, double rampSeconds = 0.020) noexcept;
    void renderSmoothedMix(int numSamples, float* volume, float* pan, float* inputGain, float* fxSend) noexcept;
    [[nodiscard]] bool volumeIsSmoothing() const noexcept { return volumeSmooth_.isSmoothing(); }
    [[nodiscard]] bool panIsSmoothing() const noexcept { return panSmooth_.isSmoothing(); }

    void setName(std::string nextName);
    [[nodiscard]] const std::string& name() const noexcept;

    void setLoopPrefs(const TrackLoopPrefs& prefs) noexcept;
    [[nodiscard]] TrackLoopPrefs loopPrefs() const noexcept;

    void setLengthSamples(std::size_t samples) noexcept;
    [[nodiscard]] std::size_t lengthSamples() const noexcept;
    /**
     * How many equal cycles the loop is divided into (Mobius). A new live take is 1 cycle;
     * Instant Multiply doubles length and this count so cycle length (and tempo) stay the same.
     */
    void setCycleCount(int cycles) noexcept;
    [[nodiscard]] int cycleCount() const noexcept;
    /** {@code lengthSamples / max(1, cycleCount)}; 0 when empty. */
    [[nodiscard]] std::size_t cycleLengthSamples() const noexcept;
    /** Allocated loop buffer size (may be longer than {@code lengthSamples} after a trim). */
    [[nodiscard]] std::size_t bufferCapacity() const noexcept;
    /** Heap reserved for live record (may be larger than {@link bufferCapacity}). */
    [[nodiscard]] std::size_t allocatedCapacity() const noexcept;
    [[nodiscard]] int channels() const noexcept;

    void clearAudio();
    /**
     * Message-thread import: freeze sample access so the audio callback cannot read while
     * the buffer is reallocated. Pair with {@link unfreezeAudioBuffer} after replace.
     */
    void freezeAudioBuffer() noexcept;
    void unfreezeAudioBuffer() noexcept;
    /** Replace audible samples (length = numFrames). Reuses capacity; does not allocate the live-record slab. */
    void importReplaceStereo(const float* left, const float* right, std::size_t numFrames) noexcept;
    void ensureCapacity(std::size_t samples);
    bool ensureRealtimeCapacity(std::size_t samples) noexcept;
    void silenceRange(std::size_t begin,std::size_t end) noexcept;
    void resampleAudio(double oldRate, double newRate);
    void setRealtimeActive(bool active) noexcept { realtimeActive_.store(active, std::memory_order_release); }
    void serviceSaveSnapshot() noexcept;
    bool beginStateSnapshot() noexcept;
    std::size_t stateSnapshotFrames() const noexcept { return saveSnapshot_.length; }
    bool copyStateSnapshot(std::vector<float>& left, std::vector<float>& right) const;
    void endStateSnapshot() const noexcept { saveSnapshotState_.store(0,std::memory_order_release); }
    /** Prepare live record and history storage while the audio callback is stopped. */
    void reserveRecordHeadroom(std::size_t samples) noexcept;
    void writeSample(int channel, std::size_t sampleIndex, float sampleValue) noexcept;
    [[nodiscard]] float readSample(int channel, std::size_t sampleIndex) const noexcept
    {
        if (audioFrozen_.load(std::memory_order_relaxed))
        {
            return 0.0f;
        }
        return readStoredSample(channel, physicalIndex(sampleIndex));
    }
    /**
     * Copy the locked loop ({@link lengthSamples}, not allocated capacity) under a shared lock
     * so a stem export cannot read while {@link clearAudio} frees the buffer.
     * Growing the live record buffer ({@link ensureCapacity}) does not take this lock.
     */
    bool copyLockedLoopAudio(std::vector<float>& left, std::vector<float>& right) const;
    /** Locked loop, or the in-progress take when a recording has not been committed yet. */
    bool copyLockedSaveAudio(std::vector<float>& left, std::vector<float>& right, bool* hasAudio = nullptr) const;
    /** Rotate the audible loop earlier by {@code amount} samples (wraps). No-op if length is 0. */
    bool rotateLeft(std::size_t amount) noexcept;
    /** Reverse the locked loop in place. @return true if any audio changed. */
    bool reverseLiveAudio() noexcept;
    [[nodiscard]] std::size_t recordedSamples() const noexcept;
    void setRecordedSamples(std::size_t samples) noexcept;

    /** Quarter-note beats represented by one cycle. 0 = use the session time-signature numerator. */
    void setBeatsPerCycle(int beats) noexcept;
    [[nodiscard]] int beatsPerCycle() const noexcept;

    /** BPM the loop was recorded or imported at; used to time-stretch vs the current master tempo. */
    void setReferenceBpm(double bpm) noexcept;
    [[nodiscard]] double referenceBpm() const noexcept;

    static constexpr int kMaxOverdubLayers = 4;

    /** Push live loop audio as an undo layer (up to {@link kMaxOverdubLayers}); clears redo. */
    void captureOverdubUndoSnapshot() noexcept;
    void restoreOverdubUndoSnapshot() noexcept;
    void clearOverdubUndoSnapshot() noexcept;
    [[nodiscard]] bool hasOverdubUndoSnapshot() const noexcept;
    void captureOverdubRedoSnapshot() noexcept;
    void restoreOverdubRedoSnapshot() noexcept;
    void clearOverdubRedoSnapshot() noexcept;
    [[nodiscard]] bool hasOverdubRedoSnapshot() const noexcept;
    [[nodiscard]] int overdubUndoDepth() const noexcept;
    [[nodiscard]] int overdubRedoDepth() const noexcept;
    /** Zero the live loop without discarding undo/redo layers (accidental Clear). */
    void clearLiveAudioKeepHistory() noexcept;
    /** Message thread. Sizes undo/redo storage so entering overdub does not allocate. */
    void reserveOverdubHistory(std::size_t samples) noexcept;

  private:
    int channels_ = 2;
    TrackState state_ = TrackState::Empty;
    TrackMix mix_{};
    std::atomic<float> volumeTarget_{1.0f};
    std::atomic<float> panTarget_{0.0f};
    std::atomic<float> inputGainTarget_{1.0f};
    std::atomic<float> fxSendTarget_{1.0f};
    SmoothedValue<float> volumeSmooth_{};
    SmoothedValue<float> panSmooth_{};
    SmoothedValue<float> inputGainSmooth_{};
    SmoothedValue<float> fxSendSmooth_{};
    std::string name_{};
    TrackLoopPrefs loopPrefs_{};
    std::size_t lengthSamples_ = 0;
    int cycleCount_ = 1;
    int beatsPerCycle_ = 0;
    std::size_t recordedSamples_ = 0;
    double referenceBpm_ = 0.0;
    std::vector<std::vector<float,AudioStorageAllocator<float>>> audio_{};

    struct Rotation { std::size_t length=0, offset=0; };
    // Capture applies at most three seam/phase rotations before resetting the view.
    static constexpr std::size_t kMaxRotations=8;
    using Rotations=std::array<Rotation,kMaxRotations>;
    struct OverdubLayer
    {
        std::vector<std::uint8_t> pages{};
        Rotations rotations{};
        std::size_t rotationCount=0;
        std::size_t length = 0;
        int cycleCount = 1;
        int beatsPerCycle = 0;
        double referenceBpm = 0.0;
    };
    std::array<OverdubLayer, kMaxOverdubLayers> undoLayers_{};
    std::array<OverdubLayer, kMaxOverdubLayers> redoLayers_{};
    int undoCount_{0};
    int redoCount_{0};
    std::atomic<int> undoDepth_{0};
    std::atomic<int> redoDepth_{0};
    std::atomic<bool> audioFrozen_{false};
    mutable std::atomic<int> saveHold_{0};
    mutable std::atomic<int> reallocPending_{0};
    mutable std::unique_ptr<std::shared_mutex> audioMutex_{};

    void clearAudioUnlocked();
    void ensureCapacityUnlocked(std::size_t samples);
    bool copyLockedSpan(std::size_t sampleCount, std::vector<float>& left, std::vector<float>& right, bool* hasAudio = nullptr) const;
    void clearOverdubUndoSnapshotUnlocked();
    void clearOverdubRedoSnapshotUnlocked();
    void captureOverdubUndoSnapshotUnlocked();
    void captureOverdubRedoSnapshotUnlocked();
    static constexpr std::size_t kPageFrames = 1024;
    struct HistoryBank { std::vector<std::unique_ptr<float[]>> channels; std::size_t capacity = 0; };
    std::array<HistoryBank, kMaxOverdubLayers * 2 + 1> historyBanks_{};
    std::vector<std::uint8_t> livePages_, writablePages_;
    Rotations rotations_{};
    std::size_t rotationCount_=0;
    mutable OverdubLayer saveSnapshot_{};
    mutable std::atomic<int> saveSnapshotState_{0}; // 0 idle, 1 requested, 2 immutable/ready
    std::atomic<bool> realtimeActive_{false};
    void writeStoredSample(int channel, std::size_t index, float value) noexcept;
    void prepareHistoryStorage(std::size_t samples);
    void invalidateWritablePages() noexcept;
    bool makePageWritable(std::size_t page) noexcept;
    static std::size_t mapIndex(std::size_t index,const Rotations& rotations,std::size_t count) noexcept;
    static std::size_t viewSpan(std::size_t length,const Rotations& rotations,std::size_t count) noexcept;
    std::size_t physicalIndex(std::size_t index) const noexcept;
    float readStoredSample(int channel, std::size_t index) const noexcept;
    void fillLayerFromLiveUnlocked(OverdubLayer& layer);
    void applyLayerToLiveUnlocked(const OverdubLayer& layer);
    void publishLayerDepths() noexcept;
};
} // namespace retrolooper
