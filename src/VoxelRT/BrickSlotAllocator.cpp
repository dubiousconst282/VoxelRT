#include "BrickSlotAllocator.h"

#include <stdexcept>

uint64_t BrickSlotAllocator::Alloc(SectorInfo* sector, uint64_t mask) {
    uint64_t newMask = sector->AllocMask | mask;
    if (newMask == sector->AllocMask) return 0;

    uint32_t currSize = (uint32_t)std::popcount(sector->AllocMask);
    uint32_t newSize = (uint32_t)std::popcount(newMask);
    uint32_t newBase = Arena.Realloc(sector->BaseSlot, currSize, newSize);

    if (newBase == 0) {
        // TODO: defrag storage? and/or maybe add heuristics in FreeList::Realloc to minimize fragmentation
        throw std::runtime_error("Could not allocate brick slots");
    }
    sector->BaseSlot = newBase;
    sector->AllocMask = newMask;

    // Even if newBase hasn't changed, some of the old slots will be offset if
    // new bricks are allocated in the middle of the old mask.
    // For now, we'll just always say that the entire sector must be updated.
    return newMask;
}

uint64_t BrickSlotAllocator::Free(SectorInfo* sector, uint64_t mask) {
    uint64_t newMask = sector->AllocMask & ~mask;
    if (newMask == sector->AllocMask) return 0;

    uint32_t currSize = (uint32_t)std::popcount(sector->AllocMask);
    uint32_t newSize = (uint32_t)std::popcount(newMask);
    Arena.Free(sector->BaseSlot + newSize, currSize - newSize);

    if (newMask == 0) {
        sector->BaseSlot = 0;
    }
    sector->AllocMask = newMask;

    return newMask;
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