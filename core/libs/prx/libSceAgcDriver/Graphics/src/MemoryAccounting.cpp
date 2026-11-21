#include "prx/libSceAgcDriver/Graphics/include/MemoryAccounting.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DeferredDestruction.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unistd.h>

namespace AgcDriver::Graphics {
namespace {

struct DeviceAllocations {
    std::map<VkDeviceMemory, MemoryAccounting::Allocation> allocations;
    std::chrono::steady_clock::time_point reported{};
};

struct Accounting {
    std::mutex mutex;
    std::map<VkDevice, DeviceAllocations> devices;
    std::atomic<bool> complete{true};
};

Accounting& accounting() {
    static Accounting value;
    return value;
}

// The FreeBSD NVIDIA driver refuses every allocation of 256 MiB or more from a system-memory heap (imports of host
// pointers are not bounded), and once it has refused one it refuses almost every later vkAllocateMemory of the
// device. Such requests are answered here, without reaching it.
struct BoundedDevice {
    PFN_vkGetDeviceProcAddr resolver = nullptr;
    PFN_vkAllocateMemory allocate = nullptr;
    VkPhysicalDeviceMemoryProperties properties{};
    VkDeviceSize limit = 0;
};

struct BoundedDevices {
    std::mutex mutex;
    std::map<VkDevice, BoundedDevice> devices;
};

BoundedDevices& boundedDevices() {
    static BoundedDevices value;
    return value;
}

std::optional<BoundedDevice> boundedDevice(VkDevice device) {
    auto& state = boundedDevices();
    std::lock_guard lock(state.mutex);
    const auto found = state.devices.find(device);
    if (found == state.devices.end()) return std::nullopt;
    return found->second;
}

bool imports(const void* next) {
    for (auto* structure = static_cast<const VkBaseInStructure*>(next); structure != nullptr; structure = structure->pNext)
        if (structure->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT || structure->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR) return true;
    return false;
}

VKAPI_ATTR VkResult VKAPI_CALL boundedAllocateMemory(VkDevice device, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks* allocator, VkDeviceMemory* memory) {
    const auto bounded = boundedDevice(device);
    if (!bounded) return VK_ERROR_INITIALIZATION_FAILED;
    if (info != nullptr && info->memoryTypeIndex < bounded->properties.memoryTypeCount && info->allocationSize > bounded->limit && !imports(info->pNext)) {
        const auto heap = bounded->properties.memoryTypes[info->memoryTypeIndex].heapIndex;
        if ((bounded->properties.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    const auto result = bounded->allocate(device, info, allocator, memory);
    if (result != VK_SUCCESS && info != nullptr && info->memoryTypeIndex < bounded->properties.memoryTypeCount) {
        // A refusal is worth knowing about here: the driver refuses almost everything after its first one.
        std::fprintf(stderr, "[gpu] vkAllocateMemory of %llu bytes from memory type %u (heap %u)%s: Vulkan result %d\n", static_cast<unsigned long long>(info->allocationSize),
            info->memoryTypeIndex, bounded->properties.memoryTypes[info->memoryTypeIndex].heapIndex, imports(info->pNext) ? " importing" : "", static_cast<int>(result));
    }
    return result;
}

// The FreeBSD NVIDIA shader compiler initializes itself once, in the first pipeline it actually compiles; when several
// threads compile their first pipelines together that initialization deadlocks (a pthread_once routine waits for a
// mutex held by threads queued behind the once). One pipeline is compiled alone when the device is created. Its
// specialization constant is new in every process, so neither a pipeline cache nor the driver's disk cache can answer
// it without compiling. Source (glslang -V --target-env vulkan1.1):
//     #version 450
//     layout(local_size_x = 1) in;
//     layout(constant_id = 0) const uint key = 0u;
//     shared uint value;
//     void main() { value = key; }
constexpr std::uint32_t CompilerWarmup[] = {
    0x07230203, 0x00010300, 0x0008000b, 0x0000000d, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0005000f, 0x00000005, 0x00000004, 0x6e69616d, 0x00000000, 0x00060010, 0x00000004, 0x00000011,
    0x00000001, 0x00000001, 0x00000001, 0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000004,
    0x6e69616d, 0x00000000, 0x00040005, 0x00000008, 0x756c6176, 0x00000065, 0x00030005, 0x00000009,
    0x0079656b, 0x00040047, 0x00000009, 0x00000001, 0x00000000, 0x00040047, 0x0000000c, 0x0000000b,
    0x00000019, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00040015, 0x00000006,
    0x00000020, 0x00000000, 0x00040020, 0x00000007, 0x00000004, 0x00000006, 0x0004003b, 0x00000007,
    0x00000008, 0x00000004, 0x00040032, 0x00000006, 0x00000009, 0x00000000, 0x00040017, 0x0000000a,
    0x00000006, 0x00000003, 0x0004002b, 0x00000006, 0x0000000b, 0x00000001, 0x0006002c, 0x0000000a,
    0x0000000c, 0x0000000b, 0x0000000b, 0x0000000b, 0x00050036, 0x00000002, 0x00000004, 0x00000000,
    0x00000003, 0x000200f8, 0x00000005, 0x0003003e, 0x00000008, 0x00000009, 0x000100fd, 0x00010038
};

void warmUpCompiler(VkDevice device, PFN_vkGetDeviceProcAddr resolver) {
    const auto createModule = reinterpret_cast<PFN_vkCreateShaderModule>(resolver(device, "vkCreateShaderModule"));
    const auto destroyModule = reinterpret_cast<PFN_vkDestroyShaderModule>(resolver(device, "vkDestroyShaderModule"));
    const auto createLayout = reinterpret_cast<PFN_vkCreatePipelineLayout>(resolver(device, "vkCreatePipelineLayout"));
    const auto destroyLayout = reinterpret_cast<PFN_vkDestroyPipelineLayout>(resolver(device, "vkDestroyPipelineLayout"));
    const auto createPipelines = reinterpret_cast<PFN_vkCreateComputePipelines>(resolver(device, "vkCreateComputePipelines"));
    const auto destroyPipeline = reinterpret_cast<PFN_vkDestroyPipeline>(resolver(device, "vkDestroyPipeline"));
    if (!createModule || !destroyModule || !createLayout || !destroyLayout || !createPipelines || !destroyPipeline)
        throw std::runtime_error("AGC graphics: missing Vulkan function for the shader compiler warm-up");
    VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = sizeof(CompilerWarmup);
    moduleInfo.pCode = CompilerWarmup;
    VkShaderModule module = VK_NULL_HANDLE;
    if (createModule(device, &moduleInfo, nullptr, &module) != VK_SUCCESS) throw std::runtime_error("AGC graphics: vkCreateShaderModule for the shader compiler warm-up failed");
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (createLayout(device, &layoutInfo, nullptr, &layout) != VK_SUCCESS) {
        destroyModule(device, module, nullptr);
        throw std::runtime_error("AGC graphics: vkCreatePipelineLayout for the shader compiler warm-up failed");
    }
    const std::uint32_t key = static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^ static_cast<std::uint32_t>(::getpid());
    const VkSpecializationMapEntry entry{0, 0, sizeof(key)};
    const VkSpecializationInfo specialization{1, &entry, sizeof(key), &key};
    VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", &specialization};
    pipelineInfo.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = createPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
    if (result == VK_SUCCESS) destroyPipeline(device, pipeline, nullptr);
    destroyLayout(device, layout, nullptr);
    destroyModule(device, module, nullptr);
    if (result != VK_SUCCESS) throw std::runtime_error("AGC graphics: the shader compiler warm-up pipeline failed: Vulkan result " + std::to_string(result));
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL boundedDeviceProc(VkDevice device, const char* name) {
    const auto bounded = boundedDevice(device);
    if (!bounded) return nullptr;
    if (std::strcmp(name, "vkAllocateMemory") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&boundedAllocateMemory);
    return bounded->resolver(device, name);
}

}

bool MemoryAccounting::Enabled() {
    static const bool enabled = std::getenv("APS5_TRACE_MEMORY_BUDGET") != nullptr;
    return enabled;
}

void MemoryAccounting::Record(VkDevice device, VkDeviceMemory memory, const VkMemoryAllocateInfo& allocation, const VkPhysicalDeviceMemoryProperties& properties, const char* category) noexcept {
    if (!Enabled() || memory == VK_NULL_HANDLE) return;
    auto& state = accounting();
    try {
        const auto heap = allocation.memoryTypeIndex < properties.memoryTypeCount ? properties.memoryTypes[allocation.memoryTypeIndex].heapIndex : VK_MAX_MEMORY_HEAPS;
        std::lock_guard lock(state.mutex);
        auto& allocations = state.devices[device].allocations;
        if (allocations.contains(memory)) state.complete = false;
        allocations.insert_or_assign(memory, Allocation{memory, allocation.allocationSize, 0, heap, category});
    } catch (...) {
        state.complete = false;
    }
}

VkDeviceSize MemoryAccounting::HostAllocationLimit(VkDevice device) noexcept {
    try {
        const auto bounded = boundedDevice(device);
        return bounded ? bounded->limit : std::numeric_limits<VkDeviceSize>::max();
    } catch (...) {
        return std::numeric_limits<VkDeviceSize>::max();
    }
}

PFN_vkGetDeviceProcAddr MemoryAccounting::GuardDriverFaults(VkDevice device, PFN_vkGetDeviceProcAddr resolver, std::uint32_t vendor, const VkPhysicalDeviceMemoryProperties& properties) {
#if defined(__FreeBSD__)
    constexpr std::uint32_t Nvidia = 0x10de;
    if (vendor != Nvidia || device == VK_NULL_HANDLE || resolver == nullptr) return resolver;
    BoundedDevice bounded;
    bounded.resolver = resolver;
    bounded.allocate = reinterpret_cast<PFN_vkAllocateMemory>(resolver(device, "vkAllocateMemory"));
    if (bounded.allocate == nullptr) throw std::runtime_error("AGC graphics: missing Vulkan function: vkAllocateMemory");
    warmUpCompiler(device, resolver);
    bounded.properties = properties;
    bounded.limit = (256ull << 20) - 1;
    auto& state = boundedDevices();
    std::lock_guard lock(state.mutex);
    state.devices.insert_or_assign(device, bounded);
    return &boundedDeviceProc;
#else
    static_cast<void>(device);
    static_cast<void>(vendor);
    static_cast<void>(properties);
    return resolver;
#endif
}

VkResult MemoryAccounting::Allocate(const Context& context, const VkMemoryAllocateInfo& allocation, const char* category, VkDeviceMemory* memory) {
    const auto result = context.Resolved(&DeviceFunctions::allocateMemory, "vkAllocateMemory")(context.device, &allocation, nullptr, memory);
    if (result == VK_SUCCESS) Record(context.device, *memory, allocation, context.memory, category);
    return result;
}

void MemoryAccounting::Free(PFN_vkFreeMemory freeMemory, VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* allocator) {
    if (Enabled() && memory != VK_NULL_HANDLE && !IsDeferredMemoryFree(freeMemory)) {
        auto& state = accounting();
        try {
            std::lock_guard lock(state.mutex);
            const auto found = state.devices.find(device);
            if (found != state.devices.end()) found->second.allocations.erase(memory);
        } catch (...) {
            state.complete = false;
        }
    }
    freeMemory(device, memory, allocator);
}

void MemoryAccounting::Free(const Context& context, VkDeviceMemory memory) {
    Free(context.Resolved(&DeviceFunctions::freeMemory, "vkFreeMemory"), context.device, memory);
}

void MemoryAccounting::Use(VkDevice device, VkDeviceMemory memory, const char* category) noexcept {
    if (!Enabled()) return;
    auto& state = accounting();
    try {
        std::lock_guard lock(state.mutex);
        const auto found = state.devices.find(device);
        if (found == state.devices.end()) return;
        const auto allocation = found->second.allocations.find(memory);
        if (allocation == found->second.allocations.end()) return;
        allocation->second.retained = 0;
        if (category != nullptr) allocation->second.category = category;
    } catch (...) {
        state.complete = false;
    }
}

void MemoryAccounting::Retain(VkDevice device, VkDeviceMemory memory, VkDeviceSize bytes) noexcept {
    if (!Enabled()) return;
    auto& state = accounting();
    try {
        std::lock_guard lock(state.mutex);
        const auto found = state.devices.find(device);
        if (found == state.devices.end()) return;
        const auto allocation = found->second.allocations.find(memory);
        if (allocation == found->second.allocations.end()) return;
        if (bytes > allocation->second.bytes) state.complete = false;
        allocation->second.retained = std::min(bytes, allocation->second.bytes);
    } catch (...) {
        state.complete = false;
    }
}

void MemoryAccounting::ForgetDevice(VkDevice device) noexcept {
    if (!Enabled()) return;
    auto& state = accounting();
    try {
        std::lock_guard lock(state.mutex);
        state.devices.erase(device);
    } catch (...) {
        state.complete = false;
    }
}

void MemoryAccounting::RetainPart(VkDevice device, VkDeviceMemory memory, VkDeviceSize bytes, bool retained) noexcept {
    if (!Enabled()) return;
    auto& state = accounting();
    try {
        std::lock_guard lock(state.mutex);
        const auto found = state.devices.find(device);
        if (found == state.devices.end()) return;
        const auto allocation = found->second.allocations.find(memory);
        if (allocation == found->second.allocations.end()) return;
        auto& value = allocation->second;
        const auto available = retained ? value.bytes - value.retained : value.retained;
        if (bytes > available) state.complete = false;
        const auto change = std::min(bytes, available);
        if (retained) value.retained += change;
        else value.retained -= change;
    } catch (...) {
        state.complete = false;
    }
}

MemoryAccounting::Snapshot MemoryAccounting::Read(VkDevice device) {
    Snapshot result;
    if (!Enabled()) return result;
    auto& state = accounting();
    std::lock_guard lock(state.mutex);
    result.complete = state.complete;
    const auto found = state.devices.find(device);
    if (found == state.devices.end()) return result;
    result.allocations.reserve(found->second.allocations.size());
    for (const auto& [memory, allocation] : found->second.allocations) result.allocations.push_back(allocation);
    return result;
}

void MemoryAccounting::Report(const Context& context, const VkPhysicalDeviceMemoryBudgetPropertiesEXT& reported, const StorageTextureBudgetSnapshot& storage) noexcept {
    if (!Enabled()) return;
    try {
        auto& state = accounting();
        {
            std::lock_guard lock(state.mutex);
            auto& last = state.devices[context.device].reported;
            const auto now = std::chrono::steady_clock::now();
            if (last != std::chrono::steady_clock::time_point{} && now - last < std::chrono::seconds(10)) return;
            last = now;
        }
        const auto snapshot = Read(context.device);
        std::map<std::pair<std::uint32_t, std::string>, std::array<VkDeviceSize, 2>> categories;
        std::array<std::array<VkDeviceSize, 2>, VK_MAX_MEMORY_HEAPS> totals{};
        for (const auto& allocation : snapshot.allocations) {
            auto& category = categories[{allocation.heap, allocation.category}];
            category[0] += allocation.bytes - allocation.retained;
            category[1] += allocation.retained;
            if (allocation.heap >= VK_MAX_MEMORY_HEAPS) continue;
            totals[allocation.heap][0] += allocation.bytes;
            totals[allocation.heap][1] += allocation.retained;
        }
        for (std::uint32_t heap = 0; heap < context.memory.memoryHeapCount && heap < VK_MAX_MEMORY_HEAPS; ++heap) {
            const auto allocated = totals[heap][0];
            const auto retained = totals[heap][1];
            const auto usage = reported.heapUsage[heap];
            std::fprintf(stderr, "[gpu-memory] heap=%u budget=%llu usage=%llu allocated=%llu active=%llu retained=%llu unattributed_usage=%llu tracked_above_usage=%llu complete=%u\n", heap,
                static_cast<unsigned long long>(reported.heapBudget[heap]), static_cast<unsigned long long>(usage), static_cast<unsigned long long>(allocated),
                static_cast<unsigned long long>(allocated - retained), static_cast<unsigned long long>(retained), static_cast<unsigned long long>(usage - std::min(usage, allocated)),
                static_cast<unsigned long long>(allocated - std::min(usage, allocated)), snapshot.complete ? 1u : 0u);
        }
        for (const auto& [key, bytes] : categories) {
            std::fprintf(stderr, "[gpu-memory] heap=%u category=\"%s\" active=%llu retained=%llu\n", key.first, key.second.c_str(), static_cast<unsigned long long>(bytes[0]), static_cast<unsigned long long>(bytes[1]));
        }
        if (storage.heap) {
            std::fprintf(stderr, "[gpu-memory] storage heap=%u sampled_live=%llu storage_cached=%llu reserved=%llu available=%llu cap=%llu selected=%llu\n", *storage.heap,
                static_cast<unsigned long long>(storage.sampledBytes), static_cast<unsigned long long>(storage.storageBytes), static_cast<unsigned long long>(storage.reserved),
                static_cast<unsigned long long>(storage.available), static_cast<unsigned long long>(storage.ceiling), static_cast<unsigned long long>(storage.selectedBudget));
        }
    } catch (...) {
        accounting().complete = false;
        std::fprintf(stderr, "[gpu-memory] allocation report unavailable\n");
    }
}

}
