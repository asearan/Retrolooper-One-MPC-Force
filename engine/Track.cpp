// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#include "Track.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <cmath>
#include <chrono>
#include <limits>
#include <thread>
#include <utility>

namespace retrolooper
{
Track::Track() : Track(2) {}

Track::Track(int channels)
    : channels_(channels)
    , audioMutex_(std::make_unique<std::shared_mutex>())
{
    if (channels_ < 1)
    {
        channels_ = 1;
    }
    audio_.resize(static_cast<std::size_t>(channels_));
    volumeSmooth_.setCurrentAndTargetValue(1.0f);
    panSmooth_.setCurrentAndTargetValue(0.0f);
    inputGainSmooth_.setCurrentAndTargetValue(1.0f);
    fxSendSmooth_.setCurrentAndTargetValue(1.0f);
}

void Track::setState(TrackState nextState) noexcept
{
    state_ = nextState;
}

TrackState Track::state() const noexcept
{
    return state_;
}

void Track::setMix(const TrackMix& nextMix) noexcept
{
    mix_ = nextMix;
    mix_.volume = std::clamp(mix_.volume, 0.0f, 1.0f);
    mix_.inputGain = std::clamp(mix_.inputGain, 0.0f, 1.0f);
    mix_.pan = std::clamp(mix_.pan, -1.0f, 1.0f);
    mix_.fxSend = std::clamp(mix_.fxSend, 0.0f, 1.0f);
    volumeTarget_.store(mix_.volume, std::memory_order_relaxed);
    panTarget_.store(mix_.pan, std::memory_order_relaxed);
    inputGainTarget_.store(mix_.inputGain, std::memory_order_relaxed);
    fxSendTarget_.store(mix_.fxSend, std::memory_order_relaxed);
}

TrackMix Track::mix() const noexcept
{
    auto m = mix_;
    m.volume = volumeTarget_.load(std::memory_order_relaxed);
    m.pan = panTarget_.load(std::memory_order_relaxed);
    m.inputGain = inputGainTarget_.load(std::memory_order_relaxed);
    m.fxSend = fxSendTarget_.load(std::memory_order_relaxed);
    return m;
}

void Track::prepareMixSmoothing(const double sampleRate, const double rampSeconds) noexcept
{
    volumeSmooth_.reset(sampleRate, rampSeconds);
    panSmooth_.reset(sampleRate, rampSeconds);
    inputGainSmooth_.reset(sampleRate, rampSeconds);
    fxSendSmooth_.reset(sampleRate, rampSeconds);
    volumeSmooth_.setCurrentAndTargetValue(volumeTarget_.load(std::memory_order_relaxed));
    panSmooth_.setCurrentAndTargetValue(panTarget_.load(std::memory_order_relaxed));
    inputGainSmooth_.setCurrentAndTargetValue(inputGainTarget_.load(std::memory_order_relaxed));
    fxSendSmooth_.setCurrentAndTargetValue(fxSendTarget_.load(std::memory_order_relaxed));
}

void Track::renderSmoothedMix(const int numSamples, float* volume, float* pan, float* inputGain, float* fxSend) noexcept
{
    volumeSmooth_.setTargetValue(volumeTarget_.load(std::memory_order_relaxed));
    panSmooth_.setTargetValue(panTarget_.load(std::memory_order_relaxed));
    inputGainSmooth_.setTargetValue(inputGainTarget_.load(std::memory_order_relaxed));
    fxSendSmooth_.setTargetValue(fxSendTarget_.load(std::memory_order_relaxed));
    const int n = std::max(0, numSamples);
    volumeSmooth_.writeNext(volume, n);
    panSmooth_.writeNext(pan, n);
    inputGainSmooth_.writeNext(inputGain, n);
    fxSendSmooth_.writeNext(fxSend, n);
}

void Track::setName(std::string nextName)
{
    if (nextName.size() > 32)
    {
        nextName.resize(32);
    }
    for (char& c : nextName)
    {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    name_ = std::move(nextName);
}

const std::string& Track::name() const noexcept
{
    return name_;
}

void Track::setLoopPrefs(const TrackLoopPrefs& prefs) noexcept
{
    loopPrefs_ = prefs;
}

TrackLoopPrefs Track::loopPrefs() const noexcept
{
    return loopPrefs_;
}

void Track::setLengthSamples(std::size_t samples) noexcept
{
    lengthSamples_ = samples;
}

std::size_t Track::lengthSamples() const noexcept
{
    return lengthSamples_;
}

void Track::setCycleCount(int cycles) noexcept
{
    cycleCount_ = std::max(1, cycles);
}

int Track::cycleCount() const noexcept
{
    return std::max(1, cycleCount_);
}

std::size_t Track::cycleLengthSamples() const noexcept
{
    if (lengthSamples_ == 0)
    {
        return 0;
    }
    return lengthSamples_ / static_cast<std::size_t>(std::max(1, cycleCount_));
}

void Track::setBeatsPerCycle(int beats) noexcept
{
    beatsPerCycle_ = std::max(0, beats);
}

int Track::beatsPerCycle() const noexcept
{
    return std::max(0, beatsPerCycle_);
}

std::size_t Track::bufferCapacity() const noexcept
{
    return audio_.empty() ? 0 : audio_[0].size();
}

std::size_t Track::allocatedCapacity() const noexcept
{
    return audio_.empty() ? 0 : audio_[0].capacity();
}

int Track::channels() const noexcept
{
    return channels_;
}

void Track::clearAudioUnlocked()
{
    for (auto& channelData : audio_)
    {
        channelData.clear();
    }
    rotationCount_ = 0;
    recordedSamples_ = 0;
    lengthSamples_ = 0;
    cycleCount_ = 1;
    beatsPerCycle_ = 0;
    referenceBpm_ = 0.0;
    clearOverdubUndoSnapshotUnlocked();
    std::fill(livePages_.begin(), livePages_.end(), 0);
    invalidateWritablePages();
}

void Track::clearLiveAudioKeepHistory() noexcept
{
    rotationCount_ = 0;
    recordedSamples_ = 0;
    lengthSamples_ = 0;
    cycleCount_ = 1;
    beatsPerCycle_ = 0;
    referenceBpm_ = 0.0;
}

void Track::clearAudio()
{
    std::unique_lock lock(*audioMutex_);
    clearAudioUnlocked();
}

void Track::freezeAudioBuffer() noexcept
{
    audioFrozen_.store(true, std::memory_order_release);
}

void Track::unfreezeAudioBuffer() noexcept
{
    audioFrozen_.store(false, std::memory_order_release);
}

void Track::ensureCapacityUnlocked(std::size_t samples)
{
    // MSVC vector grows ~1.5x. A live take that starts from 0 reallocates near
    // 2.87e6 samples — about 60 s at 48 kHz — and copies the whole loop on the
    // audio thread. Reserve in multi-minute slabs so that copy never happens mid-take.
    constexpr std::size_t kMinReserve = 48000u * 180u;
    constexpr std::size_t kGrow = 48000u * 90u;
    bool needRealloc = false;
    for (const auto& channelData : audio_)
    {
        if (channelData.size() < samples && channelData.capacity() < samples)
        {
            needRealloc = true;
            break;
        }
    }
    if (needRealloc)
    {
        // A preset save may be copying this storage. Skipping the grow keeps
        // the old buffer alive for that copy. The next block retries.
        reallocPending_.store(1, std::memory_order_release);
        if (saveHold_.load(std::memory_order_acquire) != 0)
        {
            reallocPending_.store(0, std::memory_order_release);
            needRealloc = false;
        }
    }
    for (auto& channelData : audio_)
    {
        if (channelData.size() >= samples)
        {
            continue;
        }
        if (channelData.capacity() < samples)
        {
            if (!needRealloc)
            {
                continue;
            }
            std::size_t newCap = (std::max)(samples, kMinReserve);
            if (channelData.capacity() >= kMinReserve)
            {
                newCap = channelData.capacity() + kGrow;
                if (newCap < samples)
                {
                    newCap = samples + kGrow;
                }
            }
            channelData.reserve(newCap);
        }
        if (channelData.capacity() >= samples)
        {
            channelData.resize(samples, 0.0f);
        }
    }
    if (reallocPending_.load(std::memory_order_relaxed) != 0)
    {
        reallocPending_.store(0, std::memory_order_release);
    }
}

void Track::reserveRecordHeadroom(std::size_t samples) noexcept
{
    std::unique_lock lock(*audioMutex_);
    if (samples == 0)
    {
        return;
    }
    for (auto& channelData : audio_)
    {
        if (channelData.capacity() < samples)
        {
            channelData.reserve(samples);
        }
    }
    prepareHistoryStorage(samples);
}

void Track::ensureCapacity(std::size_t samples)
{
    // Called from the audio thread during first-pass record. Do not take audioMutex_:
    // a unique lock here waits behind UI snapshot checks and freezes the kit once Rec starts.
    ensureCapacityUnlocked(samples);
}

void Track::importReplaceStereo(const float* left, const float* right, const std::size_t numFrames) noexcept
{
    std::unique_lock lock(*audioMutex_);
    clearOverdubUndoSnapshotUnlocked();
    rotationCount_ = 0;
    std::fill(livePages_.begin(), livePages_.end(), 0);
    cycleCount_ = 1;
    beatsPerCycle_ = 0;
    referenceBpm_ = 0.0;
    if (numFrames == 0)
    {
        recordedSamples_ = 0;
        lengthSamples_ = 0;
        return;
    }
    // Exact-size grow only. Do not clear() or reserve the live-record 180 s slab —
    // that free+alloc on the message thread contends the process heap with ASIO.
    for (auto& channelData : audio_)
    {
        if (channelData.size() < numFrames)
        {
            if (channelData.capacity() < numFrames)
            {
                channelData.reserve(numFrames);
            }
            channelData.resize(numFrames, 0.0f);
        }
    }
    if (left != nullptr)
    {
        std::memcpy(audio_[0].data(), left, numFrames * sizeof(float));
    }
    else
    {
        std::fill(audio_[0].begin(), audio_[0].begin() + static_cast<std::ptrdiff_t>(numFrames), 0.0f);
    }
    if (channels_ >= 2)
    {
        const float* srcR = (right != nullptr) ? right : left;
        if (srcR != nullptr)
        {
            std::memcpy(audio_[1].data(), srcR, numFrames * sizeof(float));
        }
        else
        {
            std::fill(audio_[1].begin(), audio_[1].begin() + static_cast<std::ptrdiff_t>(numFrames), 0.0f);
        }
    }
    recordedSamples_ = numFrames;
    lengthSamples_ = numFrames;
    prepareHistoryStorage(numFrames);
}

// Only called during device preparation/import, never to grow history from a UI poll.
void Track::reserveOverdubHistory(const std::size_t samples) noexcept
{
    std::unique_lock lock(*audioMutex_);
    if (samples >= 2) prepareHistoryStorage(samples);
}

void Track::prepareHistoryStorage(const std::size_t samples)
{
    const auto pages = (samples + kPageFrames - 1) / kPageFrames;
    livePages_.resize(std::max(livePages_.size(), pages), 0);
    writablePages_.resize(livePages_.size(), 0);
    for (auto& layer : undoLayers_) layer.pages.resize(livePages_.size(), 0);
    for (auto& layer : redoLayers_) layer.pages.resize(livePages_.size(), 0);
    saveSnapshot_.pages.resize(livePages_.size(), 0);
    for (auto& bank : historyBanks_)
    {
        if (bank.capacity >= samples) continue;
        bank.channels.resize(static_cast<std::size_t>(channels_));
        const auto bankId=static_cast<std::uint8_t>(&bank-historyBanks_.data()+1);
        std::size_t used=0;
        const auto include=[&](const std::vector<std::uint8_t>& table,std::size_t length) {
            const auto pages=std::min(table.size(),(length+kPageFrames-1)/kPageFrames);
            for(std::size_t page=0;page<pages;++page)if(table[page]==bankId)used=std::max(used,std::min(length,(page+1)*kPageFrames));
        };
        include(livePages_,audio_[0].size());
        for(int i=0;i<undoCount_;++i)include(undoLayers_[static_cast<std::size_t>(i)].pages,viewSpan(undoLayers_[static_cast<std::size_t>(i)].length,undoLayers_[static_cast<std::size_t>(i)].rotations,undoLayers_[static_cast<std::size_t>(i)].rotationCount));
        for(int i=0;i<redoCount_;++i)include(redoLayers_[static_cast<std::size_t>(i)].pages,viewSpan(redoLayers_[static_cast<std::size_t>(i)].length,redoLayers_[static_cast<std::size_t>(i)].rotations,redoLayers_[static_cast<std::size_t>(i)].rotationCount));
        used=std::min(used,bank.capacity);
        for (auto& channel : bank.channels)
        {
            // Deliberately uninitialised. Physical pages are touched only when an
            // overdub changes them; device preparation reserves address space.
            std::unique_ptr<float[]> grown(new float[samples]);
            if (channel != nullptr) std::memcpy(grown.get(), channel.get(), used * sizeof(float));
            channel = std::move(grown);
        }
        bank.capacity = samples;
    }
    invalidateWritablePages();
}

void Track::invalidateWritablePages() noexcept
{
    std::fill(writablePages_.begin(), writablePages_.end(), 0);
}

std::size_t Track::mapIndex(std::size_t index,const Rotations& rotations,std::size_t count) noexcept
{
    while(count>0) {const auto& rotation=rotations[--count];if(index<rotation.length)index=(index+rotation.offset)%rotation.length;}
    return index;
}
std::size_t Track::viewSpan(std::size_t length,const Rotations& rotations,std::size_t count) noexcept
{
    for(std::size_t i=0;i<count;++i)length=std::max(length,rotations[i].length);
    return length;
}
std::size_t Track::physicalIndex(const std::size_t index) const noexcept
{
    return mapIndex(index,rotations_,rotationCount_);
}

float Track::readStoredSample(const int channel, const std::size_t index) const noexcept
{
    const auto c = static_cast<std::size_t>(channel);
    if (c >= audio_.size() || index >= audio_[c].size()) return 0.0f;
    const auto page = index / kPageFrames;
    const auto bank = page < livePages_.size() ? livePages_[page] : 0;
    if (bank == 255) return 0.0f;
    return bank == 0 ? audio_[c][index] : historyBanks_[bank - 1].channels[c][index];
}

bool Track::makePageWritable(const std::size_t page) noexcept
{
    if (page >= livePages_.size()) return false;
    if (writablePages_[page] != 0) return true;
    unsigned used = 0;
    const auto pin=[&used](std::uint8_t bank) { if(bank != 255)used |= 1u << bank; };
    for (int i=0; i<undoCount_; ++i) pin(undoLayers_[static_cast<std::size_t>(i)].pages[page]);
    for (int i=0; i<redoCount_; ++i) pin(redoLayers_[static_cast<std::size_t>(i)].pages[page]);
    if (saveSnapshotState_.load(std::memory_order_acquire) == 2) pin(saveSnapshot_.pages[page]);
    const auto current = livePages_[page];
    if (current == 255 || (used & (1u << current)) != 0)
    {
        unsigned dest = 0;
        while ((used & (1u << dest)) != 0) ++dest;
        if (dest > historyBanks_.size()) return false;
        const auto begin = page * kPageFrames;
        const auto count = std::min(kPageFrames, audio_[0].size() - begin);
        if (dest != 0 && historyBanks_[dest-1].capacity < begin+count) return false;
        for (std::size_t c=0; c<audio_.size(); ++c)
        {
            float* target = dest == 0 ? audio_[c].data() : historyBanks_[dest-1].channels[c].get();
            if(current==255) std::fill_n(target+begin,count,0.0f);
            else {
                const float* source = current == 0 ? audio_[c].data() : historyBanks_[current-1].channels[c].get();
                std::memcpy(target+begin, source+begin, count*sizeof(float));
            }
        }
        livePages_[page] = static_cast<std::uint8_t>(dest);
    }
    writablePages_[page] = 1;
    return true;
}

bool Track::ensureRealtimeCapacity(const std::size_t samples) noexcept
{
    for (const auto& channel : audio_) if (channel.capacity() < samples) return false;
    for (auto& channel : audio_) if (channel.size() < samples) channel.resize(samples);
    return true;
}

void Track::silenceRange(std::size_t begin,const std::size_t end) noexcept
{
    if(end<=begin || end>bufferCapacity())return;
    while(begin<end) {
        auto physical=begin;
        auto contiguous=end-begin;
        for(std::size_t i=rotationCount_;i>0;) {
            const auto& rotation=rotations_[--i];
            if(physical<rotation.length) {
                contiguous=std::min(contiguous,rotation.length-physical);
                physical=(physical+rotation.offset)%rotation.length;
                contiguous=std::min(contiguous,rotation.length-physical);
            }
        }
        if(physical%kPageFrames==0 && contiguous>=kPageFrames && physical/kPageFrames<livePages_.size()) {
            livePages_[physical/kPageFrames]=255;writablePages_[physical/kPageFrames]=0;begin+=kPageFrames;
        } else {
            for(int channel=0;channel<channels_;++channel)writeStoredSample(channel,begin,0.0f);
            ++begin;
        }
    }
}

void Track::fillLayerFromLiveUnlocked(OverdubLayer& layer)
{
    const std::size_t n = lengthSamples_;
    layer.length = n;
    layer.cycleCount = cycleCount_;
    layer.beatsPerCycle = beatsPerCycle_;
    layer.referenceBpm = referenceBpm_;
    if (n < 2 || audio_.empty())
    {
        return;
    }
    layer.rotations = rotations_;
    layer.rotationCount = rotationCount_;
    const auto pages = (viewSpan(n,rotations_,rotationCount_) + kPageFrames - 1) / kPageFrames;
    std::copy_n(livePages_.begin(), pages, layer.pages.begin());
    invalidateWritablePages();
}

void Track::applyLayerToLiveUnlocked(const OverdubLayer& layer)
{
    const std::size_t n = layer.length;
    if (n < 2)
    {
        recordedSamples_ = 0;
        lengthSamples_ = 0;
        cycleCount_ = 1;
        beatsPerCycle_ = 0;
        return;
    }
    ensureCapacityUnlocked(n);
    const auto pages = (viewSpan(n,layer.rotations,layer.rotationCount) + kPageFrames - 1) / kPageFrames;
    std::copy_n(layer.pages.begin(), pages, livePages_.begin());
    rotations_ = layer.rotations;
    rotationCount_ = layer.rotationCount;
    invalidateWritablePages();
    lengthSamples_ = n;
    recordedSamples_ = n;
    cycleCount_ = std::max(1, layer.cycleCount);
    beatsPerCycle_ = layer.beatsPerCycle;
    referenceBpm_ = layer.referenceBpm;
}

void Track::publishLayerDepths() noexcept
{
    undoDepth_.store(undoCount_, std::memory_order_release);
    redoDepth_.store(redoCount_, std::memory_order_release);
}

void Track::captureOverdubUndoSnapshotUnlocked()
{
    const std::size_t n = lengthSamples_;
    if (n < 2 || audio_.empty())
    {
        return;
    }
    const int dest = undoCount_ >= kMaxOverdubLayers ? 0 : undoCount_;
    const auto& reserved = undoLayers_[static_cast<std::size_t>(dest)];
    if (reserved.pages.size() < (n + kPageFrames - 1) / kPageFrames) return;
    if (undoCount_ >= kMaxOverdubLayers)
    {
        for (int i = 0; i < kMaxOverdubLayers - 1; ++i)
        {
            std::swap(undoLayers_[static_cast<std::size_t>(i)], undoLayers_[static_cast<std::size_t>(i + 1)]);
        }
        fillLayerFromLiveUnlocked(undoLayers_[static_cast<std::size_t>(kMaxOverdubLayers - 1)]);
        undoCount_ = kMaxOverdubLayers;
    }
    else
    {
        fillLayerFromLiveUnlocked(undoLayers_[static_cast<std::size_t>(undoCount_)]);
        ++undoCount_;
    }
    redoCount_ = 0;
    publishLayerDepths();
}

void Track::captureOverdubUndoSnapshot() noexcept
{
    captureOverdubUndoSnapshotUnlocked();
}

void Track::restoreOverdubUndoSnapshot() noexcept
{
    if (undoCount_ <= 0)
    {
        return;
    }
    if (redoCount_ >= kMaxOverdubLayers)
    {
        for (int i = 0; i < kMaxOverdubLayers - 1; ++i)
        {
            std::swap(redoLayers_[static_cast<std::size_t>(i)], redoLayers_[static_cast<std::size_t>(i + 1)]);
        }
        fillLayerFromLiveUnlocked(redoLayers_[static_cast<std::size_t>(kMaxOverdubLayers - 1)]);
        redoCount_ = kMaxOverdubLayers;
    }
    else
    {
        fillLayerFromLiveUnlocked(redoLayers_[static_cast<std::size_t>(redoCount_)]);
        ++redoCount_;
    }
    --undoCount_;
    applyLayerToLiveUnlocked(undoLayers_[static_cast<std::size_t>(undoCount_)]);
    publishLayerDepths();
}

void Track::clearOverdubUndoSnapshotUnlocked()
{
    undoCount_ = 0;
    redoCount_ = 0;
    publishLayerDepths();
}

void Track::clearOverdubUndoSnapshot() noexcept
{
    clearOverdubUndoSnapshotUnlocked();
}

bool Track::hasOverdubUndoSnapshot() const noexcept
{
    return undoDepth_.load(std::memory_order_acquire) > 0;
}

void Track::captureOverdubRedoSnapshotUnlocked()
{
    const std::size_t n = lengthSamples_;
    if (n < 2 || audio_.empty())
    {
        return;
    }
    const int dest = redoCount_ >= kMaxOverdubLayers ? 0 : redoCount_;
    const auto& reserved = redoLayers_[static_cast<std::size_t>(dest)];
    if (reserved.pages.size() < (n + kPageFrames - 1) / kPageFrames) return;
    if (redoCount_ >= kMaxOverdubLayers)
    {
        for (int i = 0; i < kMaxOverdubLayers - 1; ++i)
        {
            std::swap(redoLayers_[static_cast<std::size_t>(i)], redoLayers_[static_cast<std::size_t>(i + 1)]);
        }
        fillLayerFromLiveUnlocked(redoLayers_[static_cast<std::size_t>(kMaxOverdubLayers - 1)]);
        redoCount_ = kMaxOverdubLayers;
    }
    else
    {
        fillLayerFromLiveUnlocked(redoLayers_[static_cast<std::size_t>(redoCount_)]);
        ++redoCount_;
    }
    publishLayerDepths();
}

void Track::captureOverdubRedoSnapshot() noexcept
{
    captureOverdubRedoSnapshotUnlocked();
}

void Track::restoreOverdubRedoSnapshot() noexcept
{
    if (redoCount_ <= 0)
    {
        return;
    }
    if (undoCount_ >= kMaxOverdubLayers)
    {
        for (int i = 0; i < kMaxOverdubLayers - 1; ++i)
        {
            std::swap(undoLayers_[static_cast<std::size_t>(i)], undoLayers_[static_cast<std::size_t>(i + 1)]);
        }
        fillLayerFromLiveUnlocked(undoLayers_[static_cast<std::size_t>(kMaxOverdubLayers - 1)]);
        undoCount_ = kMaxOverdubLayers;
    }
    else
    {
        fillLayerFromLiveUnlocked(undoLayers_[static_cast<std::size_t>(undoCount_)]);
        ++undoCount_;
    }
    --redoCount_;
    applyLayerToLiveUnlocked(redoLayers_[static_cast<std::size_t>(redoCount_)]);
    publishLayerDepths();
}

void Track::clearOverdubRedoSnapshotUnlocked()
{
    redoCount_ = 0;
    publishLayerDepths();
}

void Track::clearOverdubRedoSnapshot() noexcept
{
    clearOverdubRedoSnapshotUnlocked();
}

bool Track::hasOverdubRedoSnapshot() const noexcept
{
    return redoDepth_.load(std::memory_order_acquire) > 0;
}

int Track::overdubUndoDepth() const noexcept
{
    const int d = undoDepth_.load(std::memory_order_acquire);
    if (d < 0)
    {
        return 0;
    }
    return d > kMaxOverdubLayers ? kMaxOverdubLayers : d;
}

int Track::overdubRedoDepth() const noexcept
{
    const int d = redoDepth_.load(std::memory_order_acquire);
    if (d < 0)
    {
        return 0;
    }
    return d > kMaxOverdubLayers ? kMaxOverdubLayers : d;
}

bool Track::beginStateSnapshot() noexcept
{
    int idle=0;
    if(!saveSnapshotState_.compare_exchange_strong(idle,1,std::memory_order_acq_rel))return false;
    serviceSaveSnapshot();return true;
}
bool Track::copyStateSnapshot(std::vector<float>& left,std::vector<float>& right) const
{
    if(saveSnapshotState_.load(std::memory_order_acquire)!=2)return false;
    return copyLockedSpan(std::numeric_limits<std::size_t>::max(),left,right);
}

void Track::serviceSaveSnapshot() noexcept
{
    int requested=1;
    if (!saveSnapshotState_.compare_exchange_strong(requested,3,std::memory_order_acq_rel)) return;
    saveSnapshot_.length = lengthSamples_ >= 2 ? lengthSamples_ : recordedSamples_;
    saveSnapshot_.rotations = rotations_; saveSnapshot_.rotationCount = rotationCount_;
    const auto pages=(viewSpan(saveSnapshot_.length,rotations_,rotationCount_)+kPageFrames-1)/kPageFrames;
    std::copy_n(livePages_.begin(),pages,saveSnapshot_.pages.begin());
    invalidateWritablePages();
    saveSnapshotState_.store(2,std::memory_order_release);
}

bool Track::copyLockedSpan(const std::size_t sampleCount, std::vector<float>& left, std::vector<float>& right, bool* hasAudio) const
{
    if (hasAudio != nullptr) *hasAudio = true;
    const bool running=realtimeActive_.load(std::memory_order_acquire);
    const bool alreadyCaptured=sampleCount==std::numeric_limits<std::size_t>::max();
    if (running && !alreadyCaptured)
    {
        int idle=0;
        if(!saveSnapshotState_.compare_exchange_strong(idle,1,std::memory_order_acq_rel)) return false;
        const auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(500);
        while(saveSnapshotState_.load(std::memory_order_acquire)!=2)
        {
            if(std::chrono::steady_clock::now()>=until || !realtimeActive_.load(std::memory_order_acquire))
            {
                int pending=1;
                if(saveSnapshotState_.compare_exchange_strong(pending,0,std::memory_order_acq_rel)) return false;
                break;
            }
            std::this_thread::yield();
        }
    }
    struct Release { std::atomic<int>& state; ~Release() { state.store(0,std::memory_order_release); } } release{saveSnapshotState_};
    // Imports/device preparation may wait here; the audio callback never takes this mutex.
    std::shared_lock lock(*audioMutex_);
    const auto n=(running || alreadyCaptured) ? saveSnapshot_.length : std::min(sampleCount,audio_[0].size());
    if (hasAudio != nullptr) *hasAudio = n >= 2;
    if(n<2) { left.clear();right.clear();return false; }
    left.resize(n);right.resize(n);
    const auto& table=(running || alreadyCaptured) ? saveSnapshot_.pages : livePages_;
    const auto& rotations=(running || alreadyCaptured) ? saveSnapshot_.rotations : rotations_;
    const auto rotationCount=(running || alreadyCaptured) ? saveSnapshot_.rotationCount : rotationCount_;
    for(std::size_t i=0;i<n;++i)
    {
        const auto index=mapIndex(i,rotations,rotationCount);
        const auto page=index/kPageFrames;
        const unsigned bank=page<table.size() ? table[page] : 0;
        left[i]=bank==255 ? 0.0f : bank==0 ? audio_[0].data()[index] : historyBanks_[bank-1].channels[0][index];
        right[i]=channels_<2 ? left[i] : bank==255 ? 0.0f : (bank==0 ? audio_[1].data()[index] : historyBanks_[bank-1].channels[1][index]);
    }
    return true;
}

bool Track::copyLockedLoopAudio(std::vector<float>& left, std::vector<float>& right) const
{
    return copyLockedSpan(realtimeActive_.load(std::memory_order_acquire) ? 0 : lengthSamples_, left, right);
}

bool Track::copyLockedSaveAudio(std::vector<float>& left, std::vector<float>& right, bool* hasAudio) const
{
    if(realtimeActive_.load(std::memory_order_acquire)) return copyLockedSpan(0,left,right,hasAudio);
    std::size_t n = lengthSamples_;
    if (n < 2 && state_ == TrackState::Recording)
    {
        n = recordedSamples_;
    }
    return copyLockedSpan(n, left, right,hasAudio);
}

void Track::writeSample(int channel, std::size_t sampleIndex, float sampleValue) noexcept
{
    if (audioFrozen_.load(std::memory_order_relaxed))
    {
        return;
    }
    writeStoredSample(channel, sampleIndex, sampleValue);
}

void Track::writeStoredSample(int channel, std::size_t sampleIndex, float sampleValue) noexcept
{
    const auto c = static_cast<std::size_t>(channel);
    if (c >= audio_.size() || sampleIndex >= audio_[c].size())
    {
        return;
    }
    const auto physical = physicalIndex(sampleIndex);
    const auto page = physical / kPageFrames;
    if (page >= livePages_.size()) { audio_[c][physical] = sampleValue; return; }
    if (!makePageWritable(page)) return;
    const auto bank = livePages_[page];
    if (bank == 0) audio_[c][physical] = sampleValue;
    else historyBanks_[bank-1].channels[c][physical] = sampleValue;
}

bool Track::rotateLeft(const std::size_t amount) noexcept
{
    const auto n=lengthSamples_>0 ? lengthSamples_ : recordedSamples_;
    if(n<2 || amount%n==0)return true;
    if(rotationCount_>0 && rotations_[rotationCount_-1].length==n) {
        auto& rotation=rotations_[rotationCount_-1];rotation.offset=(rotation.offset+amount)%n;return true;
    }
    if(rotationCount_==rotations_.size())return false;
    rotations_[rotationCount_++]={n,amount%n};return true;
}

bool Track::reverseLiveAudio() noexcept
{
    std::unique_lock lock(*audioMutex_);
    const std::size_t n = lengthSamples_ > 0 ? lengthSamples_ : recordedSamples_;
    if (n < 2)
    {
        return false;
    }
    for (int c=0; c<channels_; ++c)
        for (std::size_t i=0; i<n/2; ++i)
        {
            const auto a=readStoredSample(c,physicalIndex(i)), b=readStoredSample(c,physicalIndex(n-1-i));
            writeStoredSample(c,i,b); writeStoredSample(c,n-1-i,a);
        }
    return true;
}

std::size_t Track::recordedSamples() const noexcept
{
    return recordedSamples_;
}

void Track::setRecordedSamples(std::size_t samples) noexcept
{
    recordedSamples_ = samples;
}

void Track::resampleAudio(const double oldRate, const double newRate)
{
    std::unique_lock lock(*audioMutex_);
    if (!(oldRate > 0) || !(newRate > 0) || std::abs(oldRate-newRate)<0.5 || audio_[0].empty()) return;
    const double ratio = newRate / oldRate;
    const auto scaled = [ratio](std::size_t n) { return n == 0 ? std::size_t(0) : std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(n*ratio))); };
    auto convert = [&](const std::vector<std::uint8_t>& pages, const std::size_t oldN,
                       const Rotations& rotations, const std::size_t rotationCount) {
        std::vector<std::vector<float>> result(static_cast<std::size_t>(channels_));
        if (oldN == 0) return result;
        auto read = [&](int c, std::int64_t i) {
            i %= static_cast<std::int64_t>(oldN); if (i < 0) i += oldN;
            auto index=static_cast<std::size_t>(i);
            index=mapIndex(index,rotations,rotationCount);
            const auto page=index/kPageFrames;
            const unsigned bank=page < pages.size() ? pages[page] : 0;
            if(bank==255)return 0.0f;
            return bank==0 ? audio_[static_cast<std::size_t>(c)][index]
                : historyBanks_[bank-1].channels[static_cast<std::size_t>(c)][index];
        };
        constexpr double pi=3.14159265358979323846;
        const double cutoff=std::min(1.0,ratio)*0.95;
        for(int c=0;c<channels_;++c) {
            auto& out=result[static_cast<std::size_t>(c)]; out.resize(scaled(oldN));
            for(std::size_t i=0;i<out.size();++i) {
                const double pos=static_cast<double>(i)/ratio;
                const auto center=static_cast<std::int64_t>(std::floor(pos));
                double sum=0, weight=0;
                for(int tap=-15;tap<=16;++tap) {
                    const double x=pos-static_cast<double>(center+tap);
                    const double a=pi*x*cutoff;
                    const double sinc=std::abs(a)<1e-12 ? 1.0 : std::sin(a)/a;
                    const double win=0.5+0.5*std::cos(pi*x/16.0);
                    const double k=sinc*win*cutoff;
                    sum+=read(c,center+tap)*k; weight+=k;
                }
                out[i]=static_cast<float>(sum/weight);
            }
        }
        return result;
    };
    // Decode every referenced version before replacing any of its backing banks.
    auto live=convert(livePages_,audio_[0].size(),rotations_,rotationCount_);
    std::array<std::vector<std::vector<float>>, kMaxOverdubLayers*2> versions;
    for(int i=0;i<undoCount_;++i) { const auto& l=undoLayers_[static_cast<std::size_t>(i)]; versions[static_cast<std::size_t>(i)]=convert(l.pages,l.length,l.rotations,l.rotationCount); }
    for(int i=0;i<redoCount_;++i) { const auto& l=redoLayers_[static_cast<std::size_t>(i)]; versions[static_cast<std::size_t>(i+kMaxOverdubLayers)]=convert(l.pages,l.length,l.rotations,l.rotationCount); }
    const auto oldLength=lengthSamples_, oldRecorded=recordedSamples_;
    for(std::size_t c=0;c<audio_.size();++c)audio_[c].assign(live[c].begin(),live[c].end());
    rotationCount_=0;
    lengthSamples_=scaled(oldLength); recordedSamples_=scaled(oldRecorded);
    prepareHistoryStorage(audio_[0].size());
    std::fill(livePages_.begin(),livePages_.end(),0);
    for(std::size_t i=0;i<versions.size();++i) {
        auto& layer=i<kMaxOverdubLayers ? undoLayers_[i] : redoLayers_[i-kMaxOverdubLayers];
        if(versions[i].empty()) continue;
        layer.length=scaled(layer.length);layer.rotationCount=0;
        for(std::size_t c=0;c<versions[i].size();++c)
            std::memcpy(historyBanks_[i].channels[c].get(),versions[i][c].data(),versions[i][c].size()*sizeof(float));
        std::fill(layer.pages.begin(),layer.pages.end(),static_cast<std::uint8_t>(i+1));
    }
    invalidateWritablePages();
}

void Track::setReferenceBpm(const double bpm) noexcept
{
    referenceBpm_ = bpm;
}

double Track::referenceBpm() const noexcept
{
    return referenceBpm_;
}
} // namespace retrolooper
