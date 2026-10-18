// SPDX-FileCopyrightText: Copyright 2026 bbport
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iterator>
#include <unistd.h>
#include "common/types.h"

// bbport (diagnostics): the last steps of the flip path (guest submissions, flip interrupts,
// flips, flip events and the guest's waits for them), printed when a game's GPU watchdog fires.
namespace Libraries::VideoOut::FlipTrace {

enum Kind : u32 {
    SubmitAndFlip,  // a = buffer index
    EopRegistered,  // a = buffer index
    GfxFlipIrq,     // a = 1 if a handler was waiting for it
    SubmitFlip,     // a = buffer index
    FlipDone,       // a = flip count
    EventTriggered, // a = number of flip event queues
    WaitBegin,      // a = timeout in us
    WaitEnd,        // a = events returned
};

struct Entry {
    s64 ns;
    u32 kind;
    s32 tid;
    s64 value;
};

inline std::array<Entry, 128> ring{};
inline std::atomic<u64> head{0};

inline s64 Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline void Note(Kind kind, s64 value = 0) {
    const u64 at = head.fetch_add(1, std::memory_order_relaxed);
    ring[at % ring.size()] = Entry{Now(), kind, static_cast<s32>(gettid()), value};
}

inline void Dump() {
    static constexpr const char* names[] = {"submit+flip", "EOP flip registered", "flip IRQ",
                                            "SubmitFlip",  "flip done",           "flip event",
                                            "wait begin",  "wait end"};
    const s64 now = Now();
    const u64 end = head.load(std::memory_order_relaxed);
    const u64 count = std::min<u64>(end, 48);
    std::printf("Flip trace (last %llu steps, ms before now):\n", (unsigned long long)count);
    for (u64 i = end - count; i < end; ++i) {
        const Entry e = ring[i % ring.size()];
        std::printf("  %9.1f  tid %d  %s %lld\n", double(now - e.ns) / 1e6, e.tid,
                    e.kind < std::size(names) ? names[e.kind] : "?", (long long)e.value);
    }
    std::fflush(stdout);
}

} // namespace Libraries::VideoOut::FlipTrace
