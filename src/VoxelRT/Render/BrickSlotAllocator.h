#pragma once

#include "../VoxelMap.h"

// Simple allocator based on free-lists
struct FreeList {
    std::map<uint32_t, uint32_t> FreeRanges;
    uint32_t NumAllocated = 0;
    uint32_t Capacity;

    FreeList(uint32_t cap) {
        assert(cap < UINT_MAX);
        FreeRanges.insert({ 1, cap });
        Capacity = cap;
    }

    // Allocates, frees, or attempts to resize an existing range.
    // Returns the new base address, or zero on failure/freeing.
    uint32_t Realloc(uint32_t baseAddr, uint32_t currSize, uint32_t newSize);

private:
    void Split(auto& node, uint32_t count) {
        if (node->second > count) {
            FreeRanges.insert({ node->first + count, node->second - count });
        }
        FreeRanges.erase(node);
        NumAllocated += count;
    }
    void Free(uint32_t baseAddr, uint32_t size);
};


struct SectorAllocInfo {
    // Alignment of sector addresses, and minimum allocation size in bytes.
    static constexpr uint32_t PageSize = 16;
    
    uint32_t BasePage : 28 = 0;
    uint32_t LevelOfDetail : 2 = 0;
    uint32_t RequestedFlag : 1 = 0;

    uint64_t AllocMask = 0;

    uint64_t GetBrickAddress(uint32_t brickIdx) const {
        uint64_t mask = 1ull << brickIdx;
        assert((AllocMask & mask) != 0);

        uint32_t allocIdx = (uint32_t)std::popcount(AllocMask & (mask - 1));
        return GetBaseAddress() + allocIdx * GetBrickStride(LevelOfDetail);
    }

    uint32_t GetPackedHeader() const {
        if (AllocMask == 0) return 0;
        return uint32_t(BasePage << 4 | RequestedFlag << 2 | LevelOfDetail << 0);
    }

    uint64_t GetBaseAddress() const { return BasePage * PageSize; }
    uint64_t GetDataSize() const {
        uint32_t numAllocs = (uint32_t)std::popcount(AllocMask);
        return numAllocs * GetBrickStride(LevelOfDetail);
    }

    static uint32_t GetBrickStride(uint32_t levelOfDetail) {
        return sizeof(Brick) >> (levelOfDetail * 3);
    }
};
// Allocator for bricks within a fixed-size buffer.
// Bricks are themselves allocated at slot `BaseSlot + popcnt(AllocMask & ((1 << BrickIdx) - 1))`
struct BrickSlotAllocator {
    glm::uvec2 MaxBounds; // XZ, Y
    std::unique_ptr<SectorAllocInfo[]> Sectors;
    FreeList Arena = { UINT_MAX / SectorAllocInfo::PageSize };

    BrickSlotAllocator(glm::uvec2 maxBounds) {
        MaxBounds = maxBounds;
        Sectors = std::make_unique<SectorAllocInfo[]>(maxBounds.x * maxBounds.x * maxBounds.y);
    }

    // Allocates or releases storage addresses for bricks in the given sector, according to the given mask.
    void Reserve(SectorAllocInfo* sector, uint64_t newMask, uint32_t newLod);

    SectorAllocInfo* GetSector(glm::uvec3 pos) {
        if ((pos.x | pos.z) >= MaxBounds.x || pos.y >= MaxBounds.y) {
            return nullptr;
        }
        uint32_t idx = GetLinearIndex(pos, MaxBounds.x, MaxBounds.y);
        return &Sectors[idx];
    }
};