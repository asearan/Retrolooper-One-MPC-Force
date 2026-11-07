// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#include "LoopTrackStateMachine.h"

namespace retrolooper
{
LoopTrackRuntimeState runtimeStateForTrack(const Track& track) noexcept
{
    const bool hasLoop = track.lengthSamples() > 0;
    switch (track.state())
    {
        case TrackState::Empty: return LoopTrackRuntimeState::Empty;
        case TrackState::Armed: return LoopTrackRuntimeState::Armed;
        case TrackState::Recording:
            return hasLoop ? LoopTrackRuntimeState::Replacing : LoopTrackRuntimeState::RecordingFirstPass;
        case TrackState::Playing:
        case TrackState::Playing_MutePending:
        case TrackState::Playing_DubPending:
            return LoopTrackRuntimeState::Playing;
        case TrackState::Overdub:
        case TrackState::Overdubbing_MutePending:
            return LoopTrackRuntimeState::Overdubbing;
        case TrackState::Muted:
        case TrackState::Muted_UnmutePending:
            return LoopTrackRuntimeState::Muted;
        case TrackState::Stopped: return hasLoop ? LoopTrackRuntimeState::Stopped : LoopTrackRuntimeState::Empty;
        default: return LoopTrackRuntimeState::Error;
    }
}

LoopTrackTransitionResult resolveLoopTrackTransition(const LoopTrackRuntimeState current,
    const LoopTrackCommand command,
    const bool hasLoopContent,
    const TrackBehaviorMode behaviorMode,
    const AfterRecordMode afterRecordMode,
    const bool dubModeEnabled) noexcept
{
    LoopTrackTransitionResult out{};
    out.nextTrackState = TrackState::Empty;
    (void) dubModeEnabled;

    switch (command)
    {
        case LoopTrackCommand::Clear:
            out.clearLoop = true;
            out.nextTrackState = TrackState::Empty;
            return out;

        case LoopTrackCommand::Stop:
            if (current == LoopTrackRuntimeState::RecordingFirstPass || current == LoopTrackRuntimeState::Replacing)
            {
                out.finalizeFirstPass = true;
                out.nextTrackState = TrackState::Stopped;
                return out;
            }
            if (current == LoopTrackRuntimeState::Playing || current == LoopTrackRuntimeState::Overdubbing
                || current == LoopTrackRuntimeState::Stopped || current == LoopTrackRuntimeState::Muted)
            {
                out.nextTrackState = hasLoopContent ? TrackState::Stopped : TrackState::Empty;
                return out;
            }
            out.noOp = true;
            out.error = TransitionError::IllegalCommand;
            return out;

        case LoopTrackCommand::Play:
            if (current == LoopTrackRuntimeState::RecordingFirstPass || current == LoopTrackRuntimeState::Replacing)
            {
                out.finalizeFirstPass = true;
                out.nextTrackState = TrackState::Playing;
                return out;
            }
            if (!hasLoopContent)
            {
                out.noOp = true;
                out.error = TransitionError::MissingLoopContent;
                return out;
            }
            if (current == LoopTrackRuntimeState::Stopped || current == LoopTrackRuntimeState::Muted)
            {
                out.nextTrackState = TrackState::Playing;
                return out;
            }
            if (current == LoopTrackRuntimeState::Overdubbing)
            {
                out.nextTrackState = TrackState::Playing;
                return out;
            }
            out.noOp = true;
            return out;

        case LoopTrackCommand::Overdub:
            if (!hasLoopContent)
            {
                out.noOp = true;
                out.error = TransitionError::MissingLoopContent;
                return out;
            }
            if (current == LoopTrackRuntimeState::Playing || current == LoopTrackRuntimeState::Muted)
            {
                out.nextTrackState = TrackState::Overdub;
                out.shouldCaptureOverdubUndo = true;
                return out;
            }
            if (current == LoopTrackRuntimeState::Overdubbing)
            {
                out.nextTrackState = TrackState::Playing;
                return out;
            }
            out.noOp = true;
            out.error = TransitionError::IllegalCommand;
            return out;

        case LoopTrackCommand::Record:
            if (current == LoopTrackRuntimeState::Empty || current == LoopTrackRuntimeState::Armed)
            {
                out.nextTrackState = TrackState::Recording;
                return out;
            }
            if (current == LoopTrackRuntimeState::RecordingFirstPass || current == LoopTrackRuntimeState::Replacing)
            {
                out.finalizeFirstPass = true;
                const bool enterOverdub = afterRecordMode == AfterRecordMode::Dub;
                out.nextTrackState = enterOverdub ? TrackState::Overdub : TrackState::Playing;
                return out;
            }
            if (current == LoopTrackRuntimeState::Playing || current == LoopTrackRuntimeState::Muted)
            {
                if (behaviorMode == TrackBehaviorMode::SeparateCommands)
                {
                    out.noOp = true;
                    return out;
                }
                out.nextTrackState = TrackState::Overdub;
                out.shouldCaptureOverdubUndo = true;
                return out;
            }
            if (current == LoopTrackRuntimeState::Overdubbing)
            {
                if (behaviorMode == TrackBehaviorMode::SeparateCommands)
                {
                    out.noOp = true;
                    return out;
                }
                out.nextTrackState = TrackState::Playing;
                return out;
            }
            if (current == LoopTrackRuntimeState::Stopped && hasLoopContent)
            {
                if (behaviorMode == TrackBehaviorMode::SeparateCommands)
                {
                    out.noOp = true;
                    return out;
                }
                // Rec always punches in. Play is the button that resumes a stopped loop.
                out.nextTrackState = TrackState::Overdub;
                out.shouldCaptureOverdubUndo = true;
                return out;
            }
            out.noOp = true;
            out.error = TransitionError::IllegalCommand;
            return out;
    }

    out.noOp = true;
    out.error = TransitionError::IllegalCommand;
    return out;
}
} // namespace retrolooper
