// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>

namespace retrolooper
{
class QuantizeScheduler
{
  public:
    void setGridDivision(int beatsPerAction) noexcept;
    [[nodiscard]] std::int64_t quantizeToNextGrid(std::int64_t samplePosition, double samplesPerBeat) const noexcept;

  private:
    int beatsPerAction_ = 1;
};
} // namespace retrolooper
