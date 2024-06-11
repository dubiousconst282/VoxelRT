#pragma once

#include "Havk.h"
#include <unordered_map>


namespace havk {

// Manages the global image descriptor heap.
struct DescriptorHeap {
    HAVK_NON_COPYABLE(DescriptorHeap);

    static const uint32_t Capacity = 1024 * 64;  // Per SAMPLED / STORAGE

    DeviceContext* Context;
    VkDescriptorPool Pool;
    VkDescriptorSetLayout SetLayout;
    VkDescriptorSet Set;

    DescriptorHeap(DeviceContext* ctx);
    ~DescriptorHeap();

    uint32_t CreateHandle(VkImageView viewHandle, VkImageUsageFlags usage);
    void DestroyHandle(uint32_t handle);

private:
    struct HandleAllocator {
        uint32_t NextFreeWordIdxHint = 0;
        uint64_t UsedMap[(Capacity + 63) / 64] = {};

        uint32_t Alloc();
        void Free(uint32_t id);
    };
    HandleAllocator _allocator;
};

struct SamplerDescriptorPool {
    HAVK_NON_COPYABLE(SamplerDescriptorPool);

    DeviceContext* Context;

    SamplerDescriptorPool(DeviceContext* ctx) : Context(ctx) { }
    ~SamplerDescriptorPool();

    VkSampler GetSampler(const VkSamplerCreateInfo& desc);

    void CreateSet(std::vector<VkDescriptorSetLayoutBinding> bindings, VkDescriptorSetLayout* layout, SamplerDescriptorSet* set);
    void DestroySet(SamplerDescriptorSet& set);

private:
    static const uint32_t PoolCapacity = 4096;
    std::vector<VkDescriptorPool> _pools;

    struct SamplerHasherEq {
        bool operator()(const VkSamplerCreateInfo& a, const VkSamplerCreateInfo& b) const {
            return memcmp(&a, &b, sizeof(VkSamplerCreateInfo)) == 0;
        }
        size_t operator()(const VkSamplerCreateInfo& obj) const {
            return std::hash<int32_t>()(obj.magFilter ^ (obj.minFilter << 4) ^ (obj.mipmapMode << 8) ^ (obj.addressModeU << 12) ^
                                        (obj.addressModeV << 16) ^ (obj.compareOp << 20));
        }
    };
    std::unordered_map<VkSamplerCreateInfo, VkSampler, SamplerHasherEq, SamplerHasherEq> _samplers;
};

struct FileWatcher {
    HAVK_NON_COPYABLE(FileWatcher);

    FileWatcher(const std::filesystem::path& path);
    ~FileWatcher();

    void PollChanges(std::vector<std::filesystem::path>& changedFiles);

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

struct PipelineBuilder::HotReloadTracker {
    FileWatcher SourceWatcher;
    std::unordered_map<Pipeline*, PipelineSourceInfo> PipelineSources;

    HotReloadTracker(const std::filesystem::path& basePath) : SourceWatcher(basePath) {}
};
struct PipelineBuilder::PipelineSourceInfo {
    ShaderCompileParams CompilePars;
    std::string MainSourceFile;
    std::vector<std::filesystem::path> IncludedSourceFiles;
    std::unique_ptr<GraphicsPipelineDesc> GraphicsDesc = nullptr;

    bool IsRelatedFile(const std::filesystem::path& path) {
        if (MainSourceFile == path) {
            return true;
        }
        for (auto& includePath : IncludedSourceFiles) {
            if (includePath == path) {
                return true;
            }
        }
        return false;
    }
};

};