#pragma once

#include "VoxelMap.h"

// Simple allocator based on free-lists
struct FreeList {
    std::map<uint32_t, uint32_t> FreeRanges;
    uint32_t NumAllocated = 0;
    uint32_t Capacity;

    FreeList(uint32_t cap) {
        FreeRanges.insert({ 1, cap });
        Capacity = cap;
    }

    // Allocates or grows a range.
    uint32_t Realloc(uint32_t baseAddr, uint32_t currSize, uint32_t newSize);
    void Free(uint32_t baseAddr, uint32_t size);

private:
    void Split(auto& node, uint32_t count) {
        if (node->second > count) {
            FreeRanges.insert({ node->first + count, node->second - count });
        }
        FreeRanges.erase(node);
        NumAllocated += count;
    }
};

// Allocator for bricks within a fixed-size buffer.
// Bricks are themselves allocated at slot `BaseSlot + popcnt(AllocMask & ((1 << BrickIdx) - 1))`
struct BrickSlotAllocator {
    struct SectorInfo {
        uint32_t BaseSlot = 0;
        uint64_t AllocMask = 0;

        uint32_t GetSlot(uint32_t brickIdx) const {
            uint64_t mask = 1ull << brickIdx;
            assert((AllocMask & mask) != 0);
            return BaseSlot + (uint32_t)std::popcount(AllocMask & (mask - 1));
        }
    };

    glm::uvec2 MaxBounds; // XZ, Y
    std::unique_ptr<SectorInfo[]> Sectors;
    FreeList Arena;

    BrickSlotAllocator(glm::uvec2 maxBounds) : Arena(maxBounds.x * maxBounds.x * maxBounds.y * 64) {
        MaxBounds = maxBounds;
        Sectors = std::make_unique<SectorInfo[]>(Arena.Capacity / 64);
    }

    // Ensures that slots for bricks in the given sector are allocated, according to the given mask.
    // Returns a dirty mask for bricks in the sector that need to be updated. 
    uint64_t Alloc(SectorInfo* sector, uint64_t mask);
    uint64_t Free(SectorInfo* sector, uint64_t mask);

    SectorInfo* GetSector(glm::uvec3 pos) {
        if ((pos.x | pos.z) >= MaxBounds.x || pos.y >= MaxBounds.y) {
            return nullptr;
        }
        uint32_t idx = GetLinearIndex(pos, MaxBounds.x, MaxBounds.y);
        return &Sectors[idx];
    }
};