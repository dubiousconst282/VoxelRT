import Havk.Core;
import Map;
import "../BackendCommon";

using namespace backend::distfield;

static const int BRICK_SIZE = 8, BRICK_STRIDE = BRICK_SIZE*BRICK_SIZE*BRICK_SIZE;

struct TileUpdateRecord {
    uint3 WorldPos;
    uint64_t OccMask[TILE_STRIDE / BRICK_STRIDE][BRICK_STRIDE / 64];
};
struct DispatchParams {
    ImageHandle3D<uint> DistField;
    uint16_t NumTiles;
    uint16_t Axis;
    TileUpdateRecord* Tiles;
}
[vk::push_constant] DispatchParams pc;


#ifndef BACKEND_ID
    #define BACKEND_ID 4 // silence warning
#endif

#if BACKEND_ID == 4
    #define PropagateAxis PropagateAxis_Manhattan
    #define MAX_DIST 255
#elif BACKEND_ID == 5
    #define PropagateAxis PropagateAxis_EDT
    #define MAX_DIST 65535
#endif

[numthreads(8, 8, 1)]
void ComputeMain(uint3 tid: SV_DispatchThreadID) {
    if (tid.z >= pc.NumTiles) return;

    let record = &pc.Tiles[tid.z];

    if (pc.Axis == 0) InitRow(record, tid.xy);

    if (pc.Axis == 0) PropagateAxis(record->WorldPos + int3(0, tid.xy).xyz, int3(1, 0, 0));
    if (pc.Axis == 1) PropagateAxis(record->WorldPos + int3(0, tid.xy).yxz, int3(0, 1, 0));
    if (pc.Axis == 2) PropagateAxis(record->WorldPos + int3(0, tid.xy).zyx, int3(0, 0, 1));
}

void PropagateAxis_Manhattan(int3 offset, int3 axisDelta) {
    let field = pc.DistField.Instance;

    for (int i = 1; i < TILE_SIZE; i++) {
        int3 pos = offset + i * axisDelta;
        uint d1 = field[pos];
        uint d2 = field[pos - axisDelta];
        field[pos] = min(min(d1, d2 + 1), 255);
    }
    for (int i = TILE_SIZE - 2; i >= 0; i--) {
        int3 pos = offset + i * axisDelta;
        uint d1 = field[pos];
        uint d2 = field[pos + axisDelta];
        field[pos] = min(min(d1, d2 + 1), 255);
    }
}

// I copied this code and do not quite understand how it works:
// https://acko.net/blog/subpixel-distance-transform/
// https://cs.brown.edu/people/pfelzens/papers/dt-final.pdf
void PropagateAxis_EDT(int3 offset, int3 axisDelta) {
    const int N = TILE_SIZE;
    // This should not be too big otherwise intermediate values will overflow
    const int INF = N * N * 3;

    let field = pc.DistField.Instance;

    int v[N + 1], z[N + 1], f[N + 1];
    v[0] = 0;
    z[0] = -INF;
    z[1] = +INF;
    f[0] = field[offset + 0 * axisDelta];

    int k = 0;
    for (int q = 1; q < N; q++) {
        f[q] = field[offset + q * axisDelta];

        int s;
        do {
            int r = v[k];
            s = (f[q] - f[r] + q * q - r * r) / (q - r) / 2;
        } while (s <= z[k] && --k >= 0);

        k++;
        v[k] = q;
        z[k] = s;
        z[k + 1] = INF;
    }

    k = 0;
    for (int q = 0; q < N; q++) {
        while (z[k + 1] < q) k++;
        int r = v[k];
        int d = q - r;
        field[offset + q * axisDelta] = f[r] + d * d;
    }
}

void InitRow(TileUpdateRecord* tile, uint2 column) {
    static_assert(TILE_SIZE % 8 == 0, "Tile size must be a multiple of 8");
    
    let field = pc.DistField.Instance;

    for (int i = 0; i < TILE_SIZE; i += 8) {
        int3 pos = tile->WorldPos + int3(i, column);
        uint brickIdx = GetLinearIndex(pos / BRICK_SIZE, TILE_SIZE/BRICK_SIZE, TILE_SIZE/BRICK_SIZE);
        uint cellIdx = GetLinearIndex(pos, BRICK_SIZE, BRICK_SIZE);
        uint mask = uint(tile->OccMask[brickIdx][cellIdx / 64] >> (cellIdx & 63));
        
        for (int j = 0; j < 8; j++, pos.x++) {
            bool occupied = (mask >> j & 1) != 0;
            field[pos] = occupied ? 0 : MAX_DIST;
        }
    }
}
