// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "EngineTypes.h"
#include "Track.h"

#include <cstdint>

namespace retrolooper
{
enum class LoopTrackRuntimeState : std::uint8_t
{
    Empty = 0,
    Armed,
    RecordingFirstPass,
    Playing,
    Overdubbing,
    Stopped,
    Replacing,
    Clearing,
    Muted,
    Error
};

enum class LoopTrackCommand : std::uint8_t
{
    Record,
    Play,
    Stop,
    Overdub,
    Clear
};

enum class TrackBehaviorMode : std::uint8_t
{
    RecordPlayOverdubPlay = 0,
    RecordOverdubPlay = 1,
    SeparateCommands = 2
};

enum class TransitionError : std::uint8_t
{
    None = 0,
    IllegalCommand,
    MissingLoopContent
};

struct LoopTrackTransitionResult
{
    TrackState nextTrackState = TrackState::Empty;
    bool clearLoop = false;
    bool finalizeFirstPass = false;
    bool shouldCaptureOverdubUndo = false;
    bool noOp = false;
    TransitionError error = TransitionError::None;
};

[[nodiscard]] LoopTrackRuntimeState runtimeStateForTrack(const Track& track) noexcept;

[[nodiscard]] LoopTrackTransitionResult resolveLoopTrackTransition(LoopTrackRuntimeState current,
    LoopTrackCommand command,
    bool hasLoopContent,
    TrackBehaviorMode behaviorMode,
    AfterRecordMode afterRecordMode,
    bool dubModeEnabled) noexcept;
} // namespace retrolooper
