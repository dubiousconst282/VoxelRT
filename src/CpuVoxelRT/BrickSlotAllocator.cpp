#include "BrickSlotAllocator.h"

uint64_t BrickSlotAllocator::Alloc(SectorInfo* sector, uint64_t mask) {
    uint64_t newMask = sector->AllocMask | mask;
    if (newMask == sector->AllocMask) return 0;

    uint32_t currSize = (uint32_t)std::popcount(sector->AllocMask);
    uint32_t newSize = (uint32_t)std::popcount(newMask);
    uint32_t newBase = Arena.Realloc(sector->BaseSlot, currSize, newSize);

    if (newBase == 0) {
        // TODO: defrag storage
        throw std::exception("Could not allocate brick slots");
    }
    sector->BaseSlot = newBase;
    sector->AllocMask = newMask;

    // After reallocating slots, we could be allocating bricks in the middle of
    // the old mask which will offset slots that are already in use.
    // For now, we'll just say that the entire sector must be updated.
    return newMask;
}

uint64_t BrickSlotAllocator::Free(SectorInfo* sector, uint64_t mask) {
    uint64_t newMask = sector->AllocMask & ~mask;
    if (newMask == sector->AllocMask) return 0;

    uint32_t currSize = (uint32_t)std::popcount(sector->AllocMask);
    uint32_t newSize = (uint32_t)std::popcount(newMask);
    Arena.Free(sector->BaseSlot + newSize, currSize - newSize);
    
    sector->AllocMask = newMask;

    return newMask;
}

uint32_t FreeList::Realloc(uint32_t baseAddr, uint32_t currSize, uint32_t newSize) {
    assert(baseAddr == 0 ? currSize == 0 : true);
    assert(newSize >= currSize);
    
    NumAllocated += newSize - currSize;

    // Try to bump current allocation first
    if (baseAddr != 0) {
        auto itr = FreeRanges.lower_bound(baseAddr);

        if (itr != FreeRanges.end() && itr->first < baseAddr + newSize) {
            Split(itr, baseAddr + newSize - itr->first);
            return baseAddr;
        }

        Free(baseAddr, currSize);
    }

    // Scan free-list
    for (auto itr = FreeRanges.begin(); itr != FreeRanges.end(); itr++) {
        if (itr->second >= newSize) {
            uint32_t freeAddrBase = itr->first;
            Split(itr, newSize);
            return freeAddrBase;
        }
    }
    return 0;
}

void FreeList::Free(uint32_t baseAddr, uint32_t size) {
    NumAllocated -= size;
    auto itr = FreeRanges.lower_bound(baseAddr);

    // Coalesce on left side
    if (itr != FreeRanges.end() && baseAddr + size == itr->first) {
        FreeRanges.insert({ baseAddr, itr->second + size });
        FreeRanges.erase(itr);
        return;
    }
    // Coalesce on right side
    if (itr != FreeRanges.begin()) {
        --itr;

        if (itr->first + itr->second == baseAddr) {
            itr->second += size;
            return;
        }
    }
    // No coalescing possible
    FreeRanges.insert({ baseAddr, size });
}
