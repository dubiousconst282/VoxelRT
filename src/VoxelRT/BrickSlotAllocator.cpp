#include "BrickSlotAllocator.h"

#include <stdexcept>

static uint32_t GetNumPages(uint64_t mask, uint32_t levelOfDetail) {
    uint32_t size = (uint32_t)std::popcount(mask) * SectorAllocInfo::GetBrickStride(levelOfDetail);

    const uint32_t m = SectorAllocInfo::PageSize;
    return (size + m - 1) / m; // ceil div
}

void BrickSlotAllocator::Reserve(SectorAllocInfo* sector, uint64_t newMask, uint32_t newLod) {
    uint32_t currSize = GetNumPages(sector->AllocMask, sector->LevelOfDetail);
    uint32_t newSize = GetNumPages(newMask, newLod);

    if (newSize > currSize) {
        sector->BasePage = Arena.Realloc(sector->BasePage, currSize, newSize);

        if (sector->BasePage == 0) {
            // TODO: defrag storage? and/or maybe add heuristics in FreeList::Realloc to minimize fragmentation
            throw std::runtime_error("Could not allocate sector storage");
        }
    } else if (newSize < currSize) {
        Arena.Free(sector->BasePage + newSize, currSize - newSize);

        if (newSize == 0) {
            sector->BasePage = 0;
        }
    }

    sector->AllocMask = newMask;
    sector->LevelOfDetail = newLod;
}

uint32_t FreeList::Realloc(uint32_t baseAddr, uint32_t currSize, uint32_t newSize) {
    assert(baseAddr == 0 ? currSize == 0 : true);
    assert(newSize >= currSize);

    // Try to bump current allocation first
    if (baseAddr != 0) {
        auto itr = FreeRanges.lower_bound(baseAddr);
        uint32_t currEndAddr = baseAddr + currSize;
        uint32_t newEndAddr = baseAddr + newSize;

        if (itr != FreeRanges.end() && itr->first == currEndAddr && newEndAddr < itr->first + itr->second) {
            Split(itr, newEndAddr - itr->first);
            return baseAddr;
        }

        Free(baseAddr, currSize);
    }

    // Find best fitting node (that leads to least amount of fragmentation)
    auto bestNode = FreeRanges.end();
    for (auto itr = FreeRanges.begin(); itr != FreeRanges.end(); itr++) {
        if (itr->second >= newSize && (bestNode == FreeRanges.end() || itr->second < bestNode->second)) {
            bestNode = itr;
        }
    }
    if (bestNode != FreeRanges.end()) {
        uint32_t freeAddrBase = bestNode->first;
        Split(bestNode, newSize);
        return freeAddrBase;
    }
    return 0;
}

void FreeList::Free(uint32_t baseAddr, uint32_t size) {
    NumAllocated -= size;
    auto iter = FreeRanges.insert({ baseAddr, size }).first;

    // Advance one node ahead so that we only need to coalesce in one direction
    if (iter != FreeRanges.end()) {
        iter++;
    }

    // Coalesce ranges from right to left
    while (iter != FreeRanges.begin()) {
        auto next = iter--;

        if (iter->first + iter->second == next->first) {
            iter->second += next->second;
            FreeRanges.erase(next);
        } else if (iter->first < baseAddr) {
            break;
        }
    }
}