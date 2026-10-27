// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(MKW_ENABLE_OPENXR) && (defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__)))

// OpenXR's Vulkan structures are selected when openxr_platform.h is parsed.
#define XR_USE_GRAPHICS_API_VULKAN
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "vr/openxr_vulkan_win32.h"
#include "vr/openxr_diagnostics.h"

#include <aurora/vulkan_win32_interop.h>

#include <vulkan/vulkan.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace mkw::vr {
namespace {
// KARTFRAME_MENU_ENV: unset (or "under") the menus' environment layer goes under the screen's quad; "over"
// puts it over the quad, blended by its alpha (lane H's default); "0" there is none. The fourth headset
// session showed the ray still hidden on the screen with "over": SteamVR on the Frame evidently draws the
// projection layer before the quad whatever the submission order (H4.md). The pointer's target mark is
// drawn into the screen's own image instead (Aurora), which needs no layer order at all.
const char* MenuEnvironmentSetting() {
    const char* env = std::getenv("KARTFRAME_MENU_ENV");
    return env != nullptr ? env : "";
}
} // namespace
namespace {

// When the GPU finished each eye set. SteamVR shows a swapchain image the moment it is
// released, finished or not, so the pacing thread releases a set only after this says so.
// The frame worker, right after Aurora's queue.Submit and the bridge's release barrier,
// makes an empty submission on Dawn's queue (under Dawn's device lock) that signals the
// next value of a timeline semaphore; a watcher thread waits for the values in order and
// records each set's submit-to-completion time. Without timeline semaphores the release
// drains the queue instead (correct, slower; logged). KARTFRAME_NO_GPU_WAIT=1 skips the
// wait: the negative test for the mock's MOCK_XR_BUSY_MS check.
std::atomic<uint32_t> g_gpu_watcher_tid{0};

class GpuCompletion {
public:
    using Clock = std::chrono::steady_clock;
    enum class Mode { None, Timeline, Drain };

    ~GpuCompletion() { Stop(); }

    // Pacing thread, at bind time. get_proc may be null (headless tests): then nothing
    // is waited for and completion is the CPU token alone.
    Mode Start(PFN_vkGetInstanceProcAddr get_proc, VkInstance instance, VkDevice device, uint32_t family,
               uint32_t index, bool timeline) {
        Stop();
        const char* skip = std::getenv("KARTFRAME_NO_GPU_WAIT");
        skip_wait_ = skip && *skip && *skip != '0';
        if (!get_proc || !instance || !device) return mode_ = Mode::None;
        const auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(get_proc(instance, "vkGetDeviceProcAddr"));
        if (!gdpa) return mode_ = Mode::None;
#define KF_DEV(name) name##_ = reinterpret_cast<PFN_##name>(gdpa(device, #name))
        KF_DEV(vkGetDeviceQueue);
        KF_DEV(vkQueueSubmit);
        KF_DEV(vkQueueWaitIdle);
        KF_DEV(vkCreateSemaphore);
        KF_DEV(vkDestroySemaphore);
        KF_DEV(vkWaitSemaphores);
#undef KF_DEV
        if (!vkGetDeviceQueue_ || !vkQueueSubmit_ || !vkQueueWaitIdle_) return mode_ = Mode::None;
        device_ = device;
        vkGetDeviceQueue_(device, family, index, &queue_);
        if (!queue_) return mode_ = Mode::None;
        if (timeline && vkCreateSemaphore_ && vkDestroySemaphore_ && vkWaitSemaphores_) {
            VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
            type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            type.initialValue = 0;
            VkSemaphoreCreateInfo create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
            if (vkCreateSemaphore_(device, &create, nullptr, &semaphore_) == VK_SUCCESS) {
                {
                    std::lock_guard lock(mutex_);
                    stop_ = false;
                    signaled_ = completed_ = 0;
                    failed_ = false;
                }
                watcher_ = std::thread([this] { Watch(); });
                return mode_ = Mode::Timeline;
            }
            semaphore_ = VK_NULL_HANDLE;
        }
        return mode_ = Mode::Drain;
    }

    void Stop() {
        if (watcher_.joinable()) {
            {
                std::lock_guard lock(mutex_);
                stop_ = true;
            }
            cv_.notify_all();
            watcher_.join();
        }
        if (semaphore_ && vkDestroySemaphore_) vkDestroySemaphore_(device_, semaphore_, nullptr);
        semaphore_ = VK_NULL_HANDLE;
        mode_ = Mode::None;
    }

    Mode CurrentMode() const { return mode_; }
    bool SkipsWait() const { return skip_wait_; }

    // Frame worker, inside the bridge's submitted callback (Aurora's queue-submit mutex
    // held, Dawn's device lock not). Returns the value to wait for: 0 when there is
    // nothing to wait for (no GPU sync, or the drain mode), UINT64_MAX on failure.
    uint64_t Signal(int64_t submit_ns) {
        if (mode_ != Mode::Timeline) return 0;
        uint64_t value;
        {
            std::lock_guard lock(mutex_);
            if (failed_) return std::numeric_limits<uint64_t>::max();
            value = signaled_ + 1;
            submit_ns_[value % kRing] = submit_ns;
        }
        VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        timeline.signalSemaphoreValueCount = 1;
        timeline.pSignalSemaphoreValues = &value;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO, &timeline};
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &semaphore_;
        VkResult result;
        {
            void* guard = aurora_vulkan_win32_lock_queue();
            result = vkQueueSubmit_(queue_, 1, &submit, VK_NULL_HANDLE);
            aurora_vulkan_win32_unlock_queue(guard);
        }
        std::lock_guard lock(mutex_);
        if (result != VK_SUCCESS) {
            failed_ = true;
            cv_.notify_all();
            return std::numeric_limits<uint64_t>::max();
        }
        signaled_ = value;
        cv_.notify_all();
        return value;
    }

    // Pacing thread: true once the set that signalled `value` finished on the GPU.
    bool WaitUntil(uint64_t value, Clock::time_point deadline) {
        if (skip_wait_ || value == 0) {
            if (mode_ == Mode::Drain && !skip_wait_) {
                void* guard = aurora_vulkan_win32_lock_queue();
                const bool idle = vkQueueWaitIdle_(queue_) == VK_SUCCESS;
                aurora_vulkan_win32_unlock_queue(guard);
                return idle;
            }
            return true;
        }
        if (value == std::numeric_limits<uint64_t>::max()) return false;
        std::unique_lock lock(mutex_);
        return cv_.wait_until(lock, deadline, [&] { return completed_ >= value || failed_ || stop_; }) &&
               completed_ >= value;
    }

    bool Failed() {
        std::lock_guard lock(mutex_);
        return failed_;
    }

    // Lane U, KARTFRAME_RELEASE=idle: the caller holds the queue guard (Dawn's device lock); drains
    // Dawn's queue so that nothing of ours is queued or running on the GPU when SteamVR's client
    // submits inside xrReleaseSwapchainImage. Returns the wait in ms (-1: no queue).
    double DrainHeldMs() {
        if (!queue_ || !vkQueueWaitIdle_) return -1.0;
        const int64_t start = diagnostics::detail::NowNs();
        vkQueueWaitIdle_(queue_);
        return static_cast<double>(diagnostics::detail::NowNs() - start) / 1.0e6;
    }

private:
    static constexpr uint64_t kRing = 64;

    void Watch() {
#if defined(__linux__)
        g_gpu_watcher_tid.store(static_cast<uint32_t>(syscall(SYS_gettid)), std::memory_order_release);
#endif
        uint64_t next = 1;
        for (;;) {
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [&] { return stop_ || failed_ || signaled_ >= next; });
                if (stop_ || failed_) return;
            }
            VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            wait.semaphoreCount = 1;
            wait.pSemaphores = &semaphore_;
            wait.pValues = &next;
            const VkResult result = vkWaitSemaphores_(device_, &wait, 50'000'000ull);
            if (result == VK_TIMEOUT) continue;
            const int64_t now = diagnostics::detail::NowNs();
            std::lock_guard lock(mutex_);
            if (result != VK_SUCCESS) {
                failed_ = true;
                cv_.notify_all();
                return;
            }
            pacing::OnSetGpu(static_cast<double>(now - submit_ns_[next % kRing]) / 1.0e6);
            completed_ = next++;
            cv_.notify_all();
        }
    }

    Mode mode_ = Mode::None;
    bool skip_wait_ = false;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkSemaphore semaphore_ = VK_NULL_HANDLE;
    PFN_vkGetDeviceQueue vkGetDeviceQueue_ = nullptr;
    PFN_vkQueueSubmit vkQueueSubmit_ = nullptr;
    PFN_vkQueueWaitIdle vkQueueWaitIdle_ = nullptr;
    PFN_vkCreateSemaphore vkCreateSemaphore_ = nullptr;
    PFN_vkDestroySemaphore vkDestroySemaphore_ = nullptr;
    PFN_vkWaitSemaphores vkWaitSemaphores_ = nullptr;
    std::thread watcher_;
    std::mutex mutex_;
    std::condition_variable cv_;
    uint64_t signaled_ = 0;
    uint64_t completed_ = 0;
    bool failed_ = false;
    bool stop_ = false;
    std::array<int64_t, kRing> submit_ns_{};
};

// Lane Q: the bridge's own small command buffers on Dawn's queue (taken under Dawn's queue lock, like
// GpuCompletion's signal): the first-write barrier before each set's writes (KARTFRAME_FIRST_WRITE_BARRIER)
// and the one-time clear of the race quad's images (KARTFRAME_RACE_QUAD). Pacing thread only.
class BridgeCommands {
public:
    ~BridgeCommands() { Stop(); }

    bool Start(PFN_vkGetInstanceProcAddr get_proc, VkInstance instance, VkDevice device, uint32_t family,
               uint32_t index) {
        Stop();
        if (!get_proc || !instance || !device) return false;
        const auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(get_proc(instance, "vkGetDeviceProcAddr"));
        if (!gdpa) return false;
#define KF_DEV(name) name##_ = reinterpret_cast<PFN_##name>(gdpa(device, #name))
        KF_DEV(vkGetDeviceQueue);
        KF_DEV(vkQueueSubmit);
        KF_DEV(vkCreateCommandPool);
        KF_DEV(vkDestroyCommandPool);
        KF_DEV(vkAllocateCommandBuffers);
        KF_DEV(vkBeginCommandBuffer);
        KF_DEV(vkEndCommandBuffer);
        KF_DEV(vkCmdPipelineBarrier);
        KF_DEV(vkCmdClearColorImage);
        KF_DEV(vkCreateFence);
        KF_DEV(vkDestroyFence);
        KF_DEV(vkWaitForFences);
        KF_DEV(vkResetFences);
#undef KF_DEV
        if (!vkGetDeviceQueue_ || !vkQueueSubmit_ || !vkCreateCommandPool_ || !vkDestroyCommandPool_ ||
            !vkAllocateCommandBuffers_ || !vkBeginCommandBuffer_ || !vkEndCommandBuffer_ || !vkCmdPipelineBarrier_ ||
            !vkCmdClearColorImage_ || !vkCreateFence_ || !vkDestroyFence_ || !vkWaitForFences_ || !vkResetFences_)
            return false;
        device_ = device;
        vkGetDeviceQueue_(device, family, index, &queue_);
        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool_info.queueFamilyIndex = family;
        if (!queue_ || vkCreateCommandPool_(device, &pool_info, nullptr, &pool_) != VK_SUCCESS) {
            pool_ = VK_NULL_HANDLE;
            return false;
        }
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = pool_;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = kSlots;
        bool ok = vkAllocateCommandBuffers_(device, &alloc, buffers_.data()) == VK_SUCCESS;
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (auto& fence : fences_) ok = ok && vkCreateFence_(device, &fence_info, nullptr, &fence) == VK_SUCCESS;
        if (!ok) Stop();
        return ready_ = ok;
    }

    // After the queue drained (Shutdown).
    void Stop() {
        if (device_ && vkDestroyFence_)
            for (auto& fence : fences_) {
                if (fence) vkDestroyFence_(device_, fence, nullptr);
                fence = VK_NULL_HANDLE;
            }
        if (device_ && pool_ && vkDestroyCommandPool_) vkDestroyCommandPool_(device_, pool_, nullptr);
        pool_ = VK_NULL_HANDLE;
        buffers_ = {};
        ready_ = false;
    }

    bool Ready() const { return ready_; }

    // GalaxyFrame's barrier at the first write of an image after xrWaitSwapchainImage (H9.md section 3):
    // everything earlier on the queue (SteamVR's client's acquire work included) and all its memory writes
    // before anything later reads or writes the images. Layout kept: COLOR_ATTACHMENT_OPTIMAL, as the
    // runtime hands an acquired colour image over and as Dawn tracks it. Submitted on its own, before
    // Aurora's set; a barrier's first scope is everything earlier in submission order on the queue.
    bool FirstWriteBarrier(const VkImage* images, uint32_t count) {
        VkCommandBuffer cb;
        VkFence fence;
        if (!ready_ || count == 0 || !Begin(cb, fence)) return false;
        std::array<VkImageMemoryBarrier, 4> barriers{};
        count = std::min<uint32_t>(count, static_cast<uint32_t>(barriers.size()));
        for (uint32_t i = 0; i < count; ++i) {
            auto& b = barriers[i];
            b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = images[i];
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        vkCmdPipelineBarrier_(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                              nullptr, count, barriers.data());
        return Submit(cb, fence);
    }

    // The race quad's image: cleared to transparent black, left COLOR_ATTACHMENT_OPTIMAL, and waited for.
    bool ClearAndWait(VkImage image) {
        VkCommandBuffer cb;
        VkFence fence;
        if (!ready_ || !Begin(cb, fence)) return false;
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = range;
        vkCmdPipelineBarrier_(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                              nullptr, 1, &b);
        const VkClearColorValue clear{};  // 0, 0, 0, 0: fully transparent
        vkCmdClearColorImage_(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        vkCmdPipelineBarrier_(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                              nullptr, 1, &b);
        return Submit(cb, fence) && vkWaitForFences_(device_, 1, &fence, VK_TRUE, 1'000'000'000ull) == VK_SUCCESS;
    }

private:
    static constexpr uint32_t kSlots = 8;

    bool Begin(VkCommandBuffer& cb, VkFence& fence) {
        const uint32_t slot = next_++ % kSlots;
        cb = buffers_[slot];
        fence = fences_[slot];
        // Eight sets ago: long finished (a barrier-only submission).
        if (vkWaitForFences_(device_, 1, &fence, VK_TRUE, 1'000'000'000ull) != VK_SUCCESS ||
            vkResetFences_(device_, 1, &fence) != VK_SUCCESS)
            return false;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        return vkBeginCommandBuffer_(cb, &begin) == VK_SUCCESS;
    }

    bool Submit(VkCommandBuffer cb, VkFence fence) {
        if (vkEndCommandBuffer_(cb) != VK_SUCCESS) return false;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cb;
        void* guard = aurora_vulkan_win32_lock_queue();
        const VkResult result = vkQueueSubmit_(queue_, 1, &submit, fence);
        aurora_vulkan_win32_unlock_queue(guard);
        if (result != VK_SUCCESS) {
            // The fence stays unsignalled: make the slot usable again.
            VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            const uint32_t slot = (next_ - 1) % kSlots;
            vkDestroyFence_(device_, fences_[slot], nullptr);
            if (vkCreateFence_(device_, &fence_info, nullptr, &fences_[slot]) != VK_SUCCESS) ready_ = false;
        }
        return result == VK_SUCCESS;
    }

    bool ready_ = false;
    uint32_t next_ = 0;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, kSlots> buffers_{};
    std::array<VkFence, kSlots> fences_{};
    PFN_vkGetDeviceQueue vkGetDeviceQueue_ = nullptr;
    PFN_vkQueueSubmit vkQueueSubmit_ = nullptr;
    PFN_vkCreateCommandPool vkCreateCommandPool_ = nullptr;
    PFN_vkDestroyCommandPool vkDestroyCommandPool_ = nullptr;
    PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers_ = nullptr;
    PFN_vkBeginCommandBuffer vkBeginCommandBuffer_ = nullptr;
    PFN_vkEndCommandBuffer vkEndCommandBuffer_ = nullptr;
    PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier_ = nullptr;
    PFN_vkCmdClearColorImage vkCmdClearColorImage_ = nullptr;
    PFN_vkCreateFence vkCreateFence_ = nullptr;
    PFN_vkDestroyFence vkDestroyFence_ = nullptr;
    PFN_vkWaitForFences vkWaitForFences_ = nullptr;
    PFN_vkResetFences vkResetFences_ = nullptr;
};

// KARTFRAME_FIRST_WRITE_BARRIER=all|dawn (lane Q, default all): GalaxyFrame's first-write barrier
// (BridgeCommands::FirstWriteBarrier) before every set's writes to its swapchain images; dawn leaves
// only Dawn's own tracked barrier (COLOR_ATTACHMENT_OUTPUT to COLOR_ATTACHMENT_OUTPUT).
bool FirstWriteBarrierSetting() {
    static const bool all = [] {
        const char* env = std::getenv("KARTFRAME_FIRST_WRITE_BARRIER");
        return !(env != nullptr && std::string_view(env) == "dawn");
    }();
    return all;
}
// KARTFRAME_RACE_QUAD=1|cycle|0 (lane Q, default 1): a frame whose only layer would be the projection
// (races, loading, the guide) gets one more layer, a 16x16 fully transparent quad 2 m ahead, 1 cm across,
// alpha blended, after the projection: SteamVR then never composites a projection layer alone (KartFrame's
// menus and GalaxyFrame, both clean on the Frame, always submit quads with their projection). 1: the
// images are cleared once and the quad shows the last released one every frame; cycle: one image is
// acquired, waited and released every frame as well (no GPU work, GalaxyFrame's per-frame pattern).
int RaceQuadSetting() {
    static const int mode = [] {
        const char* env = std::getenv("KARTFRAME_RACE_QUAD");
        const std::string_view v = env != nullptr ? env : "1";
        return v == "0" ? 0 : v == "cycle" ? 2 : 1;
    }();
    return mode;
}

bool SameVkCopyFamily(VkFormat a, VkFormat b) {
    return a == b || ((a == VK_FORMAT_R8G8B8A8_UNORM || a == VK_FORMAT_R8G8B8A8_SRGB) &&
                     (b == VK_FORMAT_R8G8B8A8_UNORM || b == VK_FORMAT_R8G8B8A8_SRGB)) ||
                    ((a == VK_FORMAT_B8G8R8A8_UNORM || a == VK_FORMAT_B8G8R8A8_SRGB) &&
                     (b == VK_FORMAT_B8G8R8A8_UNORM || b == VK_FORMAT_B8G8R8A8_SRGB));
}
VkFormat WindowsVkSrgbSibling(VkFormat f) {
    if (f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB) return VK_FORMAT_R8G8B8A8_SRGB;
    if (f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB) return VK_FORMAT_B8G8R8A8_SRGB;
    return VK_FORMAT_UNDEFINED;
}
bool IsWindowsVkSrgbFormat(VkFormat f) { return f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB; }

// Lane O (H9.md): the eye swapchains as GalaxyFrame makes them, which SteamVR on the Frame shows clean.
// Aurora writes the eyes through an sRGB view with a draw (KARTFRAME_SINK=srgb, the default), so the
// swapchain's channel order is free: R8G8B8A8_SRGB (VkFormat 43, GalaxyFrame's) by default,
// KARTFRAME_XR_FORMAT=bgra the old B8G8R8A8_SRGB (50, Aurora's own order). The copy and blit sinks
// keep Aurora's order (a copy cannot swap channels).
bool SrgbSinkSetting() {
    static const bool srgb = [] {
        const char* env = std::getenv("KARTFRAME_SINK");
        const std::string_view v = env != nullptr ? env : "";
        return v != "copy" && v != "blit";
    }();
    return srgb;
}
bool RgbaEyesSetting() {
    static const bool rgba = [] {
        const char* env = std::getenv("KARTFRAME_XR_FORMAT");
        return !(env != nullptr && std::string_view(env) == "bgra");
    }();
    return rgba;
}
// Usage of the eye swapchains: GalaxyFrame's COLOR_ATTACHMENT | SAMPLED by default (plus TRANSFER_DST
// only where a copy can still reach an eye image: Aurora's channel order, or the test stamp);
// KARTFRAME_XR_USAGE=kart the old COLOR_ATTACHMENT | TRANSFER_DST.
bool GalaxyUsageSetting() {
    static const bool galaxy = [] {
        const char* env = std::getenv("KARTFRAME_XR_USAGE");
        return !(env != nullptr && std::string_view(env) == "kart");
    }();
    return galaxy;
}
// Lane AA (test only, default off): KARTFRAME_STEREO_PACKED=1 gives each eye set ONE swapchain, both eyes side by
// side (eye N at x = N * eye width; 2 * eye wide, single mip, arraySize 1, the eyes' format and usage), acquired,
// waited and released once per set, the projection views picking their halves with imageRect offsets. SteamVR on the
// Frame takes each eye's latest released image when it reaches that eye, so per-eye swapchains released separately
// can pair a right eye from one set with a left eye from another (the race's right-eye double image, sessions h19
// and h20); with one image per set both eyes always come from the same set. The menus' screen quad uses the left
// half. Aurora's sink reads the same switch (vulkan_win32_interop.cpp) and writes each eye into its half.
bool StereoPackedSetting() {
    static const bool packed = [] {
        const char* env = std::getenv("KARTFRAME_STEREO_PACKED");
        return env != nullptr && env[0] == '1';
    }();
    return packed;
}
bool FrameStampSetting() {
    static const bool stamp = [] {
        const char* env = std::getenv("KARTFRAME_FRAME_STAMP");
        return env != nullptr && env[0] == '1';
    }();
    return stamp;
}
// GalaxyFrame locates the views once more for the frame's own display time between releasing a set and
// xrEndFrame (its pose check); KARTFRAME_END_LOCATE=0 leaves that call out.
bool EndLocateSetting() {
    static const bool on = [] {
        const char* env = std::getenv("KARTFRAME_END_LOCATE");
        return !(env != nullptr && env[0] == '0');
    }();
    return on;
}

// Lane R: KARTFRAME_SUBMIT_POSE=fresh submits race frames that show a new set with the poses of the
// pose check's locate for the frame's own display time (the images stay as rendered); default rendered.
bool SubmitFreshPoseSetting() {
    static const bool fresh = [] {
        const char* env = std::getenv("KARTFRAME_SUBMIT_POSE");
        return env != nullptr && std::string_view(env) == "fresh";
    }();
    return fresh;
}
// KARTFRAME_END_TIME=locate (lane G's rule): end a layer with its poses' locate time.
bool EndAtLocateSetting() {
    static const bool on = [] {
        const char* env = std::getenv("KARTFRAME_END_TIME");
        return env != nullptr && std::string_view(env) == "locate";
    }();
    return on;
}

const char* WindowsVkBeginStatusOperation(OpenXRFrameStatus status) noexcept {
    switch (status) {
    case OpenXRFrameStatus::Ready:
        return "ready";
    case OpenXRFrameStatus::SessionNotRunning:
        return "session is not running";
    case OpenXRFrameStatus::ExitRequested:
        return "runtime requested exit";
    case OpenXRFrameStatus::Error:
        return "xrWaitFrame failed";
    }
    return "unknown frame status";
}

} // namespace

class OpenXRWindowsVulkanBackend::Impl final {
public:
    explicit Impl(OpenXRLogCallback logger) : logger_(std::move(logger)) {}

    ~Impl() { Shutdown(); }

    struct EyeSwapchain {
        XrSwapchain handle = XR_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<XrSwapchainImageVulkanKHR> images;
        uint32_t acquired_index = 0;
        int32_t released_index = -1;  // the image released last (frame log)
        uint32_t pair_id = 0;         // which eye swapchain pair (frame log)
        bool acquired = false;
        bool waited = false;
        bool release_forbidden = false;
    };
    using EyePair = std::array<EyeSwapchain, kOpenXREyeCount>;
    // Lane K: a set rendered ahead carries the slot it was written into (see LoadAheadSlot).
    struct AheadSlot {
        uint64_t serial = 0;
        EyePair eyes{};
        EyeSwapchain env{};
        EyeSwapchain panel{};
        uint64_t session_serial = 0;
        uint64_t space_serial = 0;
    };

    static uint32_t VkVersion(XrVersion v) {
        return VK_MAKE_API_VERSION(0, XR_VERSION_MAJOR(v), XR_VERSION_MINOR(v), XR_VERSION_PATCH(v));
    }
    // Dawn's hooks. proc may be null in the headless replay tests.
    static int32_t CreateInstance(void* self, void* proc, const void* info, const void* allocator, void** out) {
        auto& owner = *static_cast<Impl*>(self);
        const auto get_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(proc);
        XrVulkanInstanceCreateInfoKHR create{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
        create.systemId = owner.runtime_->SystemId();
        create.pfnGetInstanceProcAddr = get_proc;
        auto vk_info = *static_cast<const VkInstanceCreateInfo*>(info);
        if (!vk_info.pApplicationInfo) return VK_ERROR_INITIALIZATION_FAILED;
        auto app = *vk_info.pApplicationInfo;
        // Dawn asks for Vulkan 1.1. Prefer 1.2 where the loader and runtime allow
        // it: PC runtimes create timeline semaphores on this device, and from 1.2
        // that feature is core, so CreateDevice can enable it without knowing
        // which extensions the runtime appends.
        uint32_t loader_version = VK_API_VERSION_1_0;
        const auto enumerate = get_proc ? reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
                                              get_proc(nullptr, "vkEnumerateInstanceVersion")) : nullptr;
        if (!enumerate || enumerate(&loader_version) != VK_SUCCESS) loader_version = VK_API_VERSION_1_0;
        const uint32_t preferred = std::min({static_cast<uint32_t>(VK_API_VERSION_1_2), loader_version,
                                             VkVersion(owner.requirements_.max_api_version)});
        app.apiVersion = std::max({app.apiVersion, VkVersion(owner.requirements_.min_api_version), preferred});
        if (app.apiVersion > VkVersion(owner.requirements_.max_api_version)) return VK_ERROR_INCOMPATIBLE_DRIVER;
        vk_info.pApplicationInfo = &app;
        // Dawn enables VK_EXT_swapchain_maintenance1 on the device whenever the device offers it, but
        // that extension requires VK_EXT_surface_maintenance1 (and its dependency
        // VK_KHR_get_surface_capabilities2) on the instance, which Dawn does not ask for here
        // (VUID-vkCreateDevice-ppEnabledExtensionNames-01387). Add them when the loader offers them.
        std::vector<const char*> extensions(vk_info.ppEnabledExtensionNames,
                                            vk_info.ppEnabledExtensionNames + vk_info.enabledExtensionCount);
        if (const auto enumerate_ext = get_proc ? reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
                                                      get_proc(nullptr, "vkEnumerateInstanceExtensionProperties"))
                                                : nullptr) {
            uint32_t count = 0;
            std::vector<VkExtensionProperties> offered;
            if (enumerate_ext(nullptr, &count, nullptr) == VK_SUCCESS) {
                offered.resize(count);
                if (enumerate_ext(nullptr, &count, offered.data()) != VK_SUCCESS) offered.clear();
                offered.resize(std::min<size_t>(offered.size(), count));
            }
            const auto is_offered = [&](const char* name) {
                for (const auto& e : offered) if (std::strcmp(e.extensionName, name) == 0) return true;
                return false;
            };
            const auto is_enabled = [&](const char* name) {
                for (const char* e : extensions) if (std::strcmp(e, name) == 0) return true;
                return false;
            };
            if (is_offered(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME) &&
                is_offered(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) &&
                !is_enabled(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME)) {
                if (!is_enabled(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME))
                    extensions.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
                extensions.push_back(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
                vk_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
                vk_info.ppEnabledExtensionNames = extensions.data();
            }
        }
        create.vulkanCreateInfo = &vk_info;
        create.vulkanAllocator = static_cast<const VkAllocationCallbacks*>(allocator);
        VkResult result = VK_ERROR_INITIALIZATION_FAILED;
        const XrResult xr = owner.create_instance_(owner.runtime_->Instance(), &create, reinterpret_cast<VkInstance*>(out), &result);
        if (XR_SUCCEEDED(xr) && result == VK_SUCCESS) {
            owner.vk_instance_ = *reinterpret_cast<VkInstance*>(out);
            owner.instance_api_version_ = app.apiVersion;
            owner.vk_get_proc_ = get_proc;
            owner.LogInstanceLayers(get_proc, vk_info);
        }
        return XR_SUCCEEDED(xr) ? result : VK_ERROR_INITIALIZATION_FAILED;
    }
    static int32_t GetPhysical(void* self, void* instance, void** out) {
        auto& owner = *static_cast<Impl*>(self);
        XrVulkanGraphicsDeviceGetInfoKHR get{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
        get.systemId = owner.runtime_->SystemId();
        get.vulkanInstance = static_cast<VkInstance>(instance);
        return XR_SUCCEEDED(owner.get_device_(owner.runtime_->Instance(), &get,
            reinterpret_cast<VkPhysicalDevice*>(out))) ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
    }
    static int32_t CreateDevice(void* self, void* proc, void* physical, const void* info, const void* allocator, void** out) {
        auto& owner = *static_cast<Impl*>(self);
        const auto get_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(proc);
        const auto vk_physical = static_cast<VkPhysicalDevice>(physical);
        auto vk_info = *static_cast<const VkDeviceCreateInfo*>(info);
        // PC runtimes (Virtual Desktop, SteamVR) create timeline semaphores on the
        // application's device. The runtime appends the extension, but a feature
        // that is declared and not enabled is undefined behaviour and was seen as
        // VK_ERROR_DEVICE_LOST seconds into a session. From Vulkan 1.2 the
        // feature is core, so enable it whenever the device offers it and Dawn's
        // own chain does not already speak for it.
        VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
        if (get_proc && owner.vk_instance_ && owner.instance_api_version_ >= VK_API_VERSION_1_2) {
            const auto properties_fn = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
                get_proc(owner.vk_instance_, "vkGetPhysicalDeviceProperties"));
            const auto features_fn = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
                get_proc(owner.vk_instance_, "vkGetPhysicalDeviceFeatures2"));
            VkPhysicalDeviceProperties properties{};
            if (properties_fn) properties_fn(vk_physical, &properties);
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &timeline};
            if (features_fn && properties.apiVersion >= VK_API_VERSION_1_2) features_fn(vk_physical, &features);
            bool chained = false;
            for (auto* next = static_cast<const VkBaseInStructure*>(vk_info.pNext); next; next = next->pNext) {
                chained |= next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES ||
                           next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
            }
            if (timeline.timelineSemaphore && !chained) {
                timeline.pNext = const_cast<void*>(vk_info.pNext);
                vk_info.pNext = &timeline;
            } else {
                timeline.timelineSemaphore = VK_FALSE;
            }
        }
        // Lane Y (test only, default off): KARTFRAME_QUEUE_PRIORITY=low|medium|high|realtime chains
        // VkDeviceQueueGlobalPriorityCreateInfoKHR onto every queue Dawn asks for (one: the queue SteamVR's client
        // also submits on in this process) and enables VK_KHR_global_priority (or the EXT) when the device offers
        // it. A refusal (NOT_PERMITTED for high/realtime without CAP_SYS_NICE) retries with Dawn's own request.
        const auto plain_info = vk_info;
        std::vector<VkDeviceQueueCreateInfo> queues;
        std::vector<VkDeviceQueueGlobalPriorityCreateInfoKHR> priorities;
        std::vector<const char*> device_extensions;
        const int priority = QueuePrioritySetting();
        if (priority != 0) {
            const char* ext = PriorityExtension(get_proc, owner.vk_instance_, vk_physical);
            std::ostringstream line;
            line << "queue priority: KARTFRAME_QUEUE_PRIORITY " << priority << ", ";
            if (ext == nullptr) {
                line << "no global priority extension on this device; Dawn's queue keeps the default";
            } else {
                queues.assign(vk_info.pQueueCreateInfos, vk_info.pQueueCreateInfos + vk_info.queueCreateInfoCount);
                priorities.resize(queues.size());
                for (size_t i = 0; i < queues.size(); ++i) {
                    priorities[i] = VkDeviceQueueGlobalPriorityCreateInfoKHR{VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR};
                    priorities[i].pNext = queues[i].pNext;
                    priorities[i].globalPriority = static_cast<VkQueueGlobalPriorityKHR>(priority);
                    queues[i].pNext = &priorities[i];
                }
                vk_info.pQueueCreateInfos = queues.data();
                device_extensions.assign(vk_info.ppEnabledExtensionNames, vk_info.ppEnabledExtensionNames + vk_info.enabledExtensionCount);
                bool have = false;
                for (const char* e : device_extensions) have |= std::strcmp(e, ext) == 0;
                if (!have) device_extensions.push_back(ext);
                vk_info.enabledExtensionCount = static_cast<uint32_t>(device_extensions.size());
                vk_info.ppEnabledExtensionNames = device_extensions.data();
                line << "chained on " << queues.size() << " queue(s) with " << ext;
            }
            owner.Log(OpenXRLogLevel::Info, line.str());
        }
        XrVulkanDeviceCreateInfoKHR create{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
        create.systemId = owner.runtime_->SystemId();
        create.pfnGetInstanceProcAddr = get_proc;
        create.vulkanPhysicalDevice = vk_physical;
        create.vulkanCreateInfo = &vk_info;
        create.vulkanAllocator = static_cast<const VkAllocationCallbacks*>(allocator);
        VkResult result = VK_ERROR_INITIALIZATION_FAILED;
        XrResult xr = owner.create_device_(owner.runtime_->Instance(), &create, reinterpret_cast<VkDevice*>(out), &result);
        if (!queues.empty()) {
            if (XR_FAILED(xr) || result != VK_SUCCESS) {
                owner.Log(OpenXRLogLevel::Warning, "queue priority: device creation refused (XrResult " + std::to_string(xr) +
                                                 ", VkResult " + std::to_string(result) + "); retrying with Dawn's own request");
                vk_info = plain_info;
                result = VK_ERROR_INITIALIZATION_FAILED;
                xr = owner.create_device_(owner.runtime_->Instance(), &create, reinterpret_cast<VkDevice*>(out), &result);
            } else {
                owner.Log(OpenXRLogLevel::Info, "queue priority: device created with global priority " + std::to_string(priority));
            }
        }
        if (XR_SUCCEEDED(xr) && result == VK_SUCCESS) {
            owner.timeline_semaphores_ = timeline.timelineSemaphore == VK_TRUE;
            owner.LogDeviceLayers(get_proc, vk_physical);
        }
        return XR_SUCCEEDED(xr) ? result : VK_ERROR_INITIALIZATION_FAILED;
    }
    // KARTFRAME_QUEUE_PRIORITY (lane Y): the VkQueueGlobalPriorityKHR value, 0 when unset or unknown.
    static int QueuePrioritySetting() {
        static const int value = [] {
            const char* env = std::getenv("KARTFRAME_QUEUE_PRIORITY");
            if (env == nullptr || *env == '\0') return 0;
            const std::string_view v(env);
            return v == "low" ? static_cast<int>(VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR)
                 : v == "medium" ? static_cast<int>(VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR)
                 : v == "high" ? static_cast<int>(VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR)
                 : v == "realtime" ? static_cast<int>(VK_QUEUE_GLOBAL_PRIORITY_REALTIME_KHR) : 0;
        }();
        return value;
    }
    static const char* PriorityExtension(PFN_vkGetInstanceProcAddr get_proc, VkInstance instance, VkPhysicalDevice physical) {
        if (!get_proc || !instance) return nullptr;
        const auto enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
            get_proc(instance, "vkEnumerateDeviceExtensionProperties"));
        uint32_t count = 0;
        if (!enumerate || enumerate(physical, nullptr, &count, nullptr) != VK_SUCCESS) return nullptr;
        std::vector<VkExtensionProperties> offered(count);
        if (enumerate(physical, nullptr, &count, offered.data()) != VK_SUCCESS) return nullptr;
        bool khr = false, ext = false;
        for (uint32_t i = 0; i < count && i < offered.size(); ++i) {
            khr |= std::strcmp(offered[i].extensionName, VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME) == 0;
            ext |= std::strcmp(offered[i].extensionName, VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME) == 0;
        }
        return khr ? VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME : ext ? VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME : nullptr;
    }
    // The Vulkan layer chain, logged once at startup: Valve's FDM injection (an implicit
    // layer of SteamVR's) and the validation layer change what reaches the driver.
    // DISABLE_VULKAN_FDM_INJECTION_LAYER=1 is the A/B switch for the former.
    void LogInstanceLayers(PFN_vkGetInstanceProcAddr get_proc, const VkInstanceCreateInfo& info) const {
        if (!get_proc) return;
        std::ostringstream line;
        line << "Vulkan instance: API " << VK_API_VERSION_MAJOR(instance_api_version_) << '.'
             << VK_API_VERSION_MINOR(instance_api_version_) << ", layers asked for by Dawn:";
        for (uint32_t i = 0; i < info.enabledLayerCount; ++i) line << ' ' << info.ppEnabledLayerNames[i];
        if (info.enabledLayerCount == 0) line << " none";
        const auto enumerate = reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(
            get_proc(nullptr, "vkEnumerateInstanceLayerProperties"));
        uint32_t count = 0;
        if (enumerate && enumerate(&count, nullptr) == VK_SUCCESS) {
            std::vector<VkLayerProperties> layers(count);
            if (enumerate(&count, layers.data()) == VK_SUCCESS) {
                line << "; loader offers " << count << ':';
                for (uint32_t i = 0; i < count; ++i) line << ' ' << layers[i].layerName;
            }
        }
        const char* fdm = std::getenv("DISABLE_VULKAN_FDM_INJECTION_LAYER");
        line << "; DISABLE_VULKAN_FDM_INJECTION_LAYER=" << (fdm ? fdm : "(unset)");
        Log(OpenXRLogLevel::Info, line.str());
    }
    void LogDeviceLayers(PFN_vkGetInstanceProcAddr get_proc, VkPhysicalDevice physical) const {
        if (!get_proc || !vk_instance_) return;
        // The loader reports the instance's active layer chain here.
        const auto enumerate = reinterpret_cast<PFN_vkEnumerateDeviceLayerProperties>(
            get_proc(vk_instance_, "vkEnumerateDeviceLayerProperties"));
        uint32_t count = 0;
        if (!enumerate || enumerate(physical, &count, nullptr) != VK_SUCCESS) return;
        std::vector<VkLayerProperties> layers(count);
        if (enumerate(physical, &count, layers.data()) != VK_SUCCESS) return;
        std::ostringstream line;
        line << "Vulkan layer chain (active, " << count << "):";
        for (uint32_t i = 0; i < count; ++i) line << ' ' << layers[i].layerName;
        if (count == 0) line << " none";
        Log(OpenXRLogLevel::Info, line.str());
    }

    bool QueryGraphicsRequirements(OpenXRRuntime& runtime) {
        ClearError();
        if (!runtime.IsInitialized() || runtime.HasSession()) return Fail("Vulkan requirements need an initialized XR instance without a session");
        if (requirements_queried_ && runtime_ != &runtime)
            return Fail("Vulkan graphics requirements were already queried from another OpenXR instance");
        runtime_ = &runtime;
        PFN_xrGetVulkanGraphicsRequirements2KHR get = nullptr;
        if (!runtime.LoadFunction("xrGetVulkanGraphicsRequirements2KHR", &get) || !get ||
            !runtime.LoadFunction("xrCreateVulkanInstanceKHR", &create_instance_) || !create_instance_ ||
            !runtime.LoadFunction("xrCreateVulkanDeviceKHR", &create_device_) || !create_device_ ||
            !runtime.LoadFunction("xrGetVulkanGraphicsDevice2KHR", &get_device_) || !get_device_)
            return Fail("PC Vulkan requires XR_KHR_vulkan_enable2");
        XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
        if (XR_FAILED(get(runtime.Instance(), runtime.SystemId(), &requirements))) return Fail("Vulkan requirements query failed");
        requirements_ = {requirements.minApiVersionSupported, requirements.maxApiVersionSupported};
        AuroraDawnVulkanHooks hooks{this, CreateInstance, CreateDevice, GetPhysical};
        if (!aurora_vulkan_win32_configure(&hooks)) return Fail("PC Vulkan OpenXR requires the custom Dawn library with Aurora Vulkan ABI 1; rebuild/install it with Launcher/Build-DawnVulkan.ps1");
        hooks_installed_ = true;
        {
            std::lock_guard lock(submission_mutex_);
            shutting_down_ = false;
            submission_unsafe_ = false;
        }
        requirements_queried_ = true;
        std::ostringstream message;
        message << "OpenXR Vulkan requirements: API " << XR_VERSION_MAJOR(requirements_.min_api_version) << '.'
                << XR_VERSION_MINOR(requirements_.min_api_version) << " to " << XR_VERSION_MAJOR(requirements_.max_api_version)
                << '.' << XR_VERSION_MINOR(requirements_.max_api_version) << "; Dawn will create its device through the runtime";
        Log(OpenXRLogLevel::Info, message.str());
        return true;
    }
    bool BindAurora(OpenXRRuntime& runtime) {
        ClearError();
        if (!requirements_queried_ || runtime_ != &runtime || runtime.HasSession())
            return Fail("QueryGraphicsRequirements must succeed on this OpenXR instance before BindAurora");
        if (bound_) return Fail("OpenXR Vulkan backend is already bound");
        AuroraDawnVulkanHandles handles{};
        int64_t format = 0;
        if (!aurora_vulkan_win32_get_handles(&handles, &format)) return Fail("Aurora did not expose its native Vulkan device");
        void* physical = nullptr;
        if (GetPhysical(this, handles.instance, &physical) != VK_SUCCESS || physical != handles.physicalDevice)
            return Fail("Dawn's Vulkan GPU does not match the OpenXR runtime");
        XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
        binding.instance = static_cast<VkInstance>(handles.instance);
        binding.physicalDevice = static_cast<VkPhysicalDevice>(handles.physicalDevice);
        binding.device = static_cast<VkDevice>(handles.device);
        binding.queueFamilyIndex = handles.queueFamily;
        binding.queueIndex = handles.queueIndex;
        runtime.SetGraphicsQueueGuard(aurora_vulkan_win32_lock_queue, aurora_vulkan_win32_unlock_queue);
        if (!runtime.CreateSession(&binding)) return Fail("OpenXR rejected Dawn's Vulkan device binding");
        owns_session_ = true;
        aurora_format_ = static_cast<VkFormat>(format);
        const auto abandon_session = [&] {
            DestroySwapchains();
            runtime.DestroySession();
            runtime.SetGraphicsQueueGuard(nullptr, nullptr);
            owns_session_ = false;
        };
        if (!SelectSwapchainFormat() || !CreateSwapchains()) {
            abandon_session();
            return false;
        }
        if (runtime.ShouldExit()) {
            abandon_session();
            return Fail("OpenXR session became loss-pending while creating Vulkan swapchains");
        }
        const auto gpu_mode = gpu_.Start(vk_get_proc_, static_cast<VkInstance>(handles.instance),
                                         static_cast<VkDevice>(handles.device), handles.queueFamily,
                                         handles.queueIndex, timeline_semaphores_);
        const bool cmds_ready = cmds_.Start(vk_get_proc_, static_cast<VkInstance>(handles.instance),
                                            static_cast<VkDevice>(handles.device), handles.queueFamily, handles.queueIndex);
        if (!aurora_vulkan_win32_enable(&Impl::OnAuroraSubmitted, this)) {
            gpu_.Stop();
            cmds_.Stop();
            abandon_session();
            return Fail("Aurora could not enable its zero-readback Vulkan stereo bridge");
        }
        bridge_enabled_ = true;
        bound_ = true;
        Log(gpu_mode == GpuCompletion::Mode::Drain ? OpenXRLogLevel::Warning : OpenXRLogLevel::Info,
            gpu_.SkipsWait() ? "OpenXR Vulkan GPU completion: NOT waited for before a release (KARTFRAME_NO_GPU_WAIT)"
            : gpu_mode == GpuCompletion::Mode::Timeline
                ? "OpenXR Vulkan GPU completion: timeline semaphore after each eye set; images are released only once it signals"
            : gpu_mode == GpuCompletion::Mode::Drain
                ? "OpenXR Vulkan GPU completion: no timeline semaphores; each release drains the queue first"
                : "OpenXR Vulkan GPU completion: not available (no Vulkan entry points); releases follow Aurora's submission");
        std::ostringstream message;
        message << "OpenXR Vulkan swapchains ready: VkFormat " << static_cast<int64_t>(swapchain_format_)
                << " (Aurora " << static_cast<int64_t>(aurora_format_) << "), eye swapchains VkFormat "
                << static_cast<int64_t>(eye_format_) << " usage 0x" << std::hex << EyeUsage() << std::dec
                << " (KARTFRAME_XR_FORMAT=" << (RgbaEyesSetting() ? "rgba" : "bgra") << ", KARTFRAME_XR_USAGE="
                << (GalaxyUsageSetting() ? "galaxy" : "kart") << "), end locate "
                << (EndLocateSetting() ? "on" : "off") << " (KARTFRAME_END_LOCATE), first-write barrier "
                << (!FirstWriteBarrierSetting() ? "dawn" : cmds_ready ? "all" : "dawn (no command buffers)")
                << " (KARTFRAME_FIRST_WRITE_BARRIER), race quad "
                << (RaceQuadSetting() == 0 ? "off" : !cmds_ready ? "off (no command buffers)" : RaceQuadSetting() == 2 ? "cycle" : "on")
                << " (KARTFRAME_RACE_QUAD), eyes "
                << eye_swapchains_[0].width << 'x' << eye_swapchains_[0].height << " / "
                << eye_swapchains_[1].width << 'x' << eye_swapchains_[1].height << ", Vulkan "
                << VK_API_VERSION_MAJOR(instance_api_version_) << '.' << VK_API_VERSION_MINOR(instance_api_version_)
                << " instance, timeline semaphores " << (timeline_semaphores_ ? "enabled" : "not enabled")
                << "; same-queue native eye copies";
        Log(OpenXRLogLevel::Info, message.str());
        if (StereoPackedSetting()) {
            std::ostringstream packed;
            packed << "stereo packed: on (" << eye_swapchains_[0].width << 'x' << eye_swapchains_[0].height
                   << ", one swapchain per set)";
            Log(OpenXRLogLevel::Info, packed.str());
        }
        return true;
    }

    OpenXRWindowsVulkanBeginStatus BeginFrame(const OpenXRWindowsVulkanPresentation& presentation,
                                      OpenXRWindowsVulkanFrame& frame) {
        frame = {};
        frame.presentation = presentation;
        if (!bound_ || runtime_ == nullptr) {
            Fail("BeginFrame called before the Vulkan backend was bound");
            return OpenXRWindowsVulkanBeginStatus::Error;
        }
        if (frame_active_ || pending_packet_serial_ != 0) {
            Fail("BeginFrame called while another OpenXR frame is active");
            return OpenXRWindowsVulkanBeginStatus::Error;
        }

        const OpenXRFrameStatus status = runtime_->WaitFrame(frame.xr_frame);
        if (status != OpenXRFrameStatus::Ready) {
            if (status == OpenXRFrameStatus::Error) {
                Fail(WindowsVkBeginStatusOperation(status));
            }
            switch (status) {
            case OpenXRFrameStatus::SessionNotRunning:
                return OpenXRWindowsVulkanBeginStatus::SessionNotRunning;
            case OpenXRFrameStatus::ExitRequested:
                return OpenXRWindowsVulkanBeginStatus::ExitRequested;
            case OpenXRFrameStatus::Error:
                return OpenXRWindowsVulkanBeginStatus::Error;
            case OpenXRFrameStatus::Ready:
                break;
            }
        }
        NoteDisplayTiming(frame.xr_frame);
        if (!runtime_->BeginFrame(frame.xr_frame)) {
            Fail("xrBeginFrame failed");
            return OpenXRWindowsVulkanBeginStatus::Error;
        }
        frame_active_ = true;
        active_frame_serial_ = frame.xr_frame.serial;
        active_frame_ = frame.xr_frame;
        render_session_serial_ = runtime_->SessionRunSerial();
        render_space_serial_ = runtime_->LastReferenceSpaceChange().serial;

        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            frame.render_width[eye] = eye_width_[eye];
            frame.render_height[eye] = eye_height_[eye];
        }

        if (!frame.xr_frame.should_render) {
            return OpenXRWindowsVulkanBeginStatus::Ready;
        }
        if (!runtime_->LocateViews(frame.xr_frame)) {
            Fail("xrLocateViews failed");
            EndActiveFrameWithoutLayers(frame.xr_frame);
            return OpenXRWindowsVulkanBeginStatus::Error;
        }
        active_frame_ = frame.xr_frame;
        if (!frame.xr_frame.views_valid) {
            return OpenXRWindowsVulkanBeginStatus::Ready;
        }

        return PrepareTargets(frame);
    }

    // Both pacing paths retain ownership until completion or cancellation.
    OpenXRWindowsVulkanBeginStatus PrepareTargets(OpenXRWindowsVulkanFrame& frame) {
        // Lane J: no eye pair has cooled off long enough (see RotateEyePairs); lane K (sets rendered
        // ahead): no pair free. Nothing is rendered this refresh, the cycle repeats the retained layer.
        if (ahead_ ? !LoadAheadSlot() : !TakeCooledPair()) {
            pacing::OnPairWait();
            frame.expects_gpu_submission = false;
            return OpenXRWindowsVulkanBeginStatus::Ready;
        }
        const bool virtual_screen = frame.presentation.mode == OpenXRWindowsVulkanFrameMode::VirtualScreen;
        // The menus' environment: target 1 is the packed two-eye environment image, not the right eye.
        const bool environment = virtual_screen && frame.presentation.menu_environment && EnsureEnvSwapchains();
        frame.presentation.menu_environment = environment;
        const uint32_t target_count = virtual_screen && !environment ? 1u : kOpenXREyeCount;
        if (target_count == 1) {
            frame.render_width[1] = frame.render_width[0];
            frame.render_height[1] = frame.render_height[0];
        } else if (environment) {
            frame.render_width[1] = env_swapchain_.width;
            frame.render_height[1] = env_swapchain_.height;
        }

        std::array<AuroraD3D12StereoTarget, kOpenXREyeCount> targets{};
        const diagnostics::Stopwatch acquire_timer;
        // KARTFRAME_STEREO_PACKED: the pair's one packed image takes both eyes (or, in the menus, the screen in
        // its left half); Aurora's sink sees the same image twice (or a target twice its eye's width) and
        // writes each eye into its half.
        const bool packed = eye_swapchains_[1].handle == XR_NULL_HANDLE;
        for (uint32_t eye = 0; eye < target_count; ++eye) {
            if (packed && eye == 1 && !environment) {
                targets[1] = targets[0];
                continue;
            }
            auto& swapchain = environment && eye == 1 ? env_swapchain_ : eye_swapchains_[eye];
            // A canceled set leaves its images acquired and waited (nothing wrote them); reuse them.
            if (!(swapchain.acquired && swapchain.waited) && !AcquireSwapchain(swapchain)) {
                ReleaseAcquiredSwapchains();
                EndActiveFrameWithoutLayers(active_frame_);
                return OpenXRWindowsVulkanBeginStatus::Error;
            }
            targets[eye] = {
                reinterpret_cast<void*>(swapchain.images[swapchain.acquired_index].image),
                swapchain.width,
                swapchain.height,
                static_cast<int64_t>(environment && eye == 1 ? swapchain_format_ : eye_format_),
            };
        }
        // The settings panel's layer image, rendered with the eyes while it is open.
        AuroraD3D12StereoTarget panel_target{};
        const bool panel = frame.presentation.panel.requested && EnsurePanelSwapchains();
        frame.presentation.panel.requested = panel;
        if (panel) {
            if (!(panel_swapchain_.acquired && panel_swapchain_.waited) && !AcquireSwapchain(panel_swapchain_)) {
                ReleaseAcquiredSwapchains();
                EndActiveFrameWithoutLayers(active_frame_);
                return OpenXRWindowsVulkanBeginStatus::Error;
            }
            panel_target = {
                reinterpret_cast<void*>(panel_swapchain_.images[panel_swapchain_.acquired_index].image),
                panel_swapchain_.width,
                panel_swapchain_.height,
                static_cast<int64_t>(swapchain_format_),
            };
        }
        diagnostics::OnSwapchainAcquire(acquire_timer);
        if (FirstWriteBarrierSetting() && cmds_.Ready()) {
            // Lane Q: GalaxyFrame's barrier before the set's first writes (eyes, environment, panel).
            std::array<VkImage, kOpenXREyeCount + 1> images{};
            uint32_t image_count = 0;
            for (uint32_t eye = 0; eye < target_count; ++eye)
                if (eye == 0 || targets[eye].resource != targets[0].resource)
                    images[image_count++] = static_cast<VkImage>(targets[eye].resource);
            if (panel) images[image_count++] = static_cast<VkImage>(panel_target.resource);
            if (!cmds_.FirstWriteBarrier(images.data(), image_count) && !first_write_barrier_failed_) {
                first_write_barrier_failed_ = true;
                Log(OpenXRLogLevel::Warning, "OpenXR first-write barrier could not be submitted; Dawn's own barrier alone from here");
            }
            ++first_write_barriers_;
        }

        {
            std::lock_guard lock(submission_mutex_);
            awaiting_token_ = frame.xr_frame.serial;
            submitted_token_ = 0;
            submission_arrived_ = false;
            submission_success_ = false;
            submission_unsafe_ = false;
        }
        if (!diagnostics::Measure(diagnostics::Stage::SetTargets, [&] {
            return aurora_vulkan_win32_set_targets_with_panel(frame.xr_frame.serial, targets.data(), target_count,
                                                              panel ? &panel_target : nullptr);
        })) {
            {
                std::lock_guard lock(submission_mutex_);
                awaiting_token_ = 0;
            }
            ReleaseAcquiredSwapchains();
            Fail("Aurora rejected the acquired OpenXR Vulkan swapchain target");
            EndActiveFrameWithoutLayers(active_frame_);
            return OpenXRWindowsVulkanBeginStatus::Error;
        }
        frame.expects_gpu_submission = true;
        return OpenXRWindowsVulkanBeginStatus::Ready;
    }

    // Vulkan renders straight into the non-retained XR swapchain pair. Acquiring
    // images is independent of the compositor cycle; keep them acquired while
    // Aurora owns them, and never expose them through the retained pair early.
    OpenXRBeginStatus PreparePacket(const OpenXRPresentation& presentation, OpenXRBackendFrame& packet) {
        packet = {};
        packet.presentation = presentation;
        if (!bound_ || runtime_ == nullptr || frame_active_ || pending_packet_serial_ != 0) {
            Fail("PreparePacket called before binding or with work pending");
            return OpenXRBeginStatus::Error;
        }
        if (runtime_->ShouldExit()) return OpenXRBeginStatus::ExitRequested;
        if (!runtime_->IsSessionRunning()) return OpenXRBeginStatus::SessionNotRunning;
        if (timing_session_serial_ != runtime_->SessionRunSerial() || last_display_period_ <= 0) {
            const auto status = KeepAliveCycle();
            if (status != OpenXRBeginStatus::Ready) return status;
        }
        packet.xr_frame.serial = next_packet_serial_++;
        packet.xr_frame.predicted_display_time = last_display_time_ + 2 * last_display_period_;
        packet.xr_frame.predicted_display_period = last_display_period_;
        packet.xr_frame.should_render = last_should_render_;
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            packet.render_width[eye] = eye_width_[eye];
            packet.render_height[eye] = eye_height_[eye];
        }
        if (!packet.xr_frame.should_render) return OpenXRBeginStatus::Ready;
        if (!runtime_->LocateViewsAt(packet.xr_frame.predicted_display_time, packet.xr_frame)) {
            Fail("xrLocateViews failed for a Vulkan packet");
            return OpenXRBeginStatus::Error;
        }
        if (!packet.xr_frame.views_valid) return OpenXRBeginStatus::Ready;
        render_session_serial_ = runtime_->SessionRunSerial();
        render_space_serial_ = runtime_->LastReferenceSpaceChange().serial;
        const auto status = PrepareTargets(packet);
        if (status == OpenXRBeginStatus::Ready && packet.expects_gpu_submission) pending_packet_serial_ = packet.xr_frame.serial;
        return status;
    }

    bool TryCancelPendingPacket(OpenXRBackendFrame& packet) {
        if (frame_active_ || pending_packet_serial_ == 0 ||
            packet.xr_frame.serial != pending_packet_serial_ || !packet.expects_gpu_submission ||
            !aurora_vulkan_win32_cancel(packet.xr_frame.serial)) return false;
        {
            std::lock_guard lock(submission_mutex_);
            awaiting_token_ = submitted_token_ = 0;
            submission_arrived_ = submission_success_ = submission_unsafe_ = false;
        }
        packet.expects_gpu_submission = false;
        pending_packet_serial_ = 0;
        const diagnostics::Stopwatch release_timer;
        const bool released = ReleaseAcquiredSwapchains();
        diagnostics::OnSwapchainRelease(release_timer);
        // A release error is fatal; keep it visible to the next prepare rather
        // than letting it register new targets over still-acquired images.
        if (!released) pending_packet_serial_ = packet.xr_frame.serial;
        return true; // Encoding was canceled; a release error blocks the next prepare.
    }

    void NoteDisplayTiming(const OpenXRFrame& frame) {
        last_display_time_ = frame.predicted_display_time;
        last_display_period_ = frame.predicted_display_period;
        last_should_render_ = frame.should_render;
        timing_session_serial_ = runtime_->SessionRunSerial();
    }

    OpenXRBeginStatus BeginCompositorCycle() {
        const auto status = runtime_->WaitFrame(active_frame_);
        wake_ = std::chrono::steady_clock::now();
        release_at_ = {};
        ++cycle_count_;
        if (status != OpenXRFrameStatus::Ready) {
            if (status == OpenXRFrameStatus::Error) Fail(WindowsVkBeginStatusOperation(status));
            return status == OpenXRFrameStatus::SessionNotRunning ? OpenXRBeginStatus::SessionNotRunning
                 : status == OpenXRFrameStatus::ExitRequested ? OpenXRBeginStatus::ExitRequested
                                                             : OpenXRBeginStatus::Error;
        }
        NoteDisplayTiming(active_frame_);
        if (!runtime_->BeginFrame(active_frame_)) {
            Fail("xrBeginFrame failed for a Vulkan compositor cycle");
            return OpenXRBeginStatus::Error;
        }
        frame_active_ = true;
        return OpenXRBeginStatus::Ready;
    }

    OpenXRBeginStatus KeepAliveCycle() {
        if (!bound_ || runtime_ == nullptr || frame_active_) {
            Fail("KeepAliveCycle called before binding or with an active frame");
            return OpenXRBeginStatus::Error;
        }
        const auto status = BeginCompositorCycle();
        if (status != OpenXRBeginStatus::Ready) return status;
        const bool ended = EndRetainedFrame(false);
        frame_active_ = false;
        active_frame_ = {};
        if (!ended) {
            Fail("OpenXR could not resubmit the retained Vulkan frame");
            return OpenXRBeginStatus::Error;
        }
        return OpenXRBeginStatus::Ready;
    }

    OpenXRBeginStatus BeginFrameForPacket(const OpenXRBackendFrame& packet, OpenXRBackendFrame& frame) {
        frame = {};
        if (!bound_ || runtime_ == nullptr || frame_active_ || pending_packet_serial_ == 0 ||
            packet.xr_frame.serial != pending_packet_serial_ || !packet.expects_gpu_submission ||
            WaitForSubmission(packet, 0) != OpenXRSubmissionStatus::Success) {
            Fail("BeginFrameForPacket requires the completed current Vulkan packet");
            return OpenXRBeginStatus::Error;
        }
        const auto status = BeginCompositorCycle();
        if (status != OpenXRBeginStatus::Ready) {
            // Completion was already confirmed. A stopped session must not
            // strand a packet and block preparation after the next READY event.
            if (status == OpenXRBeginStatus::SessionNotRunning) {
                const bool released = ReleaseAcquiredSwapchains();
                pending_packet_serial_ = 0;
                std::lock_guard lock(submission_mutex_);
                awaiting_token_ = submitted_token_ = 0;
                submission_arrived_ = submission_success_ = submission_unsafe_ = false;
                if (!released) return OpenXRBeginStatus::Error;
            }
            return status;
        }
        frame = packet;
        // Use the current compositor token/time but the original render poses.
        frame.xr_frame.serial = active_frame_.serial;
        frame.xr_frame.predicted_display_time = active_frame_.predicted_display_time;
        frame.xr_frame.predicted_display_period = active_frame_.predicted_display_period;
        frame.xr_frame.should_render = active_frame_.should_render;
        active_frame_serial_ = frame.xr_frame.serial;
        {
            std::lock_guard lock(submission_mutex_);
            awaiting_token_ = submitted_token_ = frame.xr_frame.serial;
        }
        pending_packet_serial_ = 0;
        return OpenXRBeginStatus::Ready;
    }

    // ---- Per-refresh eye sets (VR interpolation: inline and pipelined pacing) ----
    //
    // A set is one acquired image per eye (plus the panel's), located for one display
    // time and rendered by Aurora into the pending swapchain pair. Compositor cycles run
    // independently: BeginCycle opens one; EndCycleWithSet shows a finished set with its
    // own poses; EndCycleRetained shows the retained pair again while a set may stay
    // acquired. At most one set is pending, so each swapchain has at most one waited image
    // (the other pair holds the image on display).

    OpenXRBeginStatus BeginCycle(OpenXRFrame& cycle) {
        cycle = {};
        if (!bound_ || runtime_ == nullptr || frame_active_) {
            Fail("BeginCycle called before binding or with a compositor frame open");
            return OpenXRBeginStatus::Error;
        }
        const auto status = BeginCompositorCycle();
        if (status != OpenXRBeginStatus::Ready) return status;
        cycle_wake_ = std::chrono::steady_clock::now();
        active_frame_serial_ = active_frame_.serial;
        cycle = active_frame_;
        return OpenXRBeginStatus::Ready;
    }

    std::chrono::steady_clock::time_point CycleWakeTime() const { return cycle_wake_; }
    bool HasPendingSet() const { return pending_packet_serial_ != 0; }

    OpenXRBeginStatus PrepareSet(const OpenXRPresentation& presentation, XrTime display_time,
                                 const OpenXRFrame& cycle, OpenXRBackendFrame& set) {
        set = {};
        set.presentation = presentation;
        if (!bound_ || runtime_ == nullptr || pending_packet_serial_ != 0) {
            Fail("PrepareSet called before binding or with a set pending");
            return OpenXRBeginStatus::Error;
        }
        if (runtime_->ShouldExit()) return OpenXRBeginStatus::ExitRequested;
        if (!runtime_->IsSessionRunning()) return OpenXRBeginStatus::SessionNotRunning;
        set.xr_frame.serial = next_packet_serial_++;
        set.xr_frame.predicted_display_period = cycle.predicted_display_period;
        set.xr_frame.should_render = cycle.should_render;
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            set.render_width[eye] = eye_width_[eye];
            set.render_height[eye] = eye_height_[eye];
        }
        if (!set.xr_frame.should_render) {
            set.xr_frame.predicted_display_time = display_time;
            return OpenXRBeginStatus::Ready;
        }
        if (!runtime_->LocateViewsAt(display_time, set.xr_frame)) {
            Fail("xrLocateViews failed for a Vulkan eye set");
            return OpenXRBeginStatus::Error;
        }
        if (!set.xr_frame.views_valid) return OpenXRBeginStatus::Ready;
        render_session_serial_ = runtime_->SessionRunSerial();
        render_space_serial_ = runtime_->LastReferenceSpaceChange().serial;
        // The per-refresh sets rotate through all eye pairs (RotateEyePairs); the render-first path
        // (interpolation off) keeps the two it always had.
        if (!extra_pairs_made_) MakeExtraEyePairs();
        if (ahead_) {
            if (!AheadCanPrepare()) {
                Fail("PrepareSet called while Aurora still holds an unsubmitted eye set");
                return OpenXRBeginStatus::Error;
            }
            std::lock_guard lock(submission_mutex_);
            ahead_expected_.push_back(set.xr_frame.serial);
        }
        // A failure here is fatal for the session (the caller ends it).
        const auto status = PrepareTargets(set);
        if (ahead_) {
            if (status == OpenXRBeginStatus::Ready && set.expects_gpu_submission) {
                // The set's images travel with it until it is shown or dropped (LoadAheadSlot, RestoreAheadSlot).
                AheadSlot slot;
                slot.serial = set.xr_frame.serial;
                slot.eyes = std::exchange(eye_swapchains_, EyePair{});
                slot.env = std::exchange(env_swapchain_, EyeSwapchain{});
                slot.panel = std::exchange(panel_swapchain_, EyeSwapchain{});
                slot.session_serial = render_session_serial_;
                slot.space_serial = render_space_serial_;
                ahead_slots_.push_back(std::move(slot));
            } else {
                std::lock_guard lock(submission_mutex_);
                ForgetExpected(set.xr_frame.serial);
            }
            return status;
        }
        if (status == OpenXRBeginStatus::Ready && set.expects_gpu_submission) pending_packet_serial_ = set.xr_frame.serial;
        return status;
    }

    // ---- Sets rendered ahead (lane K) ----
    //
    // The seventh headset session (frame/perf/H7.md): in races the old pipelined loop ended each set
    // whenever its GPU work finished, up to 7 ms after SteamVR's wake-up, later by about 1.7 ms each
    // refresh (a set started only after the last one was ended), and SteamVR showed those frames
    // inconsistently: the image zigzagged between two positions on head turns. Menus (ended within
    // 0.2 ms of the wake-up) and GalaxyFrame (0.3 to 1 ms) were clean. Sets rendered ahead end every
    // frame at the wake-up with the newest set already finished: a set is started two refreshes
    // before its display time, so one set is on the GPU while the next is encoded, and each has a
    // whole extra refresh of margin. Two in flight plus the pair on display: three pairs at least.
    // A set's pair is never the displayed one, so an overtaken set can be released unshown without
    // touching what the retained layer shows (a release makes an image its swapchain's newest).

    bool EnableAhead(bool want) {
        ahead_ = want && !extra_pairs_made_ && bound_ &&
                 (gpu_.CurrentMode() == GpuCompletion::Mode::Timeline || gpu_.SkipsWait());
        eye_pairs_wanted_ = ahead_ ? AheadPairsSetting() : EyePairsSetting();
        return ahead_;
    }

    bool AheadCanPrepare() const {
        std::lock_guard lock(submission_mutex_);
        return ahead_expected_.empty();
    }

    OpenXRSubmissionStatus PollAheadSet(const OpenXRBackendFrame& set, std::chrono::steady_clock::time_point deadline,
                                        bool gpu) {
        if (!set.expects_gpu_submission) return OpenXRSubmissionStatus::Success;
        uint64_t value = 0;
        {
            std::unique_lock lock(submission_mutex_);
            const auto find = [&] {
                return std::find_if(ahead_submitted_.begin(), ahead_submitted_.end(),
                                    [&](const AheadSubmission& r) { return r.token == set.xr_frame.serial; });
            };
            const auto ready = [&] { return shutting_down_ || find() != ahead_submitted_.end(); };
            if (!ready() && !submission_cv_.wait_until(lock, deadline, ready)) return OpenXRSubmissionStatus::Timeout;
            if (shutting_down_) return OpenXRSubmissionStatus::ShuttingDown;
            const auto record = find();
            if (!record->success) return OpenXRSubmissionStatus::Failed;
            value = record->gpu_value;
        }
        if (!gpu) return OpenXRSubmissionStatus::Success;
        if (gpu_.WaitUntil(value, deadline)) return OpenXRSubmissionStatus::Success;
        return gpu_.Failed() ? OpenXRSubmissionStatus::Failed : OpenXRSubmissionStatus::Timeout;
    }

    bool AheadSetCurrent(const OpenXRBackendFrame& set) const {
        const AheadSlot* slot = FindAheadSlot(set.xr_frame.serial);
        return slot != nullptr && runtime_ != nullptr && slot->session_serial == runtime_->SessionRunSerial() &&
               slot->space_serial == runtime_->LastReferenceSpaceChange().serial;
    }

    // Shows the finished set at once: its images released, the cycle ended with its own poses at
    // this cycle's display time. Call only after PollAheadSet(set) returned Success.
    bool EndCycleWithAheadSet(OpenXRBackendFrame& set) {
        if (!frame_active_) return Fail("EndCycleWithAheadSet needs an open compositor cycle");
        if (!RestoreAheadSlot(set.xr_frame.serial)) return Fail("EndCycleWithAheadSet: not a set in flight");
        {
            std::lock_guard lock(submission_mutex_);
            const auto record = std::find_if(ahead_submitted_.begin(), ahead_submitted_.end(),
                                             [&](const AheadSubmission& r) { return r.token == set.xr_frame.serial; });
            if (record == ahead_submitted_.end()) {
                AbandonAcquiredSwapchains();
                return Fail("EndCycleWithAheadSet: the set was never submitted");
            }
            // FinishFrame's checks (unsafe submission, the last GPU wait) read the single-set state.
            awaiting_token_ = submitted_token_ = set.xr_frame.serial;
            submission_arrived_ = true;
            submission_success_ = record->success;
            submission_unsafe_ = !record->success;
            submitted_gpu_value_ = record->gpu_value;
            ahead_submitted_.erase(record);
        }
        pending_packet_serial_ = set.xr_frame.serial;
        const bool ok = EndCycleWithSet(set);
        // The pair that was on display until now (FinishFrame swapped it out) goes back to the pool.
        ParkAheadSlot();
        return ok;
    }

    bool CancelAheadSet(OpenXRBackendFrame& set) {
        if (!set.expects_gpu_submission || FindAheadSlot(set.xr_frame.serial) == nullptr ||
            !aurora_vulkan_win32_cancel(set.xr_frame.serial)) return false;
        {
            std::lock_guard lock(submission_mutex_);
            ForgetExpected(set.xr_frame.serial);
        }
        // Nothing wrote the images: they stay acquired and waited, and the next set reuses them.
        RestoreAheadSlot(set.xr_frame.serial);
        ParkAheadSlot();
        set.expects_gpu_submission = false;
        return true;
    }

    bool DropAheadSet(OpenXRBackendFrame& set, std::chrono::milliseconds wait) {
        if (FindAheadSlot(set.xr_frame.serial) == nullptr) return true;
        if (CancelAheadSet(set)) return true;
        const auto status = PollAheadSet(set, std::chrono::steady_clock::now() + wait, true);
        RestoreAheadSlot(set.xr_frame.serial);
        {
            std::lock_guard lock(submission_mutex_);
            ForgetExpected(set.xr_frame.serial);
            ahead_submitted_.erase(std::remove_if(ahead_submitted_.begin(), ahead_submitted_.end(),
                                                  [&](const AheadSubmission& r) { return r.token == set.xr_frame.serial; }),
                                   ahead_submitted_.end());
        }
        set.expects_gpu_submission = false;
        if (status != OpenXRSubmissionStatus::Success) {
            // Never released: the images stay acquired until the session ends.
            AbandonAcquiredSwapchains();
            AbandonCurrentSlot();
            return Fail("an OpenXR Vulkan eye set could not be confirmed finished; its images stay acquired");
        }
        const bool released = ReleaseAcquiredSwapchains();
        ParkAheadSlot();
        return released;
    }

    bool CancelSet(OpenXRBackendFrame& set) {
        if (pending_packet_serial_ == 0 || set.xr_frame.serial != pending_packet_serial_ ||
            !set.expects_gpu_submission || !aurora_vulkan_win32_cancel(set.xr_frame.serial)) return false;
        {
            std::lock_guard lock(submission_mutex_);
            awaiting_token_ = submitted_token_ = 0;
            submission_arrived_ = submission_success_ = submission_unsafe_ = false;
        }
        set.expects_gpu_submission = false;
        pending_packet_serial_ = 0;
        // The images stay acquired for the next set (PrepareTargets reuses them): nothing
        // wrote them, and a release here would only churn the swapchain.
        return true;
    }

    // Shows the finished set with its own poses at this cycle's display time. Call only
    // after WaitForSubmissionUntil(set) returned Success.
    bool EndCycleWithSet(OpenXRBackendFrame& set) {
        if (!frame_active_ || pending_packet_serial_ == 0 || set.xr_frame.serial != pending_packet_serial_) {
            return Fail("EndCycleWithSet needs an open compositor cycle and the pending set");
        }
        OpenXRBackendFrame frame = set;
        frame.xr_frame.serial = active_frame_.serial;
        // KARTFRAME_END_TIME=locate ends the layer with the time its poses were located for (EndFrameWithPanel).
        next_layer_locate_time_ = set.xr_frame.predicted_display_time;
        frame.xr_frame.predicted_display_time = active_frame_.predicted_display_time;
        frame.xr_frame.predicted_display_period = active_frame_.predicted_display_period;
        frame.xr_frame.should_render = active_frame_.should_render;
        {
            std::lock_guard lock(submission_mutex_);
            if (submitted_token_ == set.xr_frame.serial) submitted_token_ = frame.xr_frame.serial;
            awaiting_token_ = frame.xr_frame.serial;
        }
        active_frame_serial_ = frame.xr_frame.serial;
        pending_packet_serial_ = 0;
        const bool ok = FinishFrame(frame, true);
        set.expects_gpu_submission = false;
        return ok;
    }

    // Ends the open cycle on the retained layer; a pending set stays acquired.
    bool EndCycleRetained() {
        if (!frame_active_ || runtime_ == nullptr) return Fail("EndCycleRetained needs an open compositor cycle");
        const bool ended = EndRetainedFrame(false);
        frame_active_ = false;
        active_frame_ = {};
        active_frame_serial_ = 0;
        if (!ended) return Fail("OpenXR could not resubmit the retained Vulkan frame");
        return true;
    }

    // A set that will never be shown (session stopped): released only once the GPU
    // finished it.
    bool DropSet(OpenXRBackendFrame& set) {
        if (pending_packet_serial_ == 0 || set.xr_frame.serial != pending_packet_serial_) return true;
        if (CancelSet(set)) return true;
        const auto status = WaitForSubmissionUntil(set, std::chrono::steady_clock::now() + std::chrono::seconds(1));
        if (status != OpenXRSubmissionStatus::Success) {
            AbandonAcquiredSwapchains();
            pending_packet_serial_ = 0;
            return Fail("an OpenXR Vulkan eye set could not be confirmed finished; its images stay acquired");
        }
        pending_packet_serial_ = 0;
        set.expects_gpu_submission = false;
        {
            std::lock_guard lock(submission_mutex_);
            awaiting_token_ = submitted_token_ = 0;
            submission_arrived_ = submission_success_ = submission_unsafe_ = false;
        }
        return ReleaseAcquiredSwapchains();
    }

    OpenXRSubmissionStatus CopyRenderedEyes(const OpenXRBackendFrame& frame) {
        if (!frame_active_ || frame.xr_frame.serial != active_frame_serial_) {
            Fail("CopyRenderedEyes received a stale Vulkan frame");
            return OpenXRSubmissionStatus::Failed;
        }
        // The native bridge already queued the copy on the session's Vulkan
        // queue before publishing completion. There is no second copy on PC.
        return WaitForSubmission(frame, 0);
    }

    OpenXRWindowsVulkanSubmissionStatus WaitForSubmission(const OpenXRWindowsVulkanFrame& frame,
                                                  uint32_t timeout_ms) {
        const auto deadline = timeout_ms == std::numeric_limits<uint32_t>::max()
                                  ? std::chrono::steady_clock::time_point::max()
                                  : std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        return WaitForSubmissionUntil(frame, deadline);
    }

    // Success only once Aurora submitted the set and the GPU finished it.
    OpenXRSubmissionStatus WaitForSubmissionUntil(const OpenXRBackendFrame& frame,
                                                  std::chrono::steady_clock::time_point deadline) {
        if (!frame.expects_gpu_submission) {
            return OpenXRWindowsVulkanSubmissionStatus::Success;
        }
        uint64_t gpu_value = 0;
        {
            std::unique_lock lock(submission_mutex_);
            const auto ready = [&] {
                return shutting_down_ ||
                       (submission_arrived_ && submitted_token_ == frame.xr_frame.serial);
            };
            if (deadline == std::chrono::steady_clock::time_point::max()) {
                submission_cv_.wait(lock, ready);
            } else if (!submission_cv_.wait_until(lock, deadline, ready)) {
                return OpenXRWindowsVulkanSubmissionStatus::Timeout;
            }
            if (shutting_down_) {
                return OpenXRWindowsVulkanSubmissionStatus::ShuttingDown;
            }
            if (!submission_success_) {
                return OpenXRWindowsVulkanSubmissionStatus::Failed;
            }
            gpu_value = submitted_gpu_value_;
        }
        const auto gpu_deadline = deadline == std::chrono::steady_clock::time_point::max()
                                      ? std::chrono::steady_clock::now() + std::chrono::hours(1)
                                      : deadline;
        if (gpu_.WaitUntil(gpu_value, gpu_deadline)) {
            return OpenXRWindowsVulkanSubmissionStatus::Success;
        }
        if (gpu_.Failed()) {
            std::lock_guard lock(submission_mutex_);
            submission_unsafe_ = true;
            submission_success_ = false;
            return OpenXRWindowsVulkanSubmissionStatus::Failed;
        }
        return OpenXRWindowsVulkanSubmissionStatus::Timeout;
    }

    bool TryCancelPendingFrame(OpenXRWindowsVulkanFrame& frame) {
        if (!frame_active_ || !frame.expects_gpu_submission ||
            frame.xr_frame.serial != active_frame_serial_) {
            return false;
        }
        if (!aurora_vulkan_win32_cancel(frame.xr_frame.serial)) {
            return false;
        }

        // A successful bridge cancellation is serialized against Encode and
        // never generates a callback, so this token has no GPU ownership.
        std::lock_guard lock(submission_mutex_);
        awaiting_token_ = 0;
        submitted_token_ = 0;
        submission_arrived_ = false;
        submission_success_ = false;
        submission_unsafe_ = false;
        frame.expects_gpu_submission = false;
        return true;
    }

    bool FinishFrame(OpenXRWindowsVulkanFrame& frame, bool submit_layer) {
        if (!frame_active_ || runtime_ == nullptr ||
            frame.xr_frame.serial != active_frame_serial_) {
            return Fail("FinishFrame received a stale or inactive OpenXR frame token");
        }

        bool submission_unsafe = false;
        {
            std::lock_guard lock(submission_mutex_);
            submission_unsafe = submission_arrived_ &&
                                submitted_token_ == frame.xr_frame.serial &&
                                submission_unsafe_;
        }
        if (submission_unsafe) {
            AbandonAcquiredSwapchains();
            Fail("Aurora's Vulkan stereo submission failed after GPU work may have been queued");
        } else if (frame.expects_gpu_submission) {
            // Callers wait first; this is the last line of defence: SteamVR would show
            // an image the GPU is still writing.
            bool arrived;
            uint64_t gpu_value;
            {
                std::lock_guard lock(submission_mutex_);
                arrived = submission_arrived_ && submitted_token_ == frame.xr_frame.serial && submission_success_;
                gpu_value = submitted_gpu_value_;
            }
            if (arrived && !gpu_.WaitUntil(gpu_value, std::chrono::steady_clock::now() + std::chrono::seconds(1))) {
                AbandonAcquiredSwapchains();
                Fail("an OpenXR Vulkan eye set did not finish on the GPU within 1 s; its images stay acquired");
            }
        }
        const diagnostics::Stopwatch release_timer;
        const auto release_start = std::chrono::steady_clock::now();
        bool release_ok = ReleaseAcquiredSwapchains();
        if (frame.expects_gpu_submission) release_at_ = release_start;  // XRF r= (lane W)
        if (frame.xr_frame.should_render && frame.xr_frame.views_valid) {
            diagnostics::OnSwapchainRelease(release_timer);
        }
        const bool position_valid =
            (frame.xr_frame.view_state_flags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
        const bool composition_pose_valid =
            frame.presentation.mode == OpenXRWindowsVulkanFrameMode::VirtualScreen || position_valid;
        const bool can_submit = submit_layer && release_ok && frame.xr_frame.should_render &&
                                frame.xr_frame.views_valid && frame.expects_gpu_submission &&
                                composition_pose_valid;
        if (submit_layer && !can_submit) {
            diagnostics::OnLayerRejected(diagnostics::ClassifyRejectedLayer(
                release_ok, frame.xr_frame.should_render, frame.xr_frame.views_valid));
        }
        if (can_submit) {
            // xrEndFrame references the MOST RECENTLY RELEASED image of a
            // swapchain, not an explicit image index. Keep the displayed pair
            // separate from the pair Aurora can write or cancel next.
            RotateEyePairs();
            if (frame.presentation.panel.requested) {
                std::swap(panel_swapchain_, retained_panel_swapchain_);
            }
            retained_panel_valid_ = frame.presentation.panel.requested;
            if (frame.presentation.menu_environment) {
                std::swap(env_swapchain_, retained_env_swapchain_);
            }
            retained_env_valid_ = frame.presentation.menu_environment;
            retained_frame_ = frame;
            retained_locate_time_ = next_layer_locate_time_;
            retained_session_serial_ = render_session_serial_;
            retained_space_serial_ = render_space_serial_;
            have_retained_frame_ = true;
        }
        next_layer_locate_time_ = 0;
        const bool end_ok = EndRetainedFrame(can_submit);

        frame_active_ = false;
        active_frame_serial_ = 0;
        active_frame_ = {};
        frame.expects_gpu_submission = false;
        {
            std::lock_guard lock(submission_mutex_);
            awaiting_token_ = 0;
            submission_arrived_ = false;
            submission_success_ = false;
            submission_unsafe_ = false;
        }
        return release_ok && end_ok;
    }

    bool RepeatFrame(const OpenXRWindowsVulkanFrame& frame) {
        if (!frame_active_ || runtime_ == nullptr ||
            frame.xr_frame.serial != active_frame_serial_) {
            return Fail("RepeatFrame received a stale or inactive render token");
        }
        const bool end_ok = EndRetainedFrame(false);
        // EndFrame consumes the compositor token even when submission fails.
        // Teardown must not try to end that same token again.
        frame_active_ = false;
        if (!end_ok) {
            return Fail("OpenXR could not resubmit the retained frame");
        }
        // active_frame_serial_ continues to identify Aurora's pending render;
        // active_frame_ identifies the independently advancing compositor cycle.
        if (runtime_->PollEvents() != OpenXREventStatus::Continue ||
            !runtime_->IsSessionRunning() || runtime_->ShouldExit()) {
            return Fail("OpenXR session stopped while waiting for stereo rendering");
        }
        if (runtime_->WaitFrame(active_frame_) != OpenXRFrameStatus::Ready ||
            !runtime_->BeginFrame(active_frame_)) {
            return Fail("OpenXR could not start a retained-frame compositor cycle");
        }
        NoteDisplayTiming(active_frame_);
        frame_active_ = true;
        return true;
    }

    // fresh: the retained layer was completed for this call rather than repeated.
    bool EndRetainedFrame(bool fresh) {
        if (!runtime_->IsSessionRunning()) {
            // A session that is no longer running needs no compositor frame
            // completion call. Preserve the original backend failure instead
            // of replacing it with a stale-token error.
            return true;
        }
        // Old poses cannot be reused after the runtime changes their coordinate
        // system. Also discard content across session restarts.
        const bool session_changed = retained_session_serial_ != runtime_->SessionRunSerial();
        if (session_changed || retained_space_serial_ != runtime_->LastReferenceSpaceChange().serial) {
            if (have_retained_frame_) {
                diagnostics::OnRetainedLayerDiscarded(session_changed
                                                          ? diagnostics::DiscardReason::SessionRestarted
                                                          : diagnostics::DiscardReason::ReferenceSpaceChanged);
            }
            have_retained_frame_ = false;
        }
        if (!have_retained_frame_ || !active_frame_.should_render) {
            diagnostics::OnEmptyFrame(!active_frame_.should_render ? diagnostics::EmptyFrameReason::ShouldRenderOff
                                                                    : diagnostics::EmptyFrameReason::NoRetainedLayer);
            last_end_display_time_ = std::max(last_end_display_time_, active_frame_.predicted_display_time);
            return runtime_->EndFrameWithoutLayers(active_frame_);
        }
        diagnostics::OnLayer(fresh);
        retained_ref_cycle_ = cycle_count_;
        log_fresh_ = fresh;  // logged by EndFrameWithPanel with the layer count
        const auto& frame = retained_frame_;
        CheckPoses(frame, fresh);
        // KARTFRAME_SUBMIT_POSE=fresh: a race frame showing a new set goes out with the check's poses.
        const bool submit_fresh = fresh && fresh_views_valid_ && SubmitFreshPoseSetting();
        if (frame.presentation.mode == OpenXRWindowsVulkanFrameMode::VirtualScreen) {
            XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
            quad.layerFlags = 0;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = retained_swapchains_[0].handle;
            // Only the picture, not the black bands letterboxing it into the square eye-sized image
            // (as the Android backend): they framed the screen in black.
            // A packed pair (KARTFRAME_STEREO_PACKED): the screen is in the left half.
            const uint32_t image_width = retained_swapchains_[1].handle == XR_NULL_HANDLE
                                             ? retained_swapchains_[0].width / kOpenXREyeCount
                                             : retained_swapchains_[0].width;
            quad.subImage.imageRect = OpenXRVirtualScreenContentRect(
                image_width, retained_swapchains_[0].height, frame.presentation.quad_content_aspect);
            quad.subImage.imageArrayIndex = 0;
            if (frame.presentation.quad_anchored) {
                // Placed in the application space, so the screen keeps its place
                // in the room while the player looks around it.
                quad.space = runtime_->AppSpace();
                quad.pose = frame.presentation.quad_pose;
            } else {
                // No head pose to anchor against yet: keep it in front of the
                // player so the menus are never left stranded behind them.
                quad.space = runtime_->ViewSpace();
                quad.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
                quad.pose.position = {
                    0.0f, 0.0f, -std::max(0.25f, frame.presentation.quad_distance_meters)};
            }
            // The whole image would be quad_width_meters across: the crop keeps that size per pixel,
            // so the picture stays where the pointer and the settings panel expect it.
            const float meters_per_pixel =
                std::max(0.25f, frame.presentation.quad_width_meters) / static_cast<float>(image_width);
            quad.size.width = meters_per_pixel * static_cast<float>(quad.subImage.imageRect.extent.width);
            quad.size.height = meters_per_pixel * static_cast<float>(quad.subImage.imageRect.extent.height);
            // The room around the screen (controllers, pointer, plain background): one projection
            // layer from the packed environment image, with the set's own poses, the frame's only
            // projection layer. Under the quad by default (the screen's area of it is transparent; the
            // pointer's mark is in the screen's own image). KARTFRAME_MENU_ENV=over puts it over the quad,
            // blended by its alpha, which SteamVR on the Frame did not honour (H4.md).
            const bool position_valid = (frame.xr_frame.view_state_flags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
            if (retained_env_valid_ && frame.presentation.menu_environment && frame.xr_frame.views_valid &&
                position_valid && retained_env_swapchain_.handle != XR_NULL_HANDLE) {
                std::array<XrCompositionLayerProjectionView, kOpenXREyeCount> views{};
                const int32_t eye_width = static_cast<int32_t>(retained_env_swapchain_.width / kOpenXREyeCount);
                for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
                    views[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                    views[eye].pose = frame.xr_frame.views[eye].pose;
                    views[eye].fov = frame.xr_frame.views[eye].fov;
                    views[eye].subImage.swapchain = retained_env_swapchain_.handle;
                    views[eye].subImage.imageRect = {{static_cast<int32_t>(eye) * eye_width, 0},
                                                     {eye_width, static_cast<int32_t>(retained_env_swapchain_.height)}};
                    views[eye].subImage.imageArrayIndex = 0;
                }
                XrCompositionLayerProjection environment{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
                environment.space = runtime_->AppSpace();
                environment.viewCount = kOpenXREyeCount;
                environment.views = views.data();
                const bool over = MenuEnvironmentOver();
                environment.layerFlags = over ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
                const XrCompositionLayerBaseHeader* scene[2] = {
                    reinterpret_cast<const XrCompositionLayerBaseHeader*>(over ? static_cast<const void*>(&quad)
                                                                               : static_cast<const void*>(&environment)),
                    reinterpret_cast<const XrCompositionLayerBaseHeader*>(over ? static_cast<const void*>(&environment)
                                                                               : static_cast<const void*>(&quad))};
                return EndFrameWithPanel(frame, scene, 2);
            }
            return EndFrameWithPanel(frame, reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad));
        } else {
            std::array<XrCompositionLayerProjectionView, kOpenXREyeCount> views{};
            for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
                views[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                const XrPosef& pose = submit_fresh ? fresh_views_[eye].pose : frame.xr_frame.views[eye].pose;
                views[eye].pose.orientation = pose.orientation;
                views[eye].pose.position = pose.position;
                views[eye].fov = frame.xr_frame.views[eye].fov;
                // KARTFRAME_STEREO_PACKED: both views from the pair's one image, eye N at x = N * eye width.
                const bool packed = retained_swapchains_[1].handle == XR_NULL_HANDLE;
                const auto& image = retained_swapchains_[packed ? 0 : eye];
                const int32_t width = static_cast<int32_t>(packed ? image.width / kOpenXREyeCount : image.width);
                views[eye].subImage.swapchain = image.handle;
                views[eye].subImage.imageRect = {{packed ? static_cast<int32_t>(eye) * width : 0, 0},
                                                 {width, static_cast<int32_t>(image.height)}};
                views[eye].subImage.imageArrayIndex = 0;
            }
            XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            projection.layerFlags = 0;
            projection.space = runtime_->AppSpace();
            projection.viewCount = kOpenXREyeCount;
            projection.views = views.data();
            return EndFrameWithPanel(frame, reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection));
        }
    }

    // Ends the compositor frame with the scene's layer and, while the retained
    // frame rendered it, the settings panel's layer over it.
    bool EndFrameWithPanel(const OpenXRBackendFrame& frame, const XrCompositionLayerBaseHeader* scene) {
        return EndFrameWithPanel(frame, &scene, 1);
    }
    bool EndFrameWithPanel(const OpenXRBackendFrame& frame, const XrCompositionLayerBaseHeader* const* scene,
                           uint32_t scene_count) {
        const auto& panel = frame.presentation.panel;
        XrCompositionLayerQuad panel_quad{};
        XrCompositionLayerQuad race_quad{};
        const XrCompositionLayerBaseHeader* layers[4] = {scene[0], scene_count > 1 ? scene[1] : nullptr, nullptr, nullptr};
        uint32_t count = std::min(scene_count, 2u);
        if (retained_panel_valid_ && panel.requested && panel.placed) {
            panel_quad = OpenXRPanelQuadLayer(panel, runtime_->AppSpace(), retained_panel_swapchain_.handle);
            layers[count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&panel_quad);
        }
        // Lane Q (KARTFRAME_RACE_QUAD): never a projection layer alone.
        if (count == 1 && layers[0]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION && EnsureRaceQuad()) {
            race_quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
            race_quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            race_quad.space = runtime_->AppSpace();
            race_quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            race_quad.subImage.swapchain = race_quad_.handle;
            race_quad.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(race_quad_.width), static_cast<int32_t>(race_quad_.height)}};
            race_quad.subImage.imageArrayIndex = 0;
            race_quad.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
            race_quad.pose.position = {0.0f, 0.0f, -2.0f};
            race_quad.size = {0.01f, 0.01f};
            layers[count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&race_quad);
        }
        LogFrame(log_fresh_, count);
        // Every frame ends with its own cycle's predicted display time: SteamVR matches a frame to
        // its refresh by that time. The layer's poses say where the images were rendered from; the
        // compositor reprojects them to the head pose at display. A pipelined set is located for
        // the next cycle's time (this cycle's prediction + one period, the same within SteamVR's
        // +-0.02 ms jitter); a late set is shown a refresh after that. In the third headset session
        // lane G's rule (end with the poses' locate time) gave every late set (24 % of the race)
        // a time one refresh older than its cycle's; SteamVR's mispresented frames went from 116
        // to 1284 and reused app frames from 0.2 to 4.4 %. KARTFRAME_END_TIME=locate restores it
        // (the mock's end-time check counts each such frame as an error).
        const bool end_at_locate = EndAtLocateSetting();
        // Display times stay increasing from one xrEndFrame to the next: a layer shown again (or a
        // late set whose time an earlier frame already used) goes out at this cycle's time.
        if (end_at_locate && retained_locate_time_ != 0 && retained_locate_time_ > last_end_display_time_ &&
            retained_locate_time_ != active_frame_.predicted_display_time) {
            OpenXRFrame located = active_frame_;
            located.predicted_display_time = retained_locate_time_;
            last_end_display_time_ = retained_locate_time_;
            DrainBeforeEnd();
            const bool ok = runtime_->EndFrame(located, layers, count);
            NoteEndAfterWake();
            return ok;
        }
        last_end_display_time_ = std::max(last_end_display_time_, active_frame_.predicted_display_time);
        DrainBeforeEnd();
        const bool ok = runtime_->EndFrame(active_frame_, layers, count);
        NoteEndAfterWake();
        return ok;
    }

    // The display time EndFrameWithPanel will end the open cycle with (KARTFRAME_END_TIME=locate: the
    // retained layer's locate time when that is still new).
    XrTime EndDisplayTime() const {
        if (EndAtLocateSetting() && retained_locate_time_ != 0 && retained_locate_time_ > last_end_display_time_ &&
            retained_locate_time_ != active_frame_.predicted_display_time)
            return retained_locate_time_;
        return active_frame_.predicted_display_time;
    }

    // Lane R: GalaxyFrame's pose check (notePoses in its xr_app.cpp). Lane O made GalaxyFrame's last
    // OpenXR call before xrEndFrame (after the release) part of KartFrame too: one xrLocateViews for the
    // frame's own display time, in the layer's space. Its result is now used: per eye, how far the poses
    // the shown images were rendered from (the set's own locate, made up to two refreshes plus SteamVR's
    // lead earlier) are from that fresh locate, and with KARTFRAME_SUBMIT_POSE=fresh the race's
    // projection layer is submitted with it. Every frame with located views (race and menus) is checked;
    // the 5 s summary counts frames that show a new set (as GalaxyFrame: each set once, when it goes out),
    // the XRF line carries every frame's angles. KARTFRAME_END_LOCATE=0 leaves the locate out (except for
    // KARTFRAME_SUBMIT_POSE=fresh frames, which need it).
    void CheckPoses(const OpenXRBackendFrame& frame, bool fresh) {
        fresh_views_valid_ = false;
        log_angle_[0] = log_angle_[1] = -1.0;
        const bool race = frame.presentation.mode != OpenXRWindowsVulkanFrameMode::VirtualScreen;
        const bool need_fresh = race && fresh && SubmitFreshPoseSetting();
        if ((!EndLocateSetting() && !need_fresh) || runtime_ == nullptr || runtime_->Session() == XR_NULL_HANDLE ||
            runtime_->AppSpace() == XR_NULL_HANDLE || !frame.xr_frame.views_valid)
            return;
        // Through the runtime's dispatch (the backend's replay test links no OpenXR loader).
        if (locate_views_ == nullptr && !runtime_->LoadFunction("xrLocateViews", &locate_views_)) return;
        const XrTime end_time = EndDisplayTime();
        XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
        info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        info.displayTime = end_time;
        info.space = runtime_->AppSpace();
        XrViewState state{XR_TYPE_VIEW_STATE};
        for (auto& view : fresh_views_) view = {XR_TYPE_VIEW};
        uint32_t count = 0;
        const auto t0 = std::chrono::steady_clock::now();
        const XrResult result = locate_views_(runtime_->Session(), &info, &state, kOpenXREyeCount, &count, fresh_views_.data());
        const double locate_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ObserveResult(result);
        constexpr XrViewStateFlags kValid = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
        if (XR_FAILED(result) || count < kOpenXREyeCount || (state.viewStateFlags & kValid) != kValid) return;
        fresh_views_valid_ = true;
        auto& pc = pose_check_;
        const auto now = std::chrono::steady_clock::now();
        if (pc.sets > 0 && (pc.race != race || now - pc.since >= std::chrono::seconds(5))) LogPoseCheck(now);
        if (pc.sets == 0) {
            pc = {};
            pc.race = race;
            pc.since = now;
        }
        const XrTime set_time = retained_locate_time_ != 0 ? retained_locate_time_ : frame.xr_frame.predicted_display_time;
        const double dt = std::fabs(static_cast<double>(set_time - end_time)) / 1e6;
        for (uint32_t e = 0; e < kOpenXREyeCount; ++e) {
            const XrPosef& sub = frame.xr_frame.views[e].pose;
            const XrPosef& now_pose = fresh_views_[e].pose;
            const double dx = sub.position.x - now_pose.position.x, dy = sub.position.y - now_pose.position.y,
                         dz = sub.position.z - now_pose.position.z;
            const double d = std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.0;
            const XrQuaternionf& r = sub.orientation;
            const XrQuaternionf& q = now_pose.orientation;
            // The angle between them from the chord (|r - s| = 2 sin(angle / 4)), exact for small angles
            // where acos of the dot product is not.
            const double sg = r.x * q.x + r.y * q.y + r.z * q.z + r.w * q.w < 0.0 ? -1.0 : 1.0;
            const double cx = r.x - sg * q.x, cy = r.y - sg * q.y, cz = r.z - sg * q.z, cw = r.w - sg * q.w;
            const double ang = 4.0 * std::asin(std::fmin(1.0, 0.5 * std::sqrt(cx * cx + cy * cy + cz * cz + cw * cw))) * 57.29578;
            log_angle_[e] = ang;
            if (!fresh) continue;
            pc.pos_sum[e] += d;
            pc.pos_max[e] = std::fmax(pc.pos_max[e], d);
            pc.ang_sum[e] += ang;
            pc.ang_max[e] = std::fmax(pc.ang_max[e], ang);
        }
        if (!fresh) return;
        ++pc.sets;
        pc.on_time += dt < 0.5;
        pc.dt_sum += dt;
        pc.dt_max = std::fmax(pc.dt_max, dt);
        const XrVector3f& l = frame.xr_frame.views[0].pose.position;
        const XrVector3f& rp = frame.xr_frame.views[1].pose.position;
        const double ipd = std::sqrt((l.x - rp.x) * (l.x - rp.x) + (l.y - rp.y) * (l.y - rp.y) + (l.z - rp.z) * (l.z - rp.z)) * 1000.0;
        pc.ipd_min = std::fmin(pc.ipd_min, ipd);
        pc.ipd_max = std::fmax(pc.ipd_max, ipd);
        pc.locate_sum += locate_ms;
        pc.locate_max = std::fmax(pc.locate_max, locate_ms);
    }

    // GalaxyFrame's words, then the scene (race: eye sets; menus: the environment's poses) and the
    // check's own locate call.
    void LogPoseCheck(std::chrono::steady_clock::time_point now) {
        auto& pc = pose_check_;
        const int k = pc.sets;
        char line[640];
        std::snprintf(line, sizeof(line),
                      "vr: views of %d sets: %d for the display time they went out at (off %.2f ms avg, %.2f max); against a locate then: "
                      "left eye %.2f mm %.3f deg avg, %.2f mm %.3f deg max; right eye %.2f mm %.3f deg avg, %.2f mm %.3f deg max; "
                      "eye distance %.2f-%.2f mm; %s, %.1f s; that locate %.3f ms avg %.3f max",
                      k, pc.on_time, pc.dt_sum / k, pc.dt_max, pc.pos_sum[0] / k, pc.ang_sum[0] / k, pc.pos_max[0], pc.ang_max[0],
                      pc.pos_sum[1] / k, pc.ang_sum[1] / k, pc.pos_max[1], pc.ang_max[1], pc.ipd_min, pc.ipd_max,
                      pc.race ? "race" : "menus", std::chrono::duration<double>(now - pc.since).count(), pc.locate_sum / k,
                      pc.locate_max);
        Log(OpenXRLogLevel::Info, line);
        pc.sets = 0;
    }

    void NoteEndAfterWake() {
        if (wake_ == std::chrono::steady_clock::time_point{}) return;
        pacing::OnEndAfterWake(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wake_).count());
    }

    // ---- Sets rendered ahead: the slots (lane K) ----
    //
    // eye_swapchains_, env_swapchain_ and panel_swapchain_ are the slot PrepareTargets writes. With
    // sets rendered ahead they are loaded from the free pools before a set (LoadAheadSlot), travel
    // with the set while it is in flight (AheadSlot), come back for its release (RestoreAheadSlot),
    // and what is left in them afterwards (the pair FinishFrame swapped off the display, or the
    // dropped set's) returns to the pools (ParkAheadSlot). The pools are first in, first out: a pair
    // is written again as long after it left the display as the pair count allows.
    const AheadSlot* FindAheadSlot(uint64_t serial) const {
        for (const auto& slot : ahead_slots_)
            if (slot.serial == serial) return &slot;
        return nullptr;
    }

    // Call with submission_mutex_ held.
    void ForgetExpected(uint64_t serial) {
        ahead_expected_.erase(std::remove(ahead_expected_.begin(), ahead_expected_.end(), serial),
                              ahead_expected_.end());
    }

    bool LoadAheadSlot() {
        if (eye_swapchains_[0].handle == XR_NULL_HANDLE) {
            if (free_pairs_.empty()) return false;
            eye_swapchains_ = std::move(free_pairs_.front());
            free_pairs_.pop_front();
        }
        if (env_swapchains_ready_ && env_swapchain_.handle == XR_NULL_HANDLE) {
            if (!free_envs_.empty()) {
                env_swapchain_ = std::move(free_envs_.front());
                free_envs_.pop_front();
            } else if (!CreateEnvSwapchain(env_swapchain_)) {
                env_swapchain_ = {};
                return false;
            }
        }
        if (panel_swapchains_ready_ && panel_swapchain_.handle == XR_NULL_HANDLE) {
            if (!free_panels_.empty()) {
                panel_swapchain_ = std::move(free_panels_.front());
                free_panels_.pop_front();
            } else if (!CreateSwapchain(panel_swapchain_, kOpenXRPanelLayerWidth, kOpenXRPanelLayerHeight,
                                        "settings panel")) {
                panel_swapchain_ = {};
                return false;
            }
        }
        return true;
    }

    void ParkAheadSlot() {
        if (eye_swapchains_[0].handle != XR_NULL_HANDLE) free_pairs_.push_back(std::exchange(eye_swapchains_, EyePair{}));
        if (env_swapchain_.handle != XR_NULL_HANDLE) free_envs_.push_back(std::exchange(env_swapchain_, EyeSwapchain{}));
        if (panel_swapchain_.handle != XR_NULL_HANDLE) free_panels_.push_back(std::exchange(panel_swapchain_, EyeSwapchain{}));
    }

    bool RestoreAheadSlot(uint64_t serial) {
        const auto it = std::find_if(ahead_slots_.begin(), ahead_slots_.end(),
                                     [&](const AheadSlot& slot) { return slot.serial == serial; });
        if (it == ahead_slots_.end()) return false;
        ParkAheadSlot();  // a pair loaded for a set that never started
        eye_swapchains_ = std::move(it->eyes);
        env_swapchain_ = std::move(it->env);
        panel_swapchain_ = std::move(it->panel);
        render_session_serial_ = it->session_serial;
        render_space_serial_ = it->space_serial;
        ahead_slots_.erase(it);
        return true;
    }

    // Images that may never be released (an unconfirmed GPU submission): kept out of the pools until
    // the session's swapchains are destroyed.
    void AbandonCurrentSlot() {
        if (eye_swapchains_[0].handle != XR_NULL_HANDLE) abandoned_pairs_.push_back(std::exchange(eye_swapchains_, EyePair{}));
        if (env_swapchain_.handle != XR_NULL_HANDLE) abandoned_pairs_.push_back({std::exchange(env_swapchain_, EyeSwapchain{}), EyeSwapchain{}});
        if (panel_swapchain_.handle != XR_NULL_HANDLE) abandoned_pairs_.push_back({std::exchange(panel_swapchain_, EyeSwapchain{}), EyeSwapchain{}});
    }

    // KARTFRAME_EYE_PAIRS with sets rendered ahead: 3 by default (the pair on display and two sets in
    // flight), at least 3; more lets each pair rest longer after it left the display. Lane R: 4 by default
    // with KARTFRAME_AHEAD_REFRESHES=1, whose next set starts before the frame is ended (the pair on
    // display, the set shown now, one it overtook, the new one).
    static uint32_t AheadPairsSetting() {
        const char* env = std::getenv("KARTFRAME_EYE_PAIRS");
        const char* lead = std::getenv("KARTFRAME_AHEAD_REFRESHES");
        // KARTFRAME_END_DELAY_US and KARTFRAME_END_PHASE (lane X) (test only) start the next set before the
        // delayed end, like lead 1: 4 pairs.
        const char* end_delay = std::getenv("KARTFRAME_END_DELAY_US");
        const char* end_phase = std::getenv("KARTFRAME_END_PHASE");
        const long fallback = (lead != nullptr && std::string_view(lead) == "1") ||
                                      (end_delay != nullptr && std::strtol(end_delay, nullptr, 10) > 0) ||
                                      (end_phase != nullptr && *end_phase != '\0')
                                  ? 4
                                  : 3;
        const long v = env != nullptr && *env != '\0' ? std::strtol(env, nullptr, 10) : fallback;
        return static_cast<uint32_t>(std::clamp(v, 3l, 8l));
    }

    // ---- Eye swapchain pairs (lane J) ----
    //
    // The panel recording of the eighth headset session showed, in races, one eye at a time displaying
    // a whole image it had shown 3 to 5 refreshes earlier (10 % of the left eye's refreshes, 7 % of the
    // right's), the race timer on the virtual screen stepping back and forth between game frames, and
    // menus (frames ended right at the wake-up) clean. With two pairs the pair a frame showed is
    // acquired and written again one refresh later, while SteamVR, which predicts a frame's display 3
    // refreshes after its wake-up, may still compose that frame: it evidently resolves each eye's image
    // when it composes, not when the frame was ended (frame/perf/PANELS.md; the mock's compositor
    // model reproduces the pattern with two pairs). So a pair is written again only
    // KARTFRAME_EYE_PAIRS - 1 refreshes after the last frame that showed it (default 6 pairs: 5
    // refreshes, beyond SteamVR's lead plus the refresh a frame stays on show); =2 restores the old
    // alternation. The pairs between the retained one and the one written cool off in cooling_pairs_.
    void RotateEyePairs() {
        if (cooling_pairs_.empty()) {
            std::swap(eye_swapchains_, retained_swapchains_);
            return;
        }
        cooling_pairs_.push_back(std::move(retained_swapchains_));
        cooling_since_.push_back(retained_ref_cycle_);
        retained_swapchains_ = std::move(eye_swapchains_);
        eye_swapchains_ = {};
        eye_pair_ready_ = false;
        TakeCooledPair();
    }

    // Moves the pair that cooled longest into eye_swapchains_ when it has cooled long enough.
    bool TakeCooledPair() {
        if (eye_pair_ready_) return true;
        if (cooling_pairs_.empty()) return false;
        size_t best = 0;
        for (size_t i = 1; i < cooling_since_.size(); ++i)
            if (cooling_since_[i] < cooling_since_[best]) best = i;
        // 0: never shown (made after the first frames).
        const uint64_t hold = eye_pairs_wanted_ - 1;
        if (cooling_since_[best] != 0 && cycle_count_ < cooling_since_[best] + hold) return false;
        eye_swapchains_ = std::move(cooling_pairs_[best]);
        cooling_pairs_.erase(cooling_pairs_.begin() + static_cast<std::ptrdiff_t>(best));
        cooling_since_.erase(cooling_since_.begin() + static_cast<std::ptrdiff_t>(best));
        eye_pair_ready_ = true;
        return true;
    }

    static uint32_t EyePairsSetting() {
        static const uint32_t pairs = [] {
            const char* env = std::getenv("KARTFRAME_EYE_PAIRS");
            // Default 2 again (lane K): six pairs were worse on the headset (seventh session, H7.md).
            const long v = env != nullptr && *env != '\0' ? std::strtol(env, nullptr, 10) : 2;
            return static_cast<uint32_t>(std::clamp(v, 2l, 8l));
        }();
        return pairs;
    }

    // KARTFRAME_FRAME_LOG=1: one line per xrEndFrame with a layer (a test-session log: about 50 MB an hour
    // at 120 Hz). XRF c=<cycle> w=<wake-up, ms since the first> e=<xrEndFrame after the wake-up, ms, at the
    // call> f=<1 a newly rendered set, 0 the retained layer again> p=<eye pair> i=<released image index
    // left/right> d=<display time step from the last frame, refreshes> l=<display time minus the poses'
    // locate time, refreshes> n=<layers submitted> (lane Q) aL=/aR=<degrees between each eye's rendered pose and
    // a locate for the frame's display time, -1 when not checked> (lane R). The panel recording (KARTFRAME_RECORD=raw) lines up with it frame by frame.
    // Lane W (absolute times, to line up with SteamVR's compositor frames in vrframes.log, frame/tools/phase.py):
    // m=<wake-up, CLOCK_MONOTONIC ms (steady_clock is CLOCK_MONOTONIC on Linux); the end call is m + e>
    // r=<eye images released, ms after the wake-up, -1 none released for this end> t=<the frame's predicted display
    // time, CLOCK_MONOTONIC ms (XR_KHR_convert_timespec_time when enabled, else XrTime read as nanoseconds)>.
    void LogFrame(bool fresh, uint32_t layer_count) {
        static const bool enabled = [] {
            const char* env = std::getenv("KARTFRAME_FRAME_LOG");
            return env != nullptr && env[0] == '1';
        }();
        if (!enabled) return;
        const auto now = std::chrono::steady_clock::now();
        if (log_epoch_ == std::chrono::steady_clock::time_point{}) log_epoch_ = wake_;
        const double period = active_frame_.predicted_display_period > 0
                                  ? static_cast<double>(active_frame_.predicted_display_period) : 8'333'333.0;
        const double step = log_last_display_ ? static_cast<double>(active_frame_.predicted_display_time - log_last_display_) / period : 0.0;
        log_last_display_ = active_frame_.predicted_display_time;
        const double located = retained_locate_time_ ? static_cast<double>(active_frame_.predicted_display_time - retained_locate_time_) / period : 0.0;
        const double released = release_at_ != std::chrono::steady_clock::time_point{}
                                    ? std::chrono::duration<double, std::milli>(release_at_ - wake_).count() : -1.0;
        release_at_ = {};
        char line[384];
        std::snprintf(line, sizeof(line), "XRF c=%llu w=%.3f e=%.2f f=%d p=%u i=%d/%d d=%.2f l=%.2f n=%u aL=%.3f aR=%.3f m=%.3f r=%.3f t=%.3f",
                      static_cast<unsigned long long>(cycle_count_),
                      std::chrono::duration<double, std::milli>(wake_ - log_epoch_).count(),
                      std::chrono::duration<double, std::milli>(now - wake_).count(), fresh ? 1 : 0,
                      retained_swapchains_[0].pair_id, retained_swapchains_[0].released_index,
                      retained_swapchains_[retained_swapchains_[1].handle == XR_NULL_HANDLE ? 0 : 1].released_index, step, located, layer_count, log_angle_[0], log_angle_[1],
                      std::chrono::duration<double, std::milli>(wake_.time_since_epoch()).count(), released,
                      MonotonicMs(active_frame_.predicted_display_time));
        Log(OpenXRLogLevel::Info, line);
    }

    // Lane W: an XrTime as CLOCK_MONOTONIC ms (XRF t=). XR_KHR_convert_timespec_time when the session enabled it
    // (SteamVR on the Frame does; the mock too, with MOCK_XR_TIME_OFFSET_S), else the XrTime read as nanoseconds.
    // The first call logs which, and XrTime minus CLOCK_MONOTONIC.
    double MonotonicMs(XrTime time) {
        using ToTimespec = XrResult(XRAPI_PTR*)(XrInstance, XrTime, struct timespec*);
        if (!clock_looked_) {
            clock_looked_ = true;
            const auto& ext = runtime_->EnabledExtensions();
            if (std::find(ext.begin(), ext.end(), "XR_KHR_convert_timespec_time") != ext.end()) {
                ToTimespec fn = nullptr;
                if (runtime_->LoadFunction("xrConvertTimeToTimespecTimeKHR", &fn)) to_timespec_ = reinterpret_cast<void*>(fn);
            }
        }
        timespec spec{};
        const auto convert = reinterpret_cast<ToTimespec>(to_timespec_);
        const bool ok = convert != nullptr && XR_SUCCEEDED(convert(runtime_->Instance(), time, &spec));
        const double ms = ok ? static_cast<double>(spec.tv_sec) * 1e3 + static_cast<double>(spec.tv_nsec) * 1e-6
                             : static_cast<double>(time) * 1e-6;
        if (!clock_logged_) {
            clock_logged_ = true;
            char line[192];
            std::snprintf(line, sizeof(line), "XRF clock: t= %s; XrTime - CLOCK_MONOTONIC = %.3f ms",
                          ok ? "from xrConvertTimeToTimespecTimeKHR" : "XrTime read as CLOCK_MONOTONIC ns (no conversion)",
                          static_cast<double>(time) * 1e-6 - ms);
            Log(OpenXRLogLevel::Info, line);
        }
        return ms;
    }

    bool Shutdown() {
        if (shutdown_unsafe_) {
            return false;
        }
        {
            std::lock_guard lock(submission_mutex_);
            shutting_down_ = true;
        }
        submission_cv_.notify_all();

        bool bridge_drained = true;
        if (bridge_enabled_) {
            bridge_drained = aurora_vulkan_win32_disable();
            bridge_enabled_ = false;
        }
        // The bridge's drain waited for the queue to go idle; the watcher can go.
        if (bridge_drained) {
            gpu_.Stop();
            cmds_.Stop();
        }
        if (!bridge_drained) {
            AbandonAcquiredSwapchains();
            shutdown_unsafe_ = true;
            Fail("Vulkan queue completion is unknown; retaining the OpenXR session and graphics owners");
            return false;
        }

        // A queue-tail fence proved that no bridge command can still reference
        // an acquired image. This also makes a conservatively abandoned image
        // releasable after an earlier submission failure.
        AllowAcquiredSwapchainsAfterGpuDrain();
        ReleaseAcquiredSwapchains();
        if (frame_active_ && runtime_ != nullptr) {
            // Shutdown is required to run on the XR owner thread after Aurora's
            // worker is idle, so it is safe to close an abandoned frame here.
            if (runtime_->IsSessionRunning()) {
                runtime_->EndFrameWithoutLayers(active_frame_);
            }
            frame_active_ = false;
            active_frame_serial_ = 0;
            active_frame_ = {};
        }
        DestroySwapchains();
        pending_packet_serial_ = 0;
        last_display_period_ = 0;
        if (owns_session_ && runtime_ != nullptr) {
            runtime_->DestroySession();
            owns_session_ = false;
        }
        if (runtime_) runtime_->SetGraphicsQueueGuard(nullptr, nullptr);
        if (hooks_installed_) { aurora_vulkan_win32_configure(nullptr); hooks_installed_ = false; }
        vk_instance_ = VK_NULL_HANDLE;
        instance_api_version_ = 0;
        timeline_semaphores_ = false;
        bound_ = false;
        requirements_queried_ = false;
        runtime_ = nullptr;
        return true;
    }

    bool IsBound() const { return bound_; }
    bool PanelLayerAvailable() const { return !panel_layer_failed_; }
    bool MenuEnvironmentAvailable() const {
        static const bool off = std::string_view(MenuEnvironmentSetting()) == "0";
        return !off && !env_layer_failed_;
    }
    const OpenXRWindowsVulkanGraphicsRequirements& GraphicsRequirements() const { return requirements_; }
    int64_t SwapchainFormat() const { return static_cast<int64_t>(swapchain_format_); }
    const std::string& LastError() const { return last_error_; }

private:
    bool SelectSwapchainFormat() {
        if (!SelectAuroraSwapchainFormat()) return false;
        const auto& formats = runtime_->SwapchainFormats();
        eye_format_ = swapchain_format_;
        if (RgbaEyesSetting() && SrgbSinkSetting() && IsWindowsVkSrgbFormat(swapchain_format_) &&
            std::find(formats.begin(), formats.end(), static_cast<int64_t>(VK_FORMAT_R8G8B8A8_SRGB)) != formats.end()) {
            eye_format_ = VK_FORMAT_R8G8B8A8_SRGB;
        }
        return true;
    }

    bool SelectAuroraSwapchainFormat() {
        const auto& formats = runtime_->SwapchainFormats();
        // Aurora's UNORM target contains the gamma-encoded bytes expected by
        // the desktop compositor. OpenXR must declare the compatible sRGB
        // sibling so the headset compositor decodes those raw bytes instead
        // of treating them as linear light (which appears severely washed out).
        const VkFormat srgb = WindowsVkSrgbSibling(aurora_format_);
        if (srgb != VK_FORMAT_UNDEFINED &&
            std::find(formats.begin(), formats.end(), static_cast<int64_t>(srgb)) !=
                formats.end()) {
            swapchain_format_ = srgb;
            return true;
        }
        const auto exact = std::find(formats.begin(), formats.end(), static_cast<int64_t>(aurora_format_));
        if (exact != formats.end()) {
            swapchain_format_ = aurora_format_;
            return true;
        }
        const auto compatible = std::find_if(formats.begin(), formats.end(), [&](int64_t format) {
            return SameVkCopyFamily(aurora_format_, static_cast<VkFormat>(format));
        });
        if (compatible == formats.end()) {
            return Fail("OpenXR offered no swapchain format copy-compatible with Aurora's Vulkan color format");
        }
        swapchain_format_ = static_cast<VkFormat>(*compatible);
        return true;
    }

    bool CreateSwapchains() {
        eye_pairs_wanted_ = ahead_ ? AheadPairsSetting() : EyePairsSetting();
        cooling_pairs_.clear();
        cooling_since_.clear();
        eye_pair_ready_ = true;
        extra_pairs_made_ = false;
        return CreateSwapchainPair(eye_swapchains_, 0) && CreateSwapchainPair(retained_swapchains_, 1);
    }

    // The pairs beyond the first two, made before the first set is rendered (the session runs by then).
    void MakeExtraEyePairs() {
        extra_pairs_made_ = true;
        for (uint32_t pair = 2; pair < eye_pairs_wanted_; ++pair) {
            EyePair extra{};
            if (!CreateSwapchainPair(extra, pair)) {
                Log(OpenXRLogLevel::Warning, "OpenXR could not make another eye swapchain pair; using fewer");
                break;
            }
            if (ahead_) {
                free_pairs_.push_back(std::move(extra));
                continue;
            }
            cooling_pairs_.push_back(std::move(extra));
            cooling_since_.push_back(0);
        }
        eye_pairs_wanted_ = static_cast<uint32_t>(ahead_ ? free_pairs_.size() : cooling_pairs_.size()) + 2;
        std::ostringstream message;
        message << "OpenXR eye swapchain pairs: " << eye_pairs_wanted_
                << (ahead_ ? std::string(" (sets rendered ahead: one pair on display, up to ") +
                                 std::to_string(std::min(eye_pairs_wanted_ - 1, 2u)) +
                                 " sets in flight; KARTFRAME_EYE_PAIRS)"
                    : eye_pairs_wanted_ > 2 ? " (a pair is written again " + std::to_string(eye_pairs_wanted_ - 1) +
                                                " refreshes after the last frame that showed it; KARTFRAME_EYE_PAIRS)"
                                          : std::string(" (alternating; KARTFRAME_EYE_PAIRS)"));
        Log(OpenXRLogLevel::Info, message.str());
    }

    bool CreateSwapchainPair(std::array<EyeSwapchain, kOpenXREyeCount>& pair, uint32_t id) {
        if (StereoPackedSetting()) {
            // One image for both eyes, side by side (pair[1] stays empty: that marks a packed pair).
            const auto& left = runtime_->ViewConfiguration()[0];
            for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
                const auto& view = runtime_->ViewConfiguration()[eye];
                if (view.render_width != left.render_width || view.render_height != left.render_height)
                    return Fail("KARTFRAME_STEREO_PACKED needs both eyes the same size");
                eye_width_[eye] = view.render_width;
                eye_height_[eye] = view.render_height;
            }
            pair[0].pair_id = id;
            pair[1] = {};
            pair[1].pair_id = id;
            return CreateSwapchain(pair[0], left.render_width * kOpenXREyeCount, left.render_height, "packed eyes", true);
        }
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            pair[eye].pair_id = id;
            const auto& view = runtime_->ViewConfiguration()[eye];
            eye_width_[eye] = view.render_width;
            eye_height_[eye] = view.render_height;
            if (!CreateSwapchain(pair[eye], view.render_width, view.render_height,
                                 eye == 0 ? "left eye" : "right eye", true)) {
                return false;
            }
        }
        return true;
    }

    // The eyes' usage flags (lane O): see GalaxyUsageSetting.
    XrSwapchainUsageFlags EyeUsage() const {
        if (!GalaxyUsageSetting()) return XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        XrSwapchainUsageFlags usage = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        if (eye_format_ != VK_FORMAT_R8G8B8A8_SRGB || FrameStampSetting() || !SrgbSinkSetting())
            usage |= XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        return usage;
    }

    // eye: one of the eye pairs (the menus' quad uses the left image of a pair as well); the menu
    // environment and the settings panel keep Aurora's format and the old usage (they take copies).
    bool CreateSwapchain(EyeSwapchain& swapchain, uint32_t width, uint32_t height, const char* what, bool eye = false,
                         XrSwapchainUsageFlags usage = 0) {
        swapchain.width = width;
        swapchain.height = height;
        const VkFormat format = eye ? eye_format_ : swapchain_format_;

        XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        create.usageFlags = eye ? EyeUsage() : XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        // Aurora writes the eyes through an sRGB view (decoding them in its shader; KARTFRAME_SINK, default
        // srgb), so the images need no MUTABLE_FORMAT, which lets the driver compress them (lane I: the
        // eyes' way into the images 0.47 ms a set instead of 2.3). KARTFRAME_SINK=copy|blit: raw copies
        // through a UNORM view, which need the flag.
        if (usage != 0) {
            create.usageFlags = usage;
        } else if (IsWindowsVkSrgbFormat(format) && !SrgbSinkSetting()) {
            // Permit a UNORM view for raw copies into the sRGB XR image.
            create.usageFlags |= XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
        }
        create.format = static_cast<int64_t>(format);
        create.sampleCount = 1;
        create.width = swapchain.width;
        create.height = swapchain.height;
        create.faceCount = 1;
        create.arraySize = 1;
        create.mipCount = 1;
        XrResult result = xrCreateSwapchain(runtime_->Session(), &create, &swapchain.handle);
        ObserveResult(result);
        if (XR_FAILED(result)) {
            std::ostringstream message;
            message << "xrCreateSwapchain failed for the Vulkan " << what << " swapchain (" << result << ')';
            return Fail(message.str());
        }

        uint32_t count = 0;
        result = xrEnumerateSwapchainImages(swapchain.handle, 0, &count, nullptr);
        ObserveResult(result);
        if (XR_FAILED(result) || count == 0) {
            return Fail("OpenXR returned no Vulkan swapchain images");
        }
        swapchain.images.resize(count);
        for (auto& image : swapchain.images) {
            image = {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR};
        }
        result = xrEnumerateSwapchainImages(
            swapchain.handle, count, &count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchain.images.data()));
        ObserveResult(result);
        if (XR_FAILED(result)) {
            return Fail("xrEnumerateSwapchainImages failed for a Vulkan eye swapchain");
        }
        return true;
    }

    bool AcquireSwapchain(EyeSwapchain& swapchain) {
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrResult result;
        {
            const auto queue_guard = runtime_->LockGraphicsQueue();
            result = xrAcquireSwapchainImage(swapchain.handle, &acquire, &swapchain.acquired_index);
        }
        ObserveResult(result);
        if (XR_FAILED(result)) {
            return Fail("xrAcquireSwapchainImage failed for a Vulkan eye swapchain");
        }
        swapchain.acquired = true;
        swapchain.waited = false;
        swapchain.release_forbidden = false;
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = XR_INFINITE_DURATION;
        // The runtime cannot access VkQueue here; let Dawn keep submitting
        // while the compositor waits for this image to become available.
        result = xrWaitSwapchainImage(swapchain.handle, &wait);
        ObserveResult(result);
        if (result == XR_TIMEOUT_EXPIRED) {
            return Fail("xrWaitSwapchainImage unexpectedly timed out for a Vulkan eye swapchain");
        }
        if (XR_FAILED(result)) {
            return Fail("xrWaitSwapchainImage failed for a Vulkan eye swapchain");
        }
        swapchain.waited = true;
        if (swapchain.acquired_index >= swapchain.images.size()) {
            return Fail("OpenXR returned an out-of-range Vulkan swapchain image index");
        }
        return true;
    }

    // Lane U: KARTFRAME_RELEASE=idle drains Dawn's queue (vkQueueWaitIdle, queue guard held) right before
    // the releases, so SteamVR's client submission inside xrReleaseSwapchainImage never waits behind our
    // next set's GPU work (the probe, clean on the headset, had nothing queued there). Default: the set's
    // own GPU-complete wait only. Logs the drain's cost every 10 s.
    static bool ReleaseIdleSetting() {
        static const bool idle = [] {
            const char* env = std::getenv("KARTFRAME_RELEASE");
            return env != nullptr && std::string_view(env) == "idle";
        }();
        return idle;
    }
    struct ReleaseDrainStats {
        int64_t since_ns = 0;
        uint32_t count[2] = {}, over1[2] = {}, over4[2] = {};
        double sum[2] = {}, max[2] = {};
    } release_drain_;

    // which: 0 before the releases, 1 before xrEndFrame.
    void NoteReleaseDrain(double ms, int which) {
        if (ms < 0.0) return;
        auto& d = release_drain_;
        const int64_t now = diagnostics::detail::NowNs();
        if (d.since_ns == 0) d.since_ns = now;
        ++d.count[which];
        d.sum[which] += ms;
        d.max[which] = std::max(d.max[which], ms);
        d.over1[which] += ms > 1.0;
        d.over4[which] += ms > 4.0;
        if (now - d.since_ns < 10'000'000'000ll) return;
        char line[256];
        std::snprintf(line, sizeof(line),
                      "release idle drain (KARTFRAME_RELEASE=idle): before release %u, mean %.2f ms, max %.2f, >1 ms %u, >4 ms %u; "
                      "before end %u, mean %.2f ms, max %.2f, >1 ms %u",
                      d.count[0], d.count[0] ? d.sum[0] / d.count[0] : 0.0, d.max[0], d.over1[0], d.over4[0], d.count[1],
                      d.count[1] ? d.sum[1] / d.count[1] : 0.0, d.max[1], d.over1[1]);
        Log(OpenXRLogLevel::Info, line);
        d = {};
        d.since_ns = now;
    }

    // KARTFRAME_RELEASE=idle: the same drain before xrEndFrame of a frame that released eye images (the frame
    // worker may have submitted the next set since the releases; SteamVR's client submits in xrEndFrame too).
    void DrainBeforeEnd() {
        if (!ReleaseIdleSetting() || runtime_ == nullptr || !released_eyes_) return;
        released_eyes_ = false;
        const auto queue_guard = runtime_->LockGraphicsQueue();
        if (queue_guard.token) NoteReleaseDrain(gpu_.DrainHeldMs(), 1);
    }
    bool released_eyes_ = false;

    bool ReleaseAcquiredSwapchains() {
        const auto queue_guard = runtime_ ? runtime_->LockGraphicsQueue() : OpenXRRuntime::GraphicsQueueGuard{nullptr, nullptr};
        if (ReleaseIdleSetting() && queue_guard.token &&
            (eye_swapchains_[0].acquired || eye_swapchains_[1].acquired)) {
            NoteReleaseDrain(gpu_.DrainHeldMs(), 0);
            released_eyes_ = true;
        }
        bool success = true;
        for (auto& swapchain : eye_swapchains_) {
            success = ReleaseSwapchain(swapchain) && success;
        }
        success = ReleaseSwapchain(env_swapchain_) && success;
        return ReleaseSwapchain(panel_swapchain_) && success;
    }

    bool ReleaseSwapchain(EyeSwapchain& swapchain) {
        if (!swapchain.acquired || swapchain.handle == XR_NULL_HANDLE) {
            return true;
        }
        if (!swapchain.waited) {
            // OpenXR only permits release after a successful wait. Keep the
            // image acquired and let session teardown destroy the child.
            Log(OpenXRLogLevel::Warning, "cannot release an OpenXR Vulkan image whose wait did not complete");
            return false;
        }
        if (swapchain.release_forbidden) {
            // Aurora reported a failed submission after it may already have
            // queued GPU work. Without a trustworthy fence the release could race
            // that work, so leave the image acquired for xrDestroySession.
            Log(OpenXRLogLevel::Warning, "deferring an OpenXR Vulkan image after an unsafe GPU submission");
            return false;
        }
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        const XrResult result = xrReleaseSwapchainImage(swapchain.handle, &release);
        ObserveResult(result);
        if (XR_FAILED(result)) {
            return Fail("xrReleaseSwapchainImage failed for an Vulkan swapchain");
        }
        swapchain.released_index = static_cast<int32_t>(swapchain.acquired_index);
        swapchain.acquired = false;
        swapchain.waited = false;
        return true;
    }

    void AbandonAcquiredSwapchains() noexcept {
        for (auto* swapchain : {&eye_swapchains_[0], &eye_swapchains_[1], &env_swapchain_, &panel_swapchain_}) {
            if (swapchain->acquired) {
                swapchain->release_forbidden = true;
            }
        }
    }

    void AllowAcquiredSwapchainsAfterGpuDrain() noexcept {
        for (auto* swapchain : {&eye_swapchains_[0], &eye_swapchains_[1], &env_swapchain_, &panel_swapchain_}) {
            if (swapchain->acquired) {
                swapchain->release_forbidden = false;
            }
        }
    }

    // Lane Q: the race quad's swapchain (KARTFRAME_RACE_QUAD), made at the first frame whose only layer
    // would be the projection: 16x16 in the eyes' format, every image cleared to transparent black once
    // (acquired, cleared, the GPU waited for, released: the eyes' rules). Mode cycle then acquires,
    // waits and releases one image per frame without writing it. A failure is logged once; frames then
    // go out with the projection alone.
    bool EnsureRaceQuad() {
        const int mode = RaceQuadSetting();
        if (mode == 0 || race_quad_failed_ || !cmds_.Ready()) return false;
        if (race_quad_ready_) return mode == 1 || CycleRaceQuad();
        const XrSwapchainUsageFlags usage = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                                            XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        const auto started = std::chrono::steady_clock::now();
        bool ok = false;
        for (const uint32_t size : {16u, 64u}) {
            if (CreateSwapchain(race_quad_, size, size, "race quad", true, usage)) {
                ok = true;
                break;
            }
            race_quad_ = {};
        }
        for (size_t i = 0; ok && i < race_quad_.images.size(); ++i) {
            ok = AcquireSwapchain(race_quad_) && cmds_.ClearAndWait(race_quad_.images[race_quad_.acquired_index].image);
            if (race_quad_.acquired && race_quad_.waited) {
                const auto queue_guard = runtime_->LockGraphicsQueue();
                // An image whose clear did not finish stays acquired (released only finished).
                ok = ok && ReleaseSwapchain(race_quad_);
            }
        }
        if (!ok) {
            race_quad_failed_ = true;
            if (race_quad_.handle != XR_NULL_HANDLE && race_quad_.acquired) race_quad_.release_forbidden = true;
            else DestroyOneSwapchain(race_quad_);
            Log(OpenXRLogLevel::Warning, "OpenXR race quad could not be made; projection-only frames stay alone");
            return false;
        }
        race_quad_ready_ = true;
        std::ostringstream message;
        message << "OpenXR race quad layer ready: " << race_quad_.width << 'x' << race_quad_.height << " VkFormat "
                << static_cast<int64_t>(eye_format_) << ", " << race_quad_.images.size()
                << " images cleared to transparent in "
                << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count()
                << " ms; a 1 cm quad 2 m ahead after every projection-only layer (KARTFRAME_RACE_QUAD="
                << (mode == 2 ? "cycle" : "1") << ")";
        Log(OpenXRLogLevel::Info, message.str());
        return mode == 1 || CycleRaceQuad();
    }

    bool CycleRaceQuad() {
        if (!AcquireSwapchain(race_quad_)) {
            race_quad_failed_ = true;
            return false;
        }
        const auto queue_guard = runtime_->LockGraphicsQueue();
        if (!ReleaseSwapchain(race_quad_)) {
            race_quad_failed_ = true;
            return false;
        }
        return true;
    }

    // The settings panel's swapchain pair, made the first time the panel opens.
    // A failure is logged once and the panel is drawn into the eyes again.
    bool EnsurePanelSwapchains() {
        if (panel_swapchains_ready_) {
            return true;
        }
        if (panel_layer_failed_) {
            return false;
        }
        if (CreateSwapchain(panel_swapchain_, kOpenXRPanelLayerWidth, kOpenXRPanelLayerHeight, "settings panel") &&
            CreateSwapchain(retained_panel_swapchain_, kOpenXRPanelLayerWidth, kOpenXRPanelLayerHeight,
                            "settings panel")) {
            panel_swapchains_ready_ = true;
            Log(OpenXRLogLevel::Info, "OpenXR settings panel layer ready");
            return true;
        }
        DestroyPanelSwapchains();
        panel_layer_failed_ = true;
        Log(OpenXRLogLevel::Warning, "the settings panel could not get its own OpenXR layer; drawing it into the eyes");
        return false;
    }

    // The menus' environment swapchain pair (both eyes side by side), made the first time a menu set
    // asks for it. A failure is logged once and the menus show the screen alone.
    bool EnsureEnvSwapchains() {
        if (env_swapchains_ready_) return true;
        if (env_layer_failed_ || !MenuEnvironmentAvailable()) return false;
        const auto& view = runtime_->ViewConfiguration()[0];
        const uint32_t width = view.render_width * kOpenXREyeCount, height = view.render_height;
        if ((env_swapchain_.handle != XR_NULL_HANDLE || CreateEnvSwapchain(env_swapchain_)) &&
            CreateEnvSwapchain(retained_env_swapchain_)) {
            env_swapchains_ready_ = true;
            std::ostringstream message;
            message << "OpenXR menu environment layer ready (" << width << 'x' << height << ", "
                    << (MenuEnvironmentOver() ? "over" : "under") << " the screen)";
            Log(OpenXRLogLevel::Info, message.str());
            return true;
        }
        DestroyEnvSwapchains();
        env_layer_failed_ = true;
        Log(OpenXRLogLevel::Warning, "the menu environment layer could not be made; menus show the screen alone");
        return false;
    }

    bool CreateEnvSwapchain(EyeSwapchain& swapchain) {
        const auto& view = runtime_->ViewConfiguration()[0];
        return CreateSwapchain(swapchain, view.render_width * kOpenXREyeCount, view.render_height, "menu environment");
    }

    void DestroyEnvSwapchains() {
        for (auto& swapchain : free_envs_) DestroyOneSwapchain(swapchain);
        free_envs_.clear();
        for (auto* swapchain : {&env_swapchain_, &retained_env_swapchain_}) {
            if (swapchain->handle != XR_NULL_HANDLE && !swapchain->acquired) {
                xrDestroySwapchain(swapchain->handle);
            } else if (swapchain->acquired) {
                Log(OpenXRLogLevel::Warning,
                    "Vulkan environment swapchain still owns an acquired image; deferring its destruction to xrDestroySession");
            }
            *swapchain = {};
        }
        env_swapchains_ready_ = false;
        retained_env_valid_ = false;
    }

    static bool MenuEnvironmentOver() {
        static const bool over = std::string_view(MenuEnvironmentSetting()) == "over";
        return over;
    }

    void DestroyPanelSwapchains() {
        for (auto& swapchain : free_panels_) DestroyOneSwapchain(swapchain);
        free_panels_.clear();
        for (auto* swapchain : {&panel_swapchain_, &retained_panel_swapchain_}) {
            if (swapchain->handle != XR_NULL_HANDLE && !swapchain->acquired) {
                xrDestroySwapchain(swapchain->handle);
            } else if (swapchain->acquired) {
                Log(OpenXRLogLevel::Warning,
                    "Vulkan panel swapchain still owns an acquired image; deferring its destruction to xrDestroySession");
            }
            *swapchain = {};
        }
        panel_swapchains_ready_ = false;
        retained_panel_valid_ = false;
    }

    void DestroySwapchains() {
        DestroyOneSwapchain(race_quad_);
        race_quad_ready_ = false;
        DestroyPanelSwapchains();
        DestroyEnvSwapchains();
        DestroySwapchainPair(eye_swapchains_);
        DestroySwapchainPair(retained_swapchains_);
        for (auto& pair : cooling_pairs_) DestroySwapchainPair(pair);
        cooling_pairs_.clear();
        cooling_since_.clear();
        for (auto& slot : ahead_slots_) {
            DestroySwapchainPair(slot.eyes);
            DestroyOneSwapchain(slot.env);
            DestroyOneSwapchain(slot.panel);
        }
        ahead_slots_.clear();
        for (auto& pair : free_pairs_) DestroySwapchainPair(pair);
        free_pairs_.clear();
        for (auto& pair : abandoned_pairs_) DestroySwapchainPair(pair);
        abandoned_pairs_.clear();
        {
            std::lock_guard lock(submission_mutex_);
            ahead_expected_.clear();
            ahead_submitted_.clear();
        }
        eye_pair_ready_ = true;
        extra_pairs_made_ = false;
        have_retained_frame_ = false;
        retained_frame_ = {};
        retained_locate_time_ = 0;
        swapchain_format_ = VK_FORMAT_UNDEFINED;
        eye_format_ = VK_FORMAT_UNDEFINED;
    }

    void DestroySwapchainPair(std::array<EyeSwapchain, kOpenXREyeCount>& pair) {
        for (auto& swapchain : pair) DestroyOneSwapchain(swapchain);
    }

    void DestroyOneSwapchain(EyeSwapchain& swapchain) {
        if (swapchain.handle != XR_NULL_HANDLE && !swapchain.acquired) {
            xrDestroySwapchain(swapchain.handle);
        } else if (swapchain.acquired) {
            Log(OpenXRLogLevel::Warning,
                "Vulkan swapchain still owns an acquired image; deferring its destruction to xrDestroySession");
        }
        swapchain = {};
    }

    void EndActiveFrameWithoutLayers(const OpenXRFrame& frame) {
        if (frame_active_ && runtime_ != nullptr && runtime_->IsSessionRunning()) {
            runtime_->EndFrameWithoutLayers(frame);
        }
        frame_active_ = false;
        active_frame_serial_ = 0;
        active_frame_ = {};
    }

    void ObserveResult(XrResult result) noexcept {
        if (runtime_ != nullptr) {
            runtime_->ObserveResult(result);
        }
    }

    // Aurora's frame worker, right after the queue.Submit holding the eyes and the
    // bridge's release barrier (Aurora's queue-submit mutex held).
    static void OnAuroraSubmitted(uint64_t token, bool success, void* userdata) {
        auto* self = static_cast<Impl*>(userdata);
        if (self == nullptr) {
            return;
        }
        const int64_t now = diagnostics::detail::NowNs();
        const auto expected = [&] {
            return token == self->awaiting_token_ ||
                   std::find(self->ahead_expected_.begin(), self->ahead_expected_.end(), token) !=
                       self->ahead_expected_.end();
        };
        {
            std::lock_guard lock(self->submission_mutex_);
            if (!expected()) {
                return;
            }
        }
        uint64_t gpu_value = 0;
        if (success) {
            gpu_value = self->gpu_.Signal(now);
            // No completion marker for work already queued: never release these images.
            if (gpu_value == std::numeric_limits<uint64_t>::max()) success = false;
        }
        pacing::OnSetSubmitted(now, aurora_vulkan_win32_submitting_retained());
        {
            std::lock_guard lock(self->submission_mutex_);
            // Sets rendered ahead: every set's submission is kept until it is shown or dropped.
            if (const auto it = std::find(self->ahead_expected_.begin(), self->ahead_expected_.end(), token);
                it != self->ahead_expected_.end()) {
                self->ahead_expected_.erase(it);
                self->ahead_submitted_.push_back({token, success, gpu_value});
            }
            if (token != self->awaiting_token_) {
                self->submission_cv_.notify_all();
                return;
            }
            self->submitted_token_ = token;
            self->submission_success_ = success;
            self->submission_arrived_ = true;
            self->submission_unsafe_ = !success;
            self->submitted_gpu_value_ = gpu_value;
        }
        self->submission_cv_.notify_all();
    }

    bool Fail(std::string message) {
        last_error_ = std::move(message);
        Log(OpenXRLogLevel::Error, last_error_);
        return false;
    }

    void ClearError() { last_error_.clear(); }

    void Log(OpenXRLogLevel level, std::string_view message) const noexcept {
        if (!logger_) {
            return;
        }
        try {
            logger_(level, message);
        } catch (...) {
        }
    }

    PFN_xrCreateVulkanInstanceKHR create_instance_ = nullptr;
    PFN_xrCreateVulkanDeviceKHR create_device_ = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR get_device_ = nullptr;
    VkInstance vk_instance_ = VK_NULL_HANDLE;
    uint32_t instance_api_version_ = 0;
    bool timeline_semaphores_ = false;
    bool hooks_installed_ = false;
    OpenXRRuntime* runtime_ = nullptr;
    OpenXRLogCallback logger_;
    OpenXRWindowsVulkanGraphicsRequirements requirements_{};
    std::array<EyeSwapchain, kOpenXREyeCount> eye_swapchains_{};
    std::array<EyeSwapchain, kOpenXREyeCount> retained_swapchains_{};
    // Lane J: pairs cooling off after they were last shown, and the cycle each was last shown in.
    std::vector<EyePair> cooling_pairs_;
    std::vector<uint64_t> cooling_since_;
    uint32_t eye_pairs_wanted_ = 2;
    // Lane K: sets rendered ahead, their slots in flight, and the free pools (pacing thread).
    bool ahead_ = false;
    std::vector<AheadSlot> ahead_slots_;
    std::deque<EyePair> free_pairs_;
    std::deque<EyeSwapchain> free_envs_;
    std::deque<EyeSwapchain> free_panels_;
    std::vector<EyePair> abandoned_pairs_;
    bool eye_pair_ready_ = true;  // eye_swapchains_ holds a pair that may be written
    bool extra_pairs_made_ = false;
    uint64_t cycle_count_ = 0;
    uint64_t retained_ref_cycle_ = 0;
    std::chrono::steady_clock::time_point wake_{};
    std::chrono::steady_clock::time_point log_epoch_{};
    XrTime log_last_display_ = 0;
    std::chrono::steady_clock::time_point release_at_{};  // XRF r=: this end's eye releases began (lane W)
    void* to_timespec_ = nullptr;  // xrConvertTimeToTimespecTimeKHR (XRF t=, lane W)
    bool clock_looked_ = false;
    bool clock_logged_ = false;
    uint32_t eye_width_[kOpenXREyeCount]{};
    uint32_t eye_height_[kOpenXREyeCount]{};
    // The settings panel's layer: written like the eyes into panel_swapchain_,
    // shown from retained_panel_swapchain_ (see FinishFrame).
    EyeSwapchain env_swapchain_{};
    EyeSwapchain retained_env_swapchain_{};
    bool env_swapchains_ready_ = false;
    bool env_layer_failed_ = false;
    bool retained_env_valid_ = false;
    EyeSwapchain panel_swapchain_{};
    EyeSwapchain retained_panel_swapchain_{};
    bool panel_swapchains_ready_ = false;
    bool panel_layer_failed_ = false;
    // The retained frame rendered the panel's image into retained_panel_swapchain_.
    bool retained_panel_valid_ = false;
    OpenXRWindowsVulkanFrame retained_frame_{};
    uint64_t retained_session_serial_ = 0;
    uint64_t retained_space_serial_ = 0;
    bool have_retained_frame_ = false;
    // The display time the retained layer's poses were located for (0: unknown, the cycle's is
    // used), and the one the layer being finished brings.
    XrTime retained_locate_time_ = 0;
    XrTime next_layer_locate_time_ = 0;
    XrTime last_end_display_time_ = 0;  // the display time of the last xrEndFrame
    VkFormat aurora_format_ = VK_FORMAT_UNDEFINED;
    VkFormat swapchain_format_ = VK_FORMAT_UNDEFINED;
    VkFormat eye_format_ = VK_FORMAT_UNDEFINED;  // lane O: the eye pairs' format (SelectSwapchainFormat)
    PFN_xrLocateViews locate_views_ = nullptr;   // lane O / R: CheckPoses
    // Lane R: the pose check's fresh locate for the frame being ended, its per-eye angles for the XRF line
    // (-1: none), and the 5 s summary.
    std::array<XrView, kOpenXREyeCount> fresh_views_{};
    bool fresh_views_valid_ = false;
    double log_angle_[kOpenXREyeCount] = {-1.0, -1.0};
    struct PoseCheckWindow {
        int sets = 0, on_time = 0;
        bool race = false;
        std::chrono::steady_clock::time_point since{};
        double dt_sum = 0, dt_max = 0, ipd_min = 1e9, ipd_max = 0, locate_sum = 0, locate_max = 0;
        double pos_sum[kOpenXREyeCount] = {}, pos_max[kOpenXREyeCount] = {}, ang_sum[kOpenXREyeCount] = {},
               ang_max[kOpenXREyeCount] = {};
    } pose_check_;
    std::string last_error_;

    mutable std::mutex submission_mutex_;
    std::condition_variable submission_cv_;
    // Sets rendered ahead (lane K): serials handed to Aurora and not yet submitted, and the
    // submissions of sets still in flight (submission_mutex_).
    struct AheadSubmission {
        uint64_t token = 0;
        bool success = false;
        uint64_t gpu_value = 0;
    };
    std::vector<uint64_t> ahead_expected_;
    std::vector<AheadSubmission> ahead_submitted_;
    uint64_t awaiting_token_ = 0;
    uint64_t submitted_token_ = 0;
    uint64_t submitted_gpu_value_ = 0;
    GpuCompletion gpu_;
    BridgeCommands cmds_;                   // lane Q: first-write barrier, race quad clear
    EyeSwapchain race_quad_{};              // lane Q: KARTFRAME_RACE_QUAD
    bool race_quad_ready_ = false;
    bool race_quad_failed_ = false;
    bool first_write_barrier_failed_ = false;
    uint64_t first_write_barriers_ = 0;
    bool log_fresh_ = false;
    PFN_vkGetInstanceProcAddr vk_get_proc_ = nullptr;
    bool submission_arrived_ = false;
    bool submission_success_ = false;
    bool submission_unsafe_ = false;
    bool shutting_down_ = false;

    uint64_t pending_packet_serial_ = 0;
    uint64_t next_packet_serial_ = 1ull << 40;
    uint64_t timing_session_serial_ = 0;
    XrTime last_display_time_ = 0;
    XrDuration last_display_period_ = 0;
    bool last_should_render_ = false;
    uint64_t active_frame_serial_ = 0;
    uint64_t render_session_serial_ = 0;
    uint64_t render_space_serial_ = 0;
    OpenXRFrame active_frame_{};
    std::chrono::steady_clock::time_point cycle_wake_{};
    bool requirements_queried_ = false;
    bool owns_session_ = false;
    bool bridge_enabled_ = false;
    bool bound_ = false;
    bool frame_active_ = false;
    bool shutdown_unsafe_ = false;
};

OpenXRWindowsVulkanBackend::OpenXRWindowsVulkanBackend(OpenXRLogCallback logger)
    : m_impl(std::make_unique<Impl>(std::move(logger))) {}

OpenXRWindowsVulkanBackend::~OpenXRWindowsVulkanBackend() = default;

bool OpenXRWindowsVulkanBackend::QueryGraphicsRequirements(OpenXRRuntime& runtime) {
    return m_impl->QueryGraphicsRequirements(runtime);
}

bool OpenXRWindowsVulkanBackend::BindAurora(OpenXRRuntime& runtime) {
    return m_impl->BindAurora(runtime);
}

OpenXRWindowsVulkanBeginStatus OpenXRWindowsVulkanBackend::BeginFrame(
    const OpenXRWindowsVulkanPresentation& presentation, OpenXRWindowsVulkanFrame& frame) {
    return m_impl->BeginFrame(presentation, frame);
}

OpenXRWindowsVulkanSubmissionStatus OpenXRWindowsVulkanBackend::WaitForSubmissionUntil(
    const OpenXRWindowsVulkanFrame& frame, std::chrono::steady_clock::time_point deadline) {
    return m_impl->WaitForSubmissionUntil(frame, deadline);
}

OpenXRWindowsVulkanSubmissionStatus OpenXRWindowsVulkanBackend::WaitForSubmission(
    const OpenXRWindowsVulkanFrame& frame, uint32_t timeout_ms) {
    return m_impl->WaitForSubmission(frame, timeout_ms);
}

bool OpenXRWindowsVulkanBackend::TryCancelPendingFrame(OpenXRWindowsVulkanFrame& frame) {
    return m_impl->TryCancelPendingFrame(frame);
}

bool OpenXRWindowsVulkanBackend::FinishFrame(OpenXRWindowsVulkanFrame& frame, bool submit_layer) {
    return m_impl->FinishFrame(frame, submit_layer);
}

bool OpenXRWindowsVulkanBackend::RepeatFrame(const OpenXRWindowsVulkanFrame& frame) {
    return m_impl->RepeatFrame(frame);
}

OpenXRBeginStatus OpenXRWindowsVulkanBackend::PreparePacket(const OpenXRPresentation& presentation, OpenXRBackendFrame& packet) {
    return m_impl->PreparePacket(presentation, packet);
}
bool OpenXRWindowsVulkanBackend::TryCancelPendingPacket(OpenXRBackendFrame& packet) {
    return m_impl->TryCancelPendingPacket(packet);
}
OpenXRBeginStatus OpenXRWindowsVulkanBackend::BeginFrameForPacket(const OpenXRBackendFrame& packet, OpenXRBackendFrame& frame) {
    return m_impl->BeginFrameForPacket(packet, frame);
}
OpenXRSubmissionStatus OpenXRWindowsVulkanBackend::CopyRenderedEyes(const OpenXRBackendFrame& frame) {
    return m_impl->CopyRenderedEyes(frame);
}
OpenXRBeginStatus OpenXRWindowsVulkanBackend::KeepAliveCycle() { return m_impl->KeepAliveCycle(); }
OpenXRBeginStatus OpenXRWindowsVulkanBackend::BeginCycle(OpenXRFrame& cycle) { return m_impl->BeginCycle(cycle); }
std::chrono::steady_clock::time_point OpenXRWindowsVulkanBackend::CycleWakeTime() const { return m_impl->CycleWakeTime(); }
bool OpenXRWindowsVulkanBackend::HasPendingSet() const { return m_impl->HasPendingSet(); }
OpenXRBeginStatus OpenXRWindowsVulkanBackend::PrepareSet(const OpenXRPresentation& presentation, XrTime display_time,
                                                         const OpenXRFrame& cycle, OpenXRBackendFrame& set) {
    return m_impl->PrepareSet(presentation, display_time, cycle, set);
}
bool OpenXRWindowsVulkanBackend::EndCycleWithSet(OpenXRBackendFrame& set) { return m_impl->EndCycleWithSet(set); }
bool OpenXRWindowsVulkanBackend::MenuEnvironmentAvailable() const { return m_impl->MenuEnvironmentAvailable(); }
bool OpenXRWindowsVulkanBackend::EndCycleRetained() { return m_impl->EndCycleRetained(); }
bool OpenXRWindowsVulkanBackend::CancelSet(OpenXRBackendFrame& set) { return m_impl->CancelSet(set); }
bool OpenXRWindowsVulkanBackend::DropSet(OpenXRBackendFrame& set) { return m_impl->DropSet(set); }
bool OpenXRWindowsVulkanBackend::EnableAhead(bool want) { return m_impl->EnableAhead(want); }
bool OpenXRWindowsVulkanBackend::AheadCanPrepare() const { return m_impl->AheadCanPrepare(); }
OpenXRSubmissionStatus OpenXRWindowsVulkanBackend::PollAheadSet(const OpenXRBackendFrame& set,
                                                                std::chrono::steady_clock::time_point deadline,
                                                                bool gpu) {
    return m_impl->PollAheadSet(set, deadline, gpu);
}
bool OpenXRWindowsVulkanBackend::AheadSetCurrent(const OpenXRBackendFrame& set) const {
    return m_impl->AheadSetCurrent(set);
}
bool OpenXRWindowsVulkanBackend::EndCycleWithAheadSet(OpenXRBackendFrame& set) {
    return m_impl->EndCycleWithAheadSet(set);
}
bool OpenXRWindowsVulkanBackend::CancelAheadSet(OpenXRBackendFrame& set) { return m_impl->CancelAheadSet(set); }
bool OpenXRWindowsVulkanBackend::DropAheadSet(OpenXRBackendFrame& set, std::chrono::milliseconds wait) {
    return m_impl->DropAheadSet(set, wait);
}

bool OpenXRWindowsVulkanBackend::Shutdown() { return m_impl->Shutdown(); }

bool OpenXRWindowsVulkanBackend::IsBound() const { return m_impl->IsBound(); }

bool OpenXRWindowsVulkanBackend::PanelLayerAvailable() const { return m_impl->PanelLayerAvailable(); }

const OpenXRWindowsVulkanGraphicsRequirements& OpenXRWindowsVulkanBackend::GraphicsRequirements() const {
    return m_impl->GraphicsRequirements();
}

int64_t OpenXRWindowsVulkanBackend::SwapchainFormat() const { return m_impl->SwapchainFormat(); }

const std::string& OpenXRWindowsVulkanBackend::LastError() const { return m_impl->LastError(); }

uint32_t OpenXRVulkanGpuWatcherThreadId() noexcept { return g_gpu_watcher_tid.load(std::memory_order_acquire); }

} // namespace mkw::vr

#endif // defined(MKW_ENABLE_OPENXR) && defined(_WIN32)
