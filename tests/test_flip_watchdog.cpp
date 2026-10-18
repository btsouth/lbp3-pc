// SPDX-License-Identifier: GPL-2.0-or-later
// A game's flip watchdog (LBP3 0x54630): ten 100 ms waits for the flip event in a row that time
// out, each counted only when sceGnmDriverCaptureInProgress says no, stop the game. A flip held
// up by pipeline compiles (a cold cache on NVIDIA, 23:04 run) must not count; a GPU that does
// not flip for no host reason still must. Also: a flip event is edge triggered (one trigger, one
// wakeup), and a poll racing triggers loses none.
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

#include "core/libraries/gnmdriver/gnmdriver.h"
#include "core/libraries/gnmdriver/host_stall.h"
#include "core/libraries/kernel/equeue.h"
#include "core/libraries/videoout/video_out.h"

using namespace Libraries;
using Kernel::OrbisKernelEvent;

namespace {
constexpr u64 FlipId = u64(VideoOut::OrbisVideoOutInternalEventId::Flip);

Kernel::EqueueInternal* FlipQueue(s64 handle) {
    auto* eq = new Kernel::EqueueInternal(handle, "flip");
    Kernel::EqueueEvent event{};
    event.event.ident = FlipId;
    event.event.filter = OrbisKernelEvent::Filter::VideoOut;
    event.event.flags = OrbisKernelEvent::Flags::Add;
    assert(eq->AddEvent(event));
    return eq;
}

void Flip(Kernel::EqueueInternal* eq) {
    assert(eq->TriggerEvent(FlipId, OrbisKernelEvent::Filter::VideoOut,
                            reinterpret_cast<void*>(FlipId)));
}

int Wait(Kernel::EqueueInternal* eq, u32 micros) {
    OrbisKernelEvent ev[4]{};
    return eq->WaitForEvents(ev, 4, &micros);
}

// LBP3 0x54630..0x5467f: true if the game would trap.
bool Watchdog(Kernel::EqueueInternal* eq) {
    int timeouts = 0;
    while (Wait(eq, 100000) == 0) {
        timeouts += !GnmDriver::sceGnmDriverCaptureInProgress();
        if (timeouts == 10) {
            return true;
        }
    }
    return false;
}
} // namespace

int main() {
    namespace Stall = GnmDriver::HostStall;
    // CaptureInProgress: false on an idle host, true during a stall and one wait period after.
    assert(!GnmDriver::sceGnmDriverCaptureInProgress());
    Stall::Begin();
    Stall::Begin();
    Stall::End();
    assert(GnmDriver::sceGnmDriverCaptureInProgress());
    Stall::End();
    assert(GnmDriver::sceGnmDriverCaptureInProgress());
    assert(!Stall::Recent(Stall::NowNs() + Stall::GraceNs));

    auto* eq = FlipQueue(1);
    // Edge triggered: two flips before a wait are one event, and the next wait times out.
    Flip(eq);
    Flip(eq);
    assert(Wait(eq, 100000) == 1);
    assert(Wait(eq, 10000) == 0);
    u32 zero = 0;
    OrbisKernelEvent ev[1]{};
    assert(eq->WaitForEvents(ev, 1, &zero) == 0);

    // A flip 1.3 s late because the GPU waited for compiles: no trap.
    std::this_thread::sleep_for(std::chrono::milliseconds(150)); // the grace above has passed
    std::thread compiling([eq] {
        Stall::Scope stall;
        std::this_thread::sleep_for(std::chrono::milliseconds(1300));
        Flip(eq);
    });
    assert(!Watchdog(eq));
    compiling.join();

    // A GPU that stops flipping with no host stall still traps after 1 s.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const auto start = std::chrono::steady_clock::now();
    assert(Watchdog(eq));
    assert(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(1000));

    // Polls (timeout 0) racing triggers: every flip is seen.
    constexpr int Flips = 20000;
    std::atomic<int> seen{0};
    std::thread flipper([&] {
        for (int i = 0; i < Flips; ++i) {
            Flip(eq);
            while (seen.load() <= i) {
                std::this_thread::yield();
            }
        }
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (seen.load() < Flips) {
        assert(std::chrono::steady_clock::now() < deadline);
        if (eq->WaitForEvents(ev, 1, &zero) == 1) {
            seen.fetch_add(1);
        }
    }
    flipper.join();
    assert(eq->WaitForEvents(ev, 1, &zero) == 0);

    std::printf("Flip watchdog: compile stalls read as a capture, real hangs still trap, "
                "flip events edge triggered, %d polled flips none lost\n",
                Flips);
    return 0;
}
