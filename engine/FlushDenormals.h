// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>

#if defined(_M_X64) || defined(_M_IX86) || defined(__SSE2__)
#include <pmmintrin.h>
#include <xmmintrin.h>
#endif

namespace retrolooper
{
/** Keeps tiny floats from stalling the audio thread. SSE on Intel, FPCR.FZ on ARM64. */
struct ScopedNoDenormals
{
#if defined(_M_X64) || defined(_M_IX86) || defined(__SSE2__)
    unsigned old_{0};
    ScopedNoDenormals() noexcept
    {
        old_ = _mm_getcsr();
        _mm_setcsr(old_ | 0x8040u);
    }
    ~ScopedNoDenormals() noexcept { _mm_setcsr(old_); }
#elif defined(__aarch64__)
    std::uint64_t old_{0};
    ScopedNoDenormals() noexcept
    {
        asm volatile("mrs %0, fpcr" : "=r"(old_));
        asm volatile("msr fpcr, %0" :: "r"(old_ | (std::uint64_t{1} << 24)));
    }
    ~ScopedNoDenormals() noexcept
    {
        asm volatile("msr fpcr, %0" :: "r"(old_));
    }
#else
    ScopedNoDenormals() noexcept {}
#endif
};
} // namespace retrolooper
