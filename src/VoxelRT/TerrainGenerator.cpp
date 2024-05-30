#include "TerrainGenerator.h"

#include <FastNoise/FastNoise.h>

// https://www.shadertoy.com/view/4tVcWR
// https://www.shadertoy.com/view/tdGGWV
static void TreeSDF(VFloat3 p, VInt& materialId) {
    // Tangent vectors for the branch local coordinate system.
    VFloat3 w = simd::normalize(VFloat3(-.8 + 0 * .01, 1.2, -1.));
    VFloat3 u = simd::normalize(simd::cross(w, VFloat3(0, 1, 0)));
    VFloat3 v = simd::normalize(simd::cross(u, w));

    float ti = 8.0;
    int j = int(glm::min(glm::floor(ti - 1.0), 7.0));

    float scale = glm::min(0.3 + ti / 6.0, 1.0);
    p /= scale;

    VFloat barkDist = 10000.0;
    float s = 1., ss = 1.6;

    // Evaluate the tree branches, which are just space-folded cylinders.
    for (int i = 0; i <= j; i++) {
        barkDist = simd::min(barkDist, scale * simd::max(p.y - 1.0, simd::max(-p.y, simd::approx_sqrt(p.x * p.x + p.z * p.z) - 0.1 / (p.y + 0.7))) / s);

        p.x = simd::abs(p.x);
        p.z = simd::abs(p.z);
        p.y -= 1.0;

        // Rotate in to the local space of a branch.
        p = VFloat3(p.x * u.x + p.y * u.y + p.z * u.z,  //
                    p.x * v.x + p.y * v.y + p.z * v.z,  //
                    p.x * w.x + p.y * w.y + p.z * w.z);
        // p *= mat3(u, simd::normalize(simd::cross(u, w)), w);

        p *= ss;
        s *= ss;
    }

    VFloat leafDist = simd::max(0.0f, simd::length(p) - 0.4) / s;
    VFloat dist = simd::min(barkDist, leafDist);

    materialId.set_if(dist < 1.0/128, simd::csel(leafDist < barkDist, 245, 241));
    // return min(d, col = max(0., length(p) - 0.25) / s);
}

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
            VInt grassId = 245 + (simd::trunc2i(noise * 1234.5678) & 3); // 4 random grass variants. no, it doesn't look good.
            p.VoxelIds = simd::csel(fillMask, grassId, 0);

            VFloat3 treePos = VFloat3(simd::conv2f(p.X - 256), simd::conv2f(p.Y - 112), simd::conv2f(p.Z - 256)) * (1/64.0) + 0.5;
            TreeSDF(treePos, p.VoxelIds);

            isNonEmpty |= simd::any(p.VoxelIds != 0);
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

#if 0
#include <format>
#include <iostream>

template<class... ArgTypes>
static void Log(const std::format_string<ArgTypes...> fmt, ArgTypes&&... args) {
    auto opaqueTid = std::this_thread::get_id();
    uint32_t tid = *(uint32_t*)&opaqueTid;

    std::string text = std::format("[TerrainGen T{}]: ", tid);
    std::format_to(std::back_inserter(text), fmt, args...);
    text += "\n";

    std::cout << text;
    std::cout.flush();
}
#else
static void Log(const char* fmt, ...) { }
#endif

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
    void PushResult(glm::ivec3 pos, std::unique_ptr<Sector> sector) {
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
        _queue->PushResult(pos, std::move(sector));
    }
    Log("Worker exit");
}
