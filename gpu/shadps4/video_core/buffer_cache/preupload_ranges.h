// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (BB_PREUPLOAD): the guest GPU memory the pre-upload walks, start -> end.
#pragma once

#include <cstdint>
#include <iterator>
#include <map>
#include <utility>
#include <vector>

namespace VideoCore {

using PreuploadRanges = std::map<std::uint64_t, std::uint64_t>;

/// [addr, end) mapped or unmapped: what overlaps it is cut out, a mapping joins its neighbours
/// (the game maps its GPU memory in many small pieces).
inline void PreuploadNoteRange(PreuploadRanges& ranges, std::uint64_t addr, std::uint64_t end,
                               bool mapped) {
    auto it = ranges.lower_bound(addr);
    if (it != ranges.begin()) {
        it = std::prev(it);
    }
    while (it != ranges.end() && it->first < end) {
        const std::uint64_t start = it->first, stop = it->second;
        if (stop <= addr) {
            ++it;
            continue;
        }
        it = ranges.erase(it);
        if (start < addr) {
            ranges[start] = addr;
        }
        if (stop > end) {
            ranges[end] = stop;
        }
    }
    if (!mapped) {
        return;
    }
    std::uint64_t stop = end;
    if (auto next = ranges.find(end); next != ranges.end()) {
        stop = next->second;
        ranges.erase(next);
    }
    if (auto next = ranges.lower_bound(addr);
        next != ranges.begin() && std::prev(next)->second == addr) {
        std::prev(next)->second = stop;
        return;
    }
    ranges[addr] = stop;
}

/// The ranges a walk starting at `cursor` reaches: from the one holding it (or the next one),
/// wrapping around once, until they span `max_regions` regions of 1 << `region_bits` bytes.
inline std::vector<std::pair<std::uint64_t, std::uint64_t>> PreuploadWalk(
    const PreuploadRanges& ranges, std::uint64_t cursor, std::uint64_t max_regions,
    unsigned region_bits) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
    auto it = ranges.upper_bound(cursor);
    if (it != ranges.begin() && std::prev(it)->second > cursor) {
        --it;
    }
    std::uint64_t regions = 0;
    for (std::size_t n = 0; n < ranges.size() && regions < max_regions; ++n) {
        if (it == ranges.end()) {
            it = ranges.begin();
        }
        out.push_back(*it);
        regions += ((it->second - 1) >> region_bits) - (it->first >> region_bits) + 1;
        ++it;
    }
    return out;
}

} // namespace VideoCore
