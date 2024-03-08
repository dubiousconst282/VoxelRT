#pragma once

#include <vector>
#include <thread>

#include "VoxelMap.h"


struct TerrainGenerator {
    using GeneratedSector = std::pair<glm::ivec3, std::unique_ptr<Sector>>;

    TerrainGenerator(std::shared_ptr<VoxelMap> map);
    ~TerrainGenerator();

    // Requests generation of a sector at the given coords.
    void RequestSector(glm::ivec3 sectorPos);
    
    // Attempts to dequeue an generated sector.
    // Returns an empty pair if no avail.
    GeneratedSector Poll();

    uint32_t GetNumPendingRequests() const;

private:
    struct RequestQueue;

    std::shared_ptr<VoxelMap> _map;
    RequestQueue* _queue;  // can't use unique_ptr on forward types
    std::vector<std::jthread> _threads;

    void WorkerFn();

    // Generate terrain for the specified sector, overwriting all voxels.
    // Returns a mask of non-empty bricks.
    uint64_t GenerateSector(Sector& sector, glm::ivec3 pos);
};