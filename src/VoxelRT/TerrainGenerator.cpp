#include "TerrainGenerator.h"

#include <FastNoise/FastNoise.h>

uint64_t TerrainGenerator::GenerateSector(Sector& sector, glm::ivec3 sectorPos) {
    static auto noiseGen = FastNoise::NewFromEncodedNodeTree("EQACAAAAAAAgQBAAAAAAQBkAEwDD9Sg/DQAEAAAAAAAgQAkAAGZmJj8AAAAAPwEEAAAAAAAAAEBAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAM3MTD4AMzMzPwAAAAA/");
    //static auto noiseGen = FastNoise::NewFromEncodedNodeTree("EQACAAAAAAAgQBAAAAAAQBkADQAEAAAAAAAgQAkAAGZmJj8AAAAAPwEEAAAAAABmZuY/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAgD4ApHA9PwAAAAA/");
    auto noiseBuffer = std::make_unique<float[]>(32 * 32 * 32);

    noiseGen->GenUniformGrid3D(noiseBuffer.get(), sectorPos.x * 32, sectorPos.y * 32 - 96, sectorPos.z * 32, 32, 32, 32, 0.004, 12345);

    uint64_t mask = 0;

    for (uint32_t i = 0; i < 64; i++) {
        Brick* brick = sector.GetBrick(i, true);
        glm::ivec3 brickPos = sectorPos * MaskIndexer::Size + MaskIndexer::GetPos(i);
        bool isNonEmpty = false;

        brick->DispatchSIMD([&](VoxelDispatchInvocationPars& p) {
            VFloat noise = VFloat::gather<4>(noiseBuffer.get(), (p.X & 31) + (p.Y & 31) * 32 + (p.Z & 31) * (32 * 32));
            VMask fillMask = noise < 0.0;
            p.VoxelIds = simd::csel(fillMask, VInt(251), VInt(0));

            isNonEmpty |= simd::any(fillMask);
            return true;
        }, brickPos);

        if (isNonEmpty) {
            mask |= 1ull << i;
        }
    }
    return mask;
}

#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>

#include <format>
#include <iostream>

template<class... ArgTypes>
static void Log(const std::format_string<ArgTypes...> fmt, ArgTypes&&... args) {
#if 0
    auto opaqueTid = std::this_thread::get_id();
    uint32_t tid = *(uint32_t*)&opaqueTid;

    std::string text = std::format("[TerrainGen T{}]: ", tid);
    std::format_to(std::back_inserter(text), fmt, args...);
    text += "\n";

    std::cout << text;
    std::cout.flush();
#endif
}

struct TerrainGenerator::RequestQueue {
    std::mutex Mutex;
    std::condition_variable AvailRequest;

    std::queue<glm::ivec3> RequestQueue;
    std::queue<GeneratedSector> OutputQueue;
    volatile bool Exit = false; // TODO: should this be atomic_bool?

    bool WaitRequest(glm::ivec3& pos) {
        std::unique_lock<std::mutex> lock(Mutex);
        while (RequestQueue.empty() && !Exit) {
            AvailRequest.wait(lock);
        }
        if (Exit) return false;

        pos = RequestQueue.front();
        RequestQueue.pop();
        return true;
    }
    void OfferResult(glm::ivec3 pos, std::unique_ptr<Sector> sector) {
        std::unique_lock<std::mutex> lock(Mutex);
        OutputQueue.push({ pos, std::move(sector) });
    }
};

TerrainGenerator::TerrainGenerator(std::shared_ptr<VoxelMap> map) {
    _map = std::move(map);
    _queue = std::make_unique<RequestQueue>();

    uint32_t numThreads = std::max(std::thread::hardware_concurrency() * 3 / 4, 1u);
    for (uint32_t i = 0; i < numThreads; i++) {
        _threads.emplace_back(&TerrainGenerator::WorkerFn, this);
    }
}
TerrainGenerator::~TerrainGenerator() {
    Log("Exit queue");
    _queue->Exit = true;
    _queue->AvailRequest.notify_all();
    _threads.clear();

    Log("Destroy");
}

void TerrainGenerator::RequestSector(glm::ivec3 sectorPos) {
    Log("Request {} {} {}", sectorPos.x, sectorPos.y, sectorPos.z);

    std::unique_lock<std::mutex> lock(_queue->Mutex);
    _queue->RequestQueue.push(sectorPos);

    // Manual unlocking is done before notifying, to avoid waking up
    // the waiting thread only to block again (see notify_one for details)
    lock.unlock();
    _queue->AvailRequest.notify_one();
}

TerrainGenerator::GeneratedSector TerrainGenerator::Poll() {
    std::unique_lock<std::mutex> lock(_queue->Mutex);
    if (_queue->OutputQueue.empty()) {
        return {};
    }
    auto res = std::move(_queue->OutputQueue.front());
    _queue->OutputQueue.pop();
    return std::move(res);
}

uint32_t TerrainGenerator::GetNumPendingRequests() const { return _queue->RequestQueue.size(); }

void TerrainGenerator::WorkerFn() {
    glm::ivec3 pos;
    Sector workSector;

    while (_queue->WaitRequest(pos)) {
        Log("Take job {} {} {}", pos.x, pos.y, pos.z);

        uint64_t mask = GenerateSector(workSector, pos);

        // Copy non-empty bricks to new sector
        auto sector = std::make_unique<Sector>();
        sector->Storage.reserve((uint32_t)std::popcount(mask));

        for (uint32_t i : BitIter(mask)) {
            *sector->GetBrick(i, true) = *workSector.GetBrick(i);
        }
        _queue->OfferResult(pos, std::move(sector));
    }
    Log("Worker exit");
}
