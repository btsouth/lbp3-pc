// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: one bit per tracked region (4 MiB), set while the region may hold CPU-modified pages.
// Lock-free checks of large bindings read the bits instead of every region (MemoryTracker).
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace VideoCore {

template <std::size_t NumRegions>
class RegionSummary {
public:
    static_assert(NumRegions % 64 == 0);

    /// Regions not created yet count as modified (they start all CPU-modified).
    RegionSummary() {
        for (auto& word : words) {
            word.store(~0ULL, std::memory_order_relaxed);
        }
    }

    std::atomic<std::uint64_t>* Word(std::size_t region) {
        return &words[region / 64];
    }
    static constexpr std::uint64_t Mask(std::size_t region) {
        return 1ULL << (region % 64);
    }

    /// Whether a bit of regions [first, last] is set; out of range counts as set.
    bool Any(std::uint64_t first, std::uint64_t last) const noexcept {
        if (last >= NumRegions || first > last) {
            return true;
        }
        for (std::uint64_t word = first / 64; word <= last / 64; ++word) {
            std::uint64_t bits = words[word].load(std::memory_order_acquire);
            if (word == first / 64) {
                bits &= ~0ULL << (first % 64);
            }
            if (word == last / 64 && last % 64 != 63) {
                bits &= (1ULL << (last % 64 + 1)) - 1;
            }
            if (bits) {
                return true;
            }
        }
        return false;
    }

private:
    std::array<std::atomic<std::uint64_t>, NumRegions / 64> words;
};

} // namespace VideoCore
