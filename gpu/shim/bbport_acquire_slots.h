// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <vector>

namespace BbPresent {

/// bbport: the swapchain's acquire semaphores are used in turn (one per slot). A slot's semaphore
/// may only be acquired with again once the submission that waits for it has run on the GPU:
/// signalling a binary semaphore whose wait is still queued is invalid, and with the present
/// thread running ahead of the GPU (high frame rates) a wait could end up never signalled, which
/// holds every later submission on the queue. Each slot keeps the tick of the present
/// scheduler's submission that waits for it (0: none).
class AcquireSlots {
public:
    void Reset(std::uint32_t count) {
        ticks.assign(count, 0);
    }
    /// The tick to wait for before acquiring with the slot's semaphore again (0: none).
    std::uint64_t Pending(std::uint32_t slot) const {
        return slot < ticks.size() ? ticks[slot] : 0;
    }
    /// The submission with `tick` waits for the slot's semaphore.
    void Note(std::uint32_t slot, std::uint64_t tick) {
        if (slot < ticks.size()) {
            ticks[slot] = tick;
        }
    }

private:
    std::vector<std::uint64_t> ticks;
};

} // namespace BbPresent
