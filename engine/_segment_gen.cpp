// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
void LooperEngine::processTrackAudioSegment(const std::int64_t fullBlockTransportStart,
    const int offsetInBlock,
    const int segSamples,
    const float* const* hardwareInput,
    const int numHardwareInputChannels,
    const float* instrumentInputLeft,
    const float* instrumentInputRight,
    const int numInstrumentChannels,
    float* const* output,
    const int numOutputChannels,
    float& inputPeakInOut,
    metering::TrackMeterBlockAccum& meterAccum,
    const float* const* trackRecordInput,
    const int numTrackRecordChannels) noexcept
{
    const std::int64_t segmentBaseTs = fullBlockTransportStart + static_cast<std::int64_t>(offsetInBlock);
    const std::int64_t segmentEndTs = segmentBaseTs + static_cast<std::int64_t>(segSamples);
    const bool haveInstrument = (numInstrumentChannels > 0 && instrumentInputLeft != nullptr);
    const bool haveHardware = (numHardwareInputChannels > 0 && hardwareInput != nullptr);
    if (segSamples > 0
        && (mixVolScratch_.size() < static_cast<std::size_t>(segSamples)
            || stretchOutL_.size() < static_cast<std::size_t>(segSamples)))
    {
        return;
    }
    for (int trackIndex = 0; trackIndex < kMaxTracks; ++trackIndex)
    {
        auto& track = tracks_[static_cast<std::size_t>(trackIndex)];
        const auto state = track.state();
        const auto len = track.lengthSamples();
        const auto mix = track.mix();
        const auto& io = trackIo_[static_cast<std::size_t>(trackIndex)];
        const auto ti = static_cast<std::size_t>(trackIndex);
        const bool haveTrackRecord = trackRecordInput != nullptr
            && numTrackRecordChannels >= (trackIndex + 1) * 2
            && (trackRecordInput[trackIndex * 2] != nullptr || trackRecordInput[trackIndex * 2 + 1] != nullptr);
        const bool canRecord = haveHardware || haveInstrument || haveTrackRecord;
        const bool capturing = state == TrackState::Recording || trackStateIsOverdub(state);
        const bool needInputPath = canRecord && (capturing || mix.directMonitor);
        const bool stopFadingEarly = state == TrackState::Stopped && len > 0 && muteGainTarget_[ti] <= 0.0f
            && muteGain_[ti] > 1.0e-4f;
        const bool stopSilentEarly = state == TrackState::Stopped && len > 0 && !stopFadingEarly;
        const bool feedLoopEarly = (trackStateAdvancesPlayhead(state) && len > 0) || stopFadingEarly;
        if (!needInputPath && !feedLoopEarly && !stopSilentEarly)
        {
            continue;
        }
        if (needInputPath || feedLoopEarly)
        {
            track.renderSmoothedMix(segSamples,
                mixVolScratch_.data(),
                mixPanScratch_.data(),
                mixInScratch_.data(),
                mixFxScratch_.data());
        }

        const double refBpm = track.referenceBpm();
        const double masterBpm = (blockMasterBpm_ > 1.0) ? blockMasterBpm_ : transport_.tempoBpm();
        // The transport boots at 120. That is not a session tempo, so an import must
        // play at its own speed until a recording or the user actually sets one.
        const bool canVarispeed = tempoManager_.isLocked() && refBpm > 1.0e-9 && len > 0
            && trackStateShouldTimeStretch(state);
        const double stretchRatio = canVarispeed ? playbackRateFromBpm(masterBpm, refBpm) : 1.0;
        const bool useStretch = canVarispeed && ratioNeedsVarispeed(stretchRatio);

        const std::int64_t phaseOrigin = playbackPhaseOrigin(trackIndex);

        if (needInputPath)
        {
            for (int sample = 0; sample < segSamples; ++sample)
            {
                const float in0 = readRecordChannel(io,
                    0,
                    offsetInBlock + sample,
                    hardwareInput,
                    numHardwareInputChannels,
                    instrumentInputLeft,
                    instrumentInputRight,
                    numInstrumentChannels,
                    trackIndex,
                    trackRecordInput,
                    numTrackRecordChannels)
                    * mixInScratch_[static_cast<std::size_t>(sample)];
                const float in1 = readRecordChannel(io,
                    1,
                    offsetInBlock + sample,
                    hardwareInput,
                    numHardwareInputChannels,
                    instrumentInputLeft,
                    instrumentInputRight,
                    numInstrumentChannels,
                    trackIndex,
                    trackRecordInput,
                    numTrackRecordChannels)
                    * mixInScratch_[static_cast<std::size_t>(sample)];
                inputPeakInOut = std::max(inputPeakInOut, std::max(std::abs(in0), std::abs(in1)));
                metering::accumulateInputChannel(meterAccum, ti, 0, in0);
                if (track.channels() > 1)
                {
                    metering::accumulateInputChannel(meterAccum, ti, 1, in1);
                }
                if (mix.directMonitor)
                {
                    const float pan = mixPanScratch_[static_cast<std::size_t>(sample)];
                    float gl = 1.0f;
                    float gr = 1.0f;
                    stereoPanGains(pan, gl, gr);
                    const float l = in0 * gl;
                    const float r = in1 * gr;
                    routePlaybackSample(io, output, numOutputChannels, offsetInBlock + sample, l, r);
                }
            }
        }

        if ((state == TrackState::Recording || trackStateIsOverdub(state)) && canRecord)
        {
            const std::int64_t recordInputGate = trackRecordInputGateTransport_[ti];
            const auto recordLimit = static_cast<std::size_t>(transport_.sampleRate() * kMaxFirstPassSeconds);
            if (state == TrackState::Recording)
                (void) track.ensureRealtimeCapacity(std::min(recordLimit, trackWriteheads_[ti] + static_cast<std::size_t>(segSamples)));
            std::size_t whead = trackWriteheads_[ti];
            for (int sample = 0; sample < segSamples; ++sample)
            {
                const std::int64_t ts = segmentBaseTs + static_cast<std::int64_t>(sample);
                if (recordInputGate >= 0 && ts < recordInputGate)
                {
                    continue;
                }

                TrackState recSt = track.state();
                if (recSt == TrackState::Recording && whead >= recordLimit)
                {
                    trackWriteheads_[ti] = whead;
                    track.setRecordedSamples(whead);
                    finishRecordingToPlaying_[ti] = true;
                    finishRecording(trackIndex, ts);
                    recordLimitReached_.store(true, std::memory_order_release);
                    break;
                }
                if (recSt == TrackState::Recording)
                {
                    const std::int64_t pend = pendingRecordEndExclusive_[ti];
                    if (pend >= 0 && ts >= pend)
                    {
                        pendingRecordEndExclusive_[ti] = -1;
                        trackWriteheads_[ti] = whead;
                        track.setRecordedSamples(whead);
                        finishRecording(trackIndex, pend);
                        recSt = track.state();
                        if (recSt == TrackState::Playing)
                        {
                            break;
                        }
                    }
                }

                recSt = track.state();
                if (!(recSt == TrackState::Recording || trackStateIsOverdub(recSt)))
                {
                    break;
                }

                const std::size_t curLen = track.lengthSamples();
                float recL = 0.0f;
                float recR = 0.0f;
                for (int channel = 0; channel < track.channels(); ++channel)
                {
                    const float inputSample = readRecordChannel(
                        io,
                        channel,
                        offsetInBlock + sample,
                        hardwareInput,
                        numHardwareInputChannels,
                        instrumentInputLeft,
                        instrumentInputRight,
                        numInstrumentChannels,
                        trackIndex,
                        trackRecordInput,
                        numTrackRecordChannels)
                        * mixInScratch_[static_cast<std::size_t>(sample)];
                    if (channel == 0)
                    {
                        recL = inputSample;
                    }
                    else
                    {
                        recR = inputSample;
                    }
                    if (trackStateIsOverdub(recSt) && curLen > 0)
                    {
                        std::size_t ringPos = 0;
                        if (useStretch && !mix.muted && recordInputGate < 0)
                        {
                            double ph = static_cast<double>(ts - phaseOrigin) * stretchRatio;
                            ph = std::fmod(ph, static_cast<double>(curLen));
                            if (ph < 0.0)
                            {
                                ph += static_cast<double>(curLen);
                            }
                            ringPos = static_cast<std::size_t>(ph);
                        }
                        else
                        {
                            const int comp = effectiveRecordLatencyCompensationSamples(trackIndex);
                            ringPos = loopPositionFromTransport(ts - static_cast<std::int64_t>(comp), phaseOrigin, curLen);
                        }
                        const auto existing = track.readSample(channel, ringPos);
                        track.writeSample(channel, ringPos, existing * overdubFeedback_ + inputSample);
                    }
                    else if (recSt == TrackState::Recording)
                    {
                        track.writeSample(channel, whead, inputSample);
                    }
                }
                if (track.channels() < 2)
                {
                    recR = recL;
                }
                if (recSt == TrackState::Recording && isRecording_.load(std::memory_order_relaxed)
                    && tempoManager_.state() == TempoSyncState::Empty)
                {
                    liveTransientDetector_.processSample(recL, recR);
                }
                if (recSt == TrackState::Recording) ++whead;
            }
            trackWriteheads_[ti] = whead;
            track.setRecordedSamples(track.state() == TrackState::Recording
                ? trackWriteheads_[ti] : track.lengthSamples());
        }

        const TrackState playState = track.state();
        const std::size_t playLen = track.lengthSamples();
        const bool canVarispeedNow = tempoManager_.isLocked() && refBpm > 1.0e-9 && playLen > 0
            && trackStateShouldTimeStretch(playState);
        const double stretchRatioNow = canVarispeedNow ? playbackRateFromBpm(masterBpm, refBpm) : 1.0;
        const bool useStretchNow = canVarispeedNow && ratioNeedsVarispeed(stretchRatioNow);
        // Stopped: 5 ms pre-FX fade, then keep feeding silence so delay/reverb tails ring.
        const bool stopFading = playState == TrackState::Stopped && playLen > 0 && muteGainTarget_[ti] <= 0.0f
            && muteGain_[ti] > 1.0e-4f;
        const bool stopSilentSpillover = playState == TrackState::Stopped && playLen > 0 && !stopFading;
        const bool feedLoop = (trackStateAdvancesPlayhead(playState) && playLen > 0) || stopFading;
        const bool runInsert = (feedLoop || stopSilentSpillover) && trackIndex < 4;
        const bool drySilent = !stopFading && muteGain_[ti] <= 1.0e-4f && muteGainTarget_[ti] <= 0.0f;

        if (feedLoop || stopSilentSpillover)
        {
            if (stopSilentSpillover || (feedLoop && drySilent))
            {
                if (feedLoop && drySilent && useStretchNow && playLen > 0)
                {
                    const double loopLenD = static_cast<double>(playLen);
                    double musicalStart = std::fmod(static_cast<double>(segmentBaseTs - phaseOrigin) * stretchRatioNow, loopLenD);
                    if (musicalStart < 0.0)
                    {
                        musicalStart += loopLenD;
                    }
                    double phaseOut = musicalStart;
                    renderTimeStretchBlock(trackIndex, track, stretchRatioNow, segSamples,
                        stereoBusScratchL_.data(), stereoBusScratchR_.data(), musicalStart, phaseOut);
                }
                std::fill_n(stereoBusScratchL_.data(), segSamples, 0.0f);
                std::fill_n(stereoBusScratchR_.data(), segSamples, 0.0f);
            }
            else if (feedLoop)
            {
                std::size_t localPos = stopFading ? stopFadeReadPos_[ti] : 0;
                const float xg = trackCrossfadeGain(trackIndex);
                const bool volFlat = !track.volumeIsSmoothing();
                const bool panFlat = trackStereoBusProcessor_ != nullptr || !track.panIsSmoothing();
                const float volHold = volFlat ? mixVolScratch_[0] : 0.0f;
                float panHoldL = 1.0f;
                float panHoldR = 1.0f;
                if (panFlat)
                {
                    stereoPanGains(trackStereoBusProcessor_ != nullptr ? 0.0f : mixPanScratch_[0], panHoldL, panHoldR);
                }
                const double loopLenD = static_cast<double>(playLen);
                const int playChR = track.channels() > 1 ? 1 : 0;
                // Resampling by the tempo ratio shifts pitch. The Sound Crate preview keeps pitch
                // and only changes speed; imported playback has to do the same.
                if (useStretchNow && !stopFading && playLen > 0)
                {
                    double musicalStart = std::fmod(static_cast<double>(segmentBaseTs - phaseOrigin) * stretchRatioNow, loopLenD);
                    if (musicalStart < 0.0)
                    {
                        musicalStart += loopLenD;
                    }
                    double phaseOut = musicalStart;
                    renderTimeStretchBlock(trackIndex, track, stretchRatioNow, segSamples,
                        stereoBusScratchL_.data(), stereoBusScratchR_.data(), musicalStart, phaseOut);
                    for (int sample = 0; sample < segSamples; ++sample)
                    {
                        const float vol = volFlat ? volHold : mixVolScratch_[static_cast<std::size_t>(sample)];
                        float gl = panHoldL;
                        float gr = panHoldR;
                        if (!panFlat)
                        {
                            stereoPanGains(mixPanScratch_[static_cast<std::size_t>(sample)], gl, gr);
                        }
                        stereoBusScratchL_[static_cast<std::size_t>(sample)] *= vol * gl * xg;
                        stereoBusScratchR_[static_cast<std::size_t>(sample)] *= vol * gr * xg;
                    }
                }
                else
                {
                for (int sample = 0; sample < segSamples; ++sample)
                {
                    const std::int64_t ts = segmentBaseTs + static_cast<std::int64_t>(sample);
                    const float vol = volFlat ? volHold : mixVolScratch_[static_cast<std::size_t>(sample)];
                    float rawL = 0.0f;
                    float rawR = 0.0f;
                    if (stopFading)
                    {
                        const auto loopPos = playLen > 0 ? (localPos % playLen) : 0;
                        rawL = readLoopSampleSeamed(track, 0, loopPos, playLen, loopCrossfadeSamples_) * vol;
                        rawR = playChR == 0
                            ? rawL
                            : readLoopSampleSeamed(track, playChR, loopPos, playLen, loopCrossfadeSamples_) * vol;
                        if (playLen > 0)
                        {
                            localPos = (localPos + 1u) % playLen;
                        }
                    }
                    else
                    {
                        const auto loopPos = loopPositionFromTransport(ts, phaseOrigin, playLen);
                        rawL = readLoopSampleSeamed(track, 0, loopPos, playLen, loopCrossfadeSamples_) * vol;
                        rawR = playChR == 0
                            ? rawL
                            : readLoopSampleSeamed(track, playChR, loopPos, playLen, loopCrossfadeSamples_) * vol;
                    }
                    float gl = panHoldL;
                    float gr = panHoldR;
                    if (!panFlat)
                    {
                        stereoPanGains(mixPanScratch_[static_cast<std::size_t>(sample)], gl, gr);
                    }
                    stereoBusScratchL_[static_cast<std::size_t>(sample)] = rawL * gl * xg;
                    stereoBusScratchR_[static_cast<std::size_t>(sample)] = rawR * gr * xg;
                }
                }
                if (stopFading)
                {
                    stopFadeReadPos_[ti] = localPos;
                }
            }
            else
            {
                std::fill_n(stereoBusScratchL_.data(), segSamples, 0.0f);
                std::fill_n(stereoBusScratchR_.data(), segSamples, 0.0f);
            }

            // Pre-FX mute/stop gain: delay/reverb still see this buffer (silence after the fade).
            for (int sample = 0; sample < segSamples; ++sample)
            {
                const std::int64_t ts = segmentBaseTs + static_cast<std::int64_t>(sample);
                const float g = serialFadeInMultiplier(trackIndex, ts) * nextMuteGain(ti);
                stereoBusScratchL_[static_cast<std::size_t>(sample)] *= g;
                stereoBusScratchR_[static_cast<std::size_t>(sample)] *= g;
            }

            if (runInsert && trackStereoBusProcessor_)
            {
                // Mixer strip (gain/mute/pan/EQ/inserts) is in-line. fxSend must not dry-bypass it.
                trackStereoBusProcessor_(trackIndex, stereoBusScratchL_.data(), stereoBusScratchR_.data(), segSamples);
            }

            for (int sample = 0; sample < segSamples; ++sample)
            {
                const float l = stereoBusScratchL_[static_cast<std::size_t>(sample)];
                const float r = stereoBusScratchR_[static_cast<std::size_t>(sample)];
                metering::accumulateOutputStereo(meterAccum, ti, l, r);
                routePlaybackSample(io, output, numOutputChannels, offsetInBlock + sample, l, r);
            }

            if (trackStateAdvancesPlayhead(playState) && playLen > 0)
            {
                if (useStretchNow)
                {
                    const double relEndPlayback = static_cast<double>(segmentEndTs - phaseOrigin);
                    double ph = std::fmod(relEndPlayback * stretchRatioNow, static_cast<double>(playLen));
                    if (ph < 0.0)
                    {
                        ph += static_cast<double>(playLen);
                    }
                    trackPlayheads_[ti] = static_cast<std::size_t>(std::llround(ph)) % playLen;
                }
                else
                {
                    trackPlayheads_[ti] = loopPositionFromTransport(segmentEndTs, phaseOrigin, playLen);
                    stretchSourceUnwrapped_[ti] = static_cast<double>(trackPlayheads_[ti]);
                }
                stopFadeReadPos_[ti] = trackPlayheads_[ti];
            }
        }
    }
}
