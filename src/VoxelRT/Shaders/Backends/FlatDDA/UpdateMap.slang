import Map;
import "../BackendCommon";

using namespace backend::flat_dda;

struct BrickUpdateRecord {
    int3 BrickPos;
    uint64_t Data[BRICK_STRIDE / 64];
};
struct DispatchParams {
    uint NumRecords;
    BrickUpdateRecord* Records;
    VoxelMap Map;
};
[vk::push_constant] DispatchParams pc;

[numthreads(64)]
void ComputeMain(uint idx: SV_DispatchThreadID) {
    if (idx >= pc.NumRecords) return;

    BrickUpdateRecord rec = pc.Records[idx];
    uint brickIdx = VoxelMap.GetBrickIndex(rec.BrickPos);

    uint64_t* destMasks = &pc.Map.Storage.VoxelMasks[brickIdx * (BRICK_STRIDE / 64)];
    bool nonEmpty = false;
    
    for (uint i = 0; i < 8; i++) {
        destMasks[i] = rec.Data[i];
        nonEmpty |= rec.Data[i] != 0;
    }

    InterlockedSetBit(pc.Map.Storage.BrickMasks[brickIdx / 64u], brickIdx & 63, nonEmpty);
}
