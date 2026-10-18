// SPDX-FileCopyrightText: Copyright 2026 bbport
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include "common/types.h"

// bbport: host work that holds up the emulated GPU and has no PS4 counterpart (the driver
// compiling a pipeline that the GPU's next draw needs). A PS4 never stops its GPU for that;
// the nearest thing a game knows of is a GPU capture by the debugger, which it learns of from
// sceGnmDriverCaptureInProgress. Games' flip watchdogs skip the timeouts that a capture spans
// (LBP3 0x54664: 10 timed-out 100 ms flip waits that were not under a capture trap the game).
// So that function reports such a stall while it lasts and for one wait period after, and a
// cold pipeline cache can no longer trip a watchdog, while a GPU that really hangs still does.
namespace Libraries::GnmDriver::HostStall {

/// A watchdog's wait period that a stall still covers after it ends (LBP3 waits 100 ms).
inline constexpr s64 GraceNs = 100'000'000;

inline std::atomic<s32> active{0};
inline std::atomic<s64> last_end_ns{0};

inline s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline void Begin() {
    active.fetch_add(1, std::memory_order_acq_rel);
}

inline void End() {
    last_end_ns.store(NowNs(), std::memory_order_release);
    active.fetch_sub(1, std::memory_order_acq_rel);
}

/// True while a stall lasts, and for GraceNs after the last one ended.
inline bool Recent(s64 now_ns = NowNs()) {
    if (active.load(std::memory_order_acquire) > 0) {
        return true;
    }
    const s64 end = last_end_ns.load(std::memory_order_acquire);
    return end != 0 && now_ns - end < GraceNs;
}

struct Scope {
    Scope() {
        Begin();
    }
    ~Scope() {
        End();
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

} // namespace Libraries::GnmDriver::HostStall
