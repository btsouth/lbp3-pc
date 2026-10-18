// SPDX-License-Identifier: GPL-2.0-or-later
// BbPresent::AcquireSlots: an acquire semaphore is reused only after the present that waited it.
#include <cassert>
#include <cstdio>
#include "bbport_acquire_slots.h"

int main() {
    BbPresent::AcquireSlots slots;
    slots.Reset(3);
    // Fresh semaphores: nothing to wait for.
    for (unsigned slot = 0; slot < 3; ++slot) {
        assert(slots.Pending(slot) == 0);
    }
    // Presents with ticks 5, 6, 7 wait slots 0, 1, 2; the next acquire on slot 0 waits tick 5.
    slots.Note(0, 5);
    slots.Note(1, 6);
    slots.Note(2, 7);
    assert(slots.Pending(0) == 5 && slots.Pending(1) == 6 && slots.Pending(2) == 7);
    // A later present on the slot replaces the tick.
    slots.Note(0, 8);
    assert(slots.Pending(0) == 8);
    // A recreated swapchain has new semaphores (its old ones were waited for).
    slots.Reset(4);
    for (unsigned slot = 0; slot < 4; ++slot) {
        assert(slots.Pending(slot) == 0);
    }
    // Out of range slots are ignored.
    slots.Note(9, 3);
    assert(slots.Pending(9) == 0);
    std::printf("acquire-slots-test: ok\n");
    return 0;
}
