// SPDX-License-Identifier: GPL-2.0-or-later
// A pipeline whose driver compile runs on a worker: Handle() waits for it, then returns at once.
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "shader_recompiler/info.h"
#include "video_core/host_shaders/fs_tri_vert.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

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

    // A real GraphicsPipeline built through a deferral (vertex stage only, no attachments): the
    // driver compile runs on the worker, Handle() waits for it and gets a pipeline; destroying a
    // pipeline whose build has not run yet waits for that build.
    const vk::Device device = instance.GetDevice();
    const vk::ShaderModule vs_module = Vulkan::CompileSPV(FS_TRI_VERT, device);
    const std::array<u32, Shader::ShaderParams::NumShaderUserData> user_data{};
    const Shader::Info vs_info{Shader::HwStage::Vertex, Shader::SwStage::Vertex,
                               Shader::ShaderParams{.user_data = user_data, .code = {}, .hash = 1}};
    std::array<const Shader::Info*, Vulkan::MaxShaderStages> infos{};
    infos[u32(Shader::SwStage::Vertex)] = &vs_info;
    std::array<Shader::RuntimeInfo, Vulkan::MaxShaderStages> runtime_infos{};
    std::array<vk::ShaderModule, Vulkan::MaxShaderStages> modules{}; // indexed by stage
    modules[u32(Shader::SwStage::Vertex)] = vs_module;
    Vulkan::GraphicsPipelineKey key{};
    key.stage_hashes[u32(Shader::SwStage::Vertex)] = 1;
    key.prim_type = AmdGpu::PrimitiveType::TriangleList;
    key.polygon_mode = AmdGpu::PolygonMode::Fill;
    key.depth_clip_enable = 1;
    key.num_samples = key.depth_samples = 1;
    key.color_samples.fill(1);

    std::mutex jobs_mutex;
    std::vector<std::function<void()>> jobs;
    std::atomic<u32> built{0};
    const Vulkan::GraphicsPipeline::Deferral deferral{
        .submit =
            [&](std::function<void()> job) {
                std::scoped_lock lk{jobs_mutex};
                jobs.push_back(std::move(job));
            },
        .built = [&](const Vulkan::GraphicsPipeline::SerializationSupport&) { ++built; },
    };
    const auto make = [&] {
        Vulkan::GraphicsPipeline::SerializationSupport sdata{};
        return std::make_unique<Vulkan::GraphicsPipeline>(
            instance, scheduler, heap, profile, key, vk::PipelineCache{}, infos, runtime_infos,
            std::nullopt, modules, sdata, false, &deferral);
    };
    const auto run_jobs_later = [&](milliseconds delay) {
        return std::thread([&, delay] {
            std::this_thread::sleep_for(delay);
            std::vector<std::function<void()>> todo;
            {
                std::scoped_lock lk{jobs_mutex};
                todo.swap(jobs);
            }
            for (auto& job : todo) {
                job();
            }
        });
    };

    auto real = make();
    assert(real->IsHandlePending() && built.load() == 0);
    std::thread real_worker = run_jobs_later(milliseconds(20));
    const vk::Pipeline handle = real->Handle();
    real_worker.join();
    assert(handle && !real->IsHandlePending() && built.load() == 1);

    auto dropped = make();
    assert(dropped->IsHandlePending());
    std::thread drop_worker = run_jobs_later(milliseconds(30));
    const auto drop_start = steady_clock::now();
    dropped.reset(); // waits for the worker's build
    assert(steady_clock::now() - drop_start >= milliseconds(25) && built.load() == 2);
    drop_worker.join();

    real.reset();
    // Vulkan calls from this executable need their own dispatcher (libbbgpu.so has its own).
    static vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic dispatch;
    dispatch.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    dispatch.init(instance.GetInstance());
    dispatch.init(device);
    device.destroyShaderModule(vs_module, nullptr, dispatch);
    std::printf("Deferred GraphicsPipeline: built on the worker, bound after it, destroyed "
                "while pending waited for its build\n");
    return 0;
}
