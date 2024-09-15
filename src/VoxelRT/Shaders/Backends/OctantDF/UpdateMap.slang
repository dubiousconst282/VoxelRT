import Havk.Core;
import Map;
import "../BackendCommon";

using namespace backend::octdf;

struct DispatchParams {
    uint Axis;
    FieldTile* Tile;
}
[vk::push_constant] DispatchParams pc;

[numthreads(8, 8, 1)]
void ComputeMain(uint3 tid: SV_DispatchThreadID) {
    let tile = pc.Tile;

    if (pc.Axis == 0) InitRow(tile, tid.xy);

    if (pc.Axis == 0) PropagateAxis(tile, int3(0, tid.xy).xyz);
    if (pc.Axis == 1) PropagateAxis(tile, int3(0, tid.xy).yxz);
    if (pc.Axis == 2) PropagateAxis(tile, int3(0, tid.xy).zyx);
}

void PropagateAxis(FieldTile* tile, int3 offset) {
    int3 axisDelta = (1 << pc.Axis) >> int3(0, 1, 2) & 1;

    for (int oct = 0; oct < 8; oct++) {
        uint8_t* field = &tile.Dists[oct][0];

        // oct = (dir.x < 0 ? 0 : 1) + (dir.y < 0 ? 0 : 2) + (dir.z < 0 ? 0 : 4);
        if ((oct >> pc.Axis & 1) == 0) {
            for (int i = 1; i < FIELD_SIZE; i++) {
                int3 pos = offset + i * axisDelta;

                uint d1 = field[GetSampleIndex(pos)];
                uint d2 = field[GetSampleIndex(pos - axisDelta)];
                field[GetSampleIndex(pos)] = uint8_t(min(min(d1, d2 + 1), 255));
            }
        } else {
            for (int i = FIELD_SIZE - 2; i >= 0; i--) {
                int3 pos = offset + i * axisDelta;

                uint d1 = field[GetSampleIndex(pos)];
                uint d2 = field[GetSampleIndex(pos + axisDelta)];
                field[GetSampleIndex(pos)] = uint8_t(min(min(d1, d2 + 1), 255));
            }
        }
    }
}

void InitRow(FieldTile* tile, uint2 column) {
    for (int i = 0; i < FIELD_SIZE; i++) {
        uint3 pos = uint3(i, column);

        static_assert(FIELD_RES == 4, "TODO");

        uint64_t mask = tile->OccMasks[GetMaskIndex(pos * FIELD_RES)];
        uint sampleIdx = GetSampleIndex(pos);

        for (int oct = 0; oct < 8; oct++) {
            tile->Dists[oct][sampleIdx] = mask != 0 ? 0 : 255;
        }
    }
}
//uint3x2 SampleSubDist(uint3 pos) {
//    uint64_t mask = tile->OccMasks[GetMaskIndex(pos * FIELD_RES)];
//}