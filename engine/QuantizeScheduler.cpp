// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#include "QuantizeScheduler.h"

namespace retrolooper
{
void QuantizeScheduler::setGridDivision(int beatsPerAction) noexcept
{
    beatsPerAction_ = beatsPerAction > 0 ? beatsPerAction : 1;
}

std::int64_t QuantizeScheduler::quantizeToNextGrid(std::int64_t samplePosition, double samplesPerBeat) const noexcept
{
    const auto grid = static_cast<std::int64_t>(samplesPerBeat * static_cast<double>(beatsPerAction_));
    if (grid <= 0)
    {
        return samplePosition;
    }

    const auto next = ((samplePosition + grid - 1) / grid) * grid;
    return next;
}
} // namespace retrolooper
