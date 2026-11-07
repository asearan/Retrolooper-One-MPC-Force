// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
// SPDX-License-Identifier: GPL-3.0-only
#include "TaggedTempo.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <string>

namespace retrolooper
{
namespace
{
[[nodiscard]] bool isAlpha(const char c) noexcept
{
    return std::isalpha(static_cast<unsigned char>(c)) != 0;
}

[[nodiscard]] bool isDigit(const char c) noexcept
{
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
}

[[nodiscard]] bool isSep(const char c) noexcept
{
    switch (c)
    {
        case ' ':
        case '\t':
        case '_':
        case '-':
        case '=':
        case ':':
        case '[':
        case ']':
        case '(':
        case ')':
            return true;
        default:
            return false;
    }
}

[[nodiscard]] bool plausibleTaggedBpm(const double bpm) noexcept
{
    return std::isfinite(bpm) && bpm >= 30.0 && bpm <= 320.0;
}

[[nodiscard]] double parseNumberAt(const std::string& s, const std::size_t i) noexcept
{
    if (i >= s.size() || !isDigit(s[i]))
    {
        return 0.0;
    }
    std::size_t j = i;
    while (j < s.size() && isDigit(s[j]))
    {
        ++j;
    }
    if (j < s.size() && s[j] == '.' && j + 1 < s.size() && isDigit(s[j + 1]))
    {
        ++j;
        while (j < s.size() && isDigit(s[j]))
        {
            ++j;
        }
    }
    if (j - i > 5)
    {
        return 0.0;
    }
    const double bpm = std::strtod(s.c_str() + static_cast<std::ptrdiff_t>(i), nullptr);
    return plausibleTaggedBpm(bpm) ? bpm : 0.0;
}

[[nodiscard]] std::size_t skipSepForward(const std::string& s, std::size_t i) noexcept
{
    while (i < s.size() && isSep(s[i]))
    {
        ++i;
    }
    return i;
}

[[nodiscard]] std::size_t skipSepBack(const std::string& s, std::size_t i) noexcept
{
    while (i > 0 && isSep(s[i - 1]))
    {
        --i;
    }
    return i;
}

[[nodiscard]] bool matchKeywordAt(const std::string& s, const std::size_t i, const char* kw) noexcept
{
    const std::size_t n = std::char_traits<char>::length(kw);
    if (i + n > s.size())
    {
        return false;
    }
    for (std::size_t k = 0; k < n; ++k)
    {
        if (s[i + k] != kw[k])
        {
            return false;
        }
    }
    // Digits may sit against the keyword (`128bpm`, `bpm140`); letters may not (`subbpm`).
    const bool leftOk = (i == 0) || !isAlpha(s[i - 1]);
    const bool rightOk = (i + n == s.size()) || !isAlpha(s[i + n]);
    return leftOk && rightOk;
}

[[nodiscard]] double numberBefore(const std::string& s, const std::size_t keywordStart) noexcept
{
    const std::size_t end = skipSepBack(s, keywordStart);
    if (end == 0)
    {
        return 0.0;
    }
    std::size_t start = end;
    bool sawDot = false;
    while (start > 0)
    {
        const char c = s[start - 1];
        if (isDigit(c))
        {
            --start;
            continue;
        }
        if (c == '.' && !sawDot)
        {
            sawDot = true;
            --start;
            continue;
        }
        break;
    }
    if (start >= end)
    {
        return 0.0;
    }
    return parseNumberAt(s, start);
}

[[nodiscard]] double findAroundKeyword(const std::string& s, const char* kw) noexcept
{
    const std::size_t n = std::char_traits<char>::length(kw);
    for (std::size_t i = 0; i + n <= s.size(); ++i)
    {
        if (!matchKeywordAt(s, i, kw))
        {
            continue;
        }
        const double after = parseNumberAt(s, skipSepForward(s, i + n));
        if (after > 0.0)
        {
            return after;
        }
        const double before = numberBefore(s, i);
        if (before > 0.0)
        {
            return before;
        }
    }
    return 0.0;
}
} // namespace

double parseTaggedTempoBpmFromText(const std::string_view text) noexcept
{
    if (text.empty())
    {
        return 0.0;
    }
    std::string s(text);
    std::transform(s.begin(), s.end(), s.begin(),
        [](const char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });

    if (const double bpm = findAroundKeyword(s, "bpm"); bpm > 0.0)
    {
        return bpm;
    }
    return findAroundKeyword(s, "tempo");
}

double parseDelimitedFilenameTempoBpm(const std::string_view fileStem) noexcept
{
    if (fileStem.empty())
    {
        return 0.0;
    }
    const std::string s(fileStem);
    const std::size_t n = s.size();
    for (std::size_t i = 0; i < n; ++i)
    {
        if (!isDigit(s[i]))
        {
            continue;
        }
        if (i > 0 && s[i - 1] != '_' && s[i - 1] != ' ')
        {
            continue;
        }
        std::size_t j = i;
        while (j < n && isDigit(s[j]))
        {
            ++j;
        }
        if (j < n && s[j] != '_' && s[j] != ' ')
        {
            i = j;
            continue;
        }
        const std::size_t digits = j - i;
        if (digits == 2 || digits == 3)
        {
            const double bpm = std::strtod(s.c_str() + static_cast<std::ptrdiff_t>(i), nullptr);
            if (bpm >= 60.0 && bpm <= 200.0)
            {
                return bpm;
            }
        }
        i = j;
    }
    return 0.0;
}

double parseTaggedTempoBpmFromFileNames(const std::string_view fileStem, const std::string_view parentFolder) noexcept
{
    if (const double fromStem = parseTaggedTempoBpmFromText(fileStem); fromStem > 0.0)
    {
        return fromStem;
    }
    return parseTaggedTempoBpmFromText(parentFolder);
}
} // namespace retrolooper
