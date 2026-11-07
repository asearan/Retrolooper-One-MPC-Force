// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <string_view>

namespace retrolooper
{
/**
 * Parse a tempo from file names, titles, or metadata text.
 * Matches tokens such as {@code 128bpm}, {@code 90 BPM}, {@code bpm=140}, {@code tempo 87.5}.
 * Ignores bare numbers (catalog IDs, years, sample rates).
 * @return BPM in 30–320, or 0 if none found.
 */
[[nodiscard]] double parseTaggedTempoBpmFromText(std::string_view text) noexcept;

/**
 * A tempo written as its own filename token, with no bpm label.
 * {@code TAMUZ_TD_90_drum} is 90. {@code TAMUZ_TD_95_shaker} is 95.
 * Hyphenated ids such as {@code loop-120}, years, and sample rates stay 0.
 */
[[nodiscard]] double parseDelimitedFilenameTempoBpm(std::string_view fileStem) noexcept;

/** Filename stem first, then parent folder (e.g. {@code DrumsFull_98bpm}, {@code AFT 1 - 98 bpm Key Am}). */
[[nodiscard]] double parseTaggedTempoBpmFromFileNames(std::string_view fileStem, std::string_view parentFolder) noexcept;
} // namespace retrolooper
