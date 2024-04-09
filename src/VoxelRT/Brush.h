#pragma once

#include "VoxelMap.h"

enum class BrushAction {
    Fill,
    Replace
};
struct BrushParams {
    BrushAction Action = BrushAction::Replace;
    float Radius = 30;
    float Probability = 1.0;  // random fill probability
    uint32_t RandomSeed = 1234;

    glm::ivec3 PointA, PointB;
    Voxel Material = { 255 };
};

struct BrushSession {
    BrushParams Pars;
    uint32_t FrameNo = 0;
    double PrevHitDist = 0;

    void UpdatePosFromRay(VoxelMap& map, glm::dvec3 origin, glm::dvec3 dir);

    void UpdatePos(glm::ivec3 pos) {
        if (FrameNo == 0) {
            Pars.PointB = pos;
        }
        Pars.PointA = Pars.PointB;
        Pars.PointB = pos;
        FrameNo++;
    }
    void Reset() {
        FrameNo = 0;
        PrevHitDist = 0;
    }

    void Dispatch(VoxelMap& map);
};