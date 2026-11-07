// Port-only public methods, included inside LooperEngine by build.py.
std::size_t hostThresholdFrames_=0,hostThresholdOffset_=0;
std::int64_t hostThresholdOrigin_=0;
// Use prepared storage as a circular sequence window. Silence uses page
// metadata; no second loop-sized buffer or audio rotation is needed.
void hostBeginThresholdCapture(std::size_t frames,std::size_t phase) noexcept {
    const auto pos=transport_.samplePosition();
    beginFirstPassRecording(0,pos);
    hostThresholdFrames_=frames;hostThresholdOffset_=phase%frames;
    hostThresholdOrigin_=pos-static_cast<std::int64_t>(hostThresholdOffset_);
    auto& tr=tracks_[0];tr.silenceRange(0,frames);tr.setLengthSamples(frames);
    sharedLoopPhaseOrigin_=hostThresholdOrigin_;
}
std::size_t hostCaptureWriteIndex(std::size_t track,std::size_t index) const noexcept {
    return track==0&&hostThresholdFrames_?(index+hostThresholdOffset_)%hostThresholdFrames_:index;
}
bool hostHasThresholdCapture() const noexcept { return hostThresholdFrames_!=0; }
void hostFinishThresholdCapture() noexcept { finishRecording(0,transport_.samplePosition()); }
void hostCompleteThresholdCapture(int track,std::int64_t pos) noexcept {
    if(track!=0||!hostThresholdFrames_)return;
    sharedLoopPhaseOrigin_=hostThresholdOrigin_;clearRecordPhaseAnchor(0);
    trackPhaseFromStart_[0]=true;trackPhaseOrigin_[0]=hostThresholdOrigin_;
    const auto length=static_cast<std::int64_t>(tracks_[0].lengthSamples());
    auto phase=(pos-hostThresholdOrigin_)%length;if(phase<0)phase+=length;
    trackPlayheads_[0]=static_cast<std::size_t>(phase);
    stretchPhaseOrigin_[0]=hostThresholdOrigin_;stretchSourceUnwrapped_[0]=trackPlayheads_[0];
    hostThresholdFrames_=hostThresholdOffset_=0;
}
void hostSnapshotSpan(std::size_t frames) noexcept { tracks_[0].setLengthSamples(frames); }
void hostAdvanceGap(int frames) noexcept { transport_.advance(frames); }
bool hostReverseAtCurrentPhase() noexcept {
    const auto phase=trackPlayheads_[0];
    if(!reverseTrack(0))return false;
    trackPlayheads_[0]=phase;resetStretchForTrack(0);return true;
}
bool hostFitLoopFrames(std::size_t frames,std::size_t sourceSpan,bool repeat) noexcept {
    auto& track=tracks_[0];
    if(frames<2||sourceSpan<2||!track.ensureRealtimeCapacity(std::max(frames,sourceSpan)))return false;
    if(repeat){for(int c=0;c<track.channels();++c)
        for(std::size_t i=sourceSpan;i<frames;++i)track.writeSample(c,i,track.readSample(c,i%sourceSpan));}
    else if(frames>sourceSpan)track.silenceRange(sourceSpan,frames);
    track.setLengthSamples(frames);track.setRecordedSamples(frames);
    track.setCycleCount(1);trackPlayheads_[0]%=frames;trackWriteheads_[0]=std::min(trackWriteheads_[0],frames);
    resetStretchForTrack(0);
    if(waveformEnvelopes_)waveformEnvelopes_->rebuildFromTrack(0,track,0,true);
    return true;
}

// Audio-thread selection gate: cancel future capture cues and end live capture
// at this block boundary, retaining playback and its existing phase.
void hostEndCaptureForSelection(bool recordPlay,bool playing) noexcept {
    cancelScheduledCommandsForTrack(0,CommandType::RecordToggle);
    cancelScheduledCommandsForTrack(0,CommandType::OverdubToggle);
    if(recordPlay){cancelScheduledCommandsForTrack(0,CommandType::Play);cancelScheduledCommandsForTrack(0,CommandType::PlayStopToggle);}
    auto& tr=tracks_[0];const auto pos=transport_.samplePosition();
    if(tr.state()==TrackState::Recording)applyCommand({CommandType::RecordToggle,0},pos,pos);
    if(trackStateIsOverdub(tr.state()))applyCommand({CommandType::OverdubToggle,0},pos,pos);
    if(tr.state()==TrackState::Playing_DubPending)tr.setState(TrackState::Playing);
    if(tr.state()==TrackState::Armed)tr.setState(tr.lengthSamples()>0?(playing?TrackState::Playing:TrackState::Stopped):TrackState::Empty);
}
