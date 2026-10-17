// SPDX-License-Identifier: GPL-2.0-or-later
// A pipeline whose driver compile runs on a worker: Handle() waits for it, then returns at once.
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {
extern std::atomic<u64> g_bb_pipeline_wait_ns;
}

namespace {
struct DeferredPipeline : Vulkan::Pipeline {
    DeferredPipeline(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                     Vulkan::DescriptorHeap& heap, const Shader::Profile& profile)
        : Pipeline{instance, scheduler, heap, profile, {}} {
        handle_pending.store(true);
    }
    void Finish() {
        PublishHandle();
    }
};
} // namespace

int main() {
    using namespace std::chrono;
    Vulkan::Instance instance(0, false);
    Vulkan::Scheduler scheduler(instance);
    const std::array pool_sizes{vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, 4}};
    Vulkan::DescriptorHeap heap(instance, scheduler.GetWorkSemaphore(), pool_sizes, 1);
    const Shader::Profile profile{};

    DeferredPipeline pipeline(instance, scheduler, heap, profile);
    assert(pipeline.IsHandlePending());
    const auto start = steady_clock::now();
    std::thread worker([&] {
        std::this_thread::sleep_for(milliseconds(50));
        pipeline.Finish();
    });
    (void)pipeline.Handle(); // the recording thread's bind
    const auto waited = steady_clock::now() - start;
    worker.join();
    assert(!pipeline.IsHandlePending());
    assert(waited >= milliseconds(45));
    assert(Vulkan::g_bb_pipeline_wait_ns.load() >= 45'000'000);

    // Published: later binds do not wait.
    const auto again = steady_clock::now();
    (void)pipeline.Handle();
    assert(steady_clock::now() - again < milliseconds(5));
    std::printf("Deferred pipeline: Handle() waited %.1f ms for the worker, then none\n",
                duration<double, std::milli>(waited).count());
    return 0;
}
