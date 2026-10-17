// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <limits>
#include <mutex>
#include <vector>
#include "video_core/renderer_vulkan/vk_breadcrumbs.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

#include "common/assert.h"

namespace Vulkan {

constexpr u64 WAIT_TIMEOUT = std::numeric_limits<u64>::max();
constexpr u64 STALL_TIMEOUT = 3'000'000'000; ///< ns without progress before the stall report

namespace {
std::mutex stall_mutex;
std::vector<std::function<void()>>& StallReporters() {
    static std::vector<std::function<void()>> reporters;
    return reporters;
}
} // namespace

void Semaphore::AddStallReporter(std::function<void()> reporter) {
    std::scoped_lock lk{stall_mutex};
    StallReporters().push_back(std::move(reporter));
}

Semaphore::Semaphore(const Instance& instance_) : instance{instance_} {
    const vk::StructureChain semaphore_chain = {
        vk::SemaphoreCreateInfo{},
        vk::SemaphoreTypeCreateInfo{
            .semaphoreType = vk::SemaphoreType::eTimeline,
            .initialValue = 0,
        },
    };
    auto [semaphore_result, sem] =
        instance.GetDevice().createSemaphoreUnique(semaphore_chain.get());
    ASSERT_MSG(semaphore_result == vk::Result::eSuccess, "Failed to create master semaphore: {}",
               vk::to_string(semaphore_result));
    semaphore = std::move(sem);
}

Semaphore::~Semaphore() = default;

void Semaphore::Refresh() {
    u64 this_tick{};
    u64 counter{};
    do {
        this_tick = gpu_tick.load(std::memory_order_acquire);
        auto [counter_result, cntr] = instance.GetDevice().getSemaphoreCounterValue(*semaphore);
        if (counter_result == vk::Result::eErrorDeviceLost) {
            Breadcrumbs::ReportDeviceLost("reading the GPU progress");
        }
        ASSERT_MSG(counter_result == vk::Result::eSuccess,
                   "Failed to get master semaphore value: {}", vk::to_string(counter_result));
        counter = cntr;
        if (counter < this_tick) {
            return;
        }
    } while (!gpu_tick.compare_exchange_weak(this_tick, counter, std::memory_order_release,
                                             std::memory_order_relaxed));
}

void Semaphore::Wait(u64 tick) {
    // No need to wait if the GPU is ahead of the tick
    if (IsFree(tick)) {
        return;
    }
    // Update the GPU tick and try again
    Refresh();
    if (IsFree(tick)) {
        return;
    }

    // If none of the above is hit, fallback to a regular wait
    const vk::SemaphoreWaitInfo wait_info = {
        .semaphoreCount = 1,
        .pSemaphores = &semaphore.get(),
        .pValues = &tick,
    };

    // bbport: a lost device ends the wait with a report instead of a silent endless loop. A wait
    // stuck for 3 s prints what is queued once per process (it then goes on waiting).
    static std::atomic_flag stall_reported = ATOMIC_FLAG_INIT;
    bool watch = !stall_reported.test();
    while (true) {
        const vk::Result result =
            instance.GetDevice().waitSemaphores(&wait_info, watch ? STALL_TIMEOUT : WAIT_TIMEOUT);
        if (result == vk::Result::eSuccess) {
            break;
        }
        if (result == vk::Result::eTimeout && watch) {
            watch = false;
            if (!stall_reported.test_and_set()) {
                const auto [counter_result, counter] =
                    instance.GetDevice().getSemaphoreCounterValue(*semaphore);
                std::printf("GPU stall: waited 3 s for tick %llu of timeline %p (the GPU is at "
                            "%llu); what was handed to the GPU:\n",
                            (unsigned long long)tick, static_cast<void*>(Handle()),
                            (unsigned long long)counter);
                std::vector<std::function<void()>> reporters;
                {
                    std::scoped_lock lk{stall_mutex};
                    reporters = StallReporters();
                }
                for (const auto& reporter : reporters) {
                    reporter();
                }
                std::fflush(stdout);
            }
            continue;
        }
        if (result == vk::Result::eErrorDeviceLost) {
            Breadcrumbs::ReportDeviceLost("waiting for a submission");
            ASSERT_MSG(false, "Device lost while waiting for a submission");
        }
    }
    Refresh();
}

} // namespace Vulkan
