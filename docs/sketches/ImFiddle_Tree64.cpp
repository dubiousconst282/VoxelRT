#include "../Canvas.h"

#include <bit>

const int CANVAS_SIZE = 768, GRID_SIZE = 64, CELL_SIZE = CANVAS_SIZE / GRID_SIZE;


#if 0
static int MaskDDA_orig(uint64_t mask, float3 currPos, float3 rayDir) {
    float3 invDir = 1.0f / rayDir;
    float3 sideDist = (step(0.0f, rayDir) - fract(currPos)) * invDir;
    int3 mapPos = int3(floor(currPos));

    for (int i = 0; i < 12; i++) {
        if (uint(mapPos.x | mapPos.y | mapPos.z) >= 4) break;

        int currIdx = mapPos.x + mapPos.z*4 +mapPos.y*16;
        if ((mask >> currIdx & 1) != 0) {
            return currIdx;
        }

        if (sideDist.x < sideDist.y && sideDist.x < sideDist.z) {
            sideDist.x += abs(invDir.x);
            mapPos.x += invDir.x < 0 ? -1 : +1;
        } else if (sideDist.y < sideDist.z) {
            sideDist.y += abs(invDir.y);
            mapPos.y += invDir.y < 0 ? -1 : +1;
        } else {
            sideDist.z += abs(invDir.z);
            mapPos.z += invDir.z < 0 ? -1 : +1;
        }
    }
    return -1;
}

static int MaskDDA_PackedIdx(uint64_t mask, float3 currPos, float3 rayDir) {
    float3 invDir = 1.0f / rayDir;
    float3 sideDist = (step(0.0f, rayDir) - fract(currPos)) * invDir;
    int3 mapPos = int3(floor(currPos));

    int currIdx = mapPos.x + mapPos.z * 4 + mapPos.y * 16;
    int signMask = 0;
    if (rayDir.x < 0) signMask |= 0b11 << 0;
    if (rayDir.y < 0) signMask |= 0b11 << 4;
    if (rayDir.z < 0) signMask |= 0b11 << 2;

    // DDA takes at worst N*3-1 iterations to traverse a N³ grid
    for (int i = 0; i < 11; i++) {
        SetFillColor(0.3, 0.5, 1.0, 0.7);
        SetStrokeColor(0, 0, 0);

        float2 rpos = float2(currIdx >> 0 & 3, currIdx >> 4 & 3);
        Rect(rpos, float2(1));

        // TODO: can maybe do 2D dda and then test span of rows like RLE/voxlap?
        if ((mask >> currIdx & 1) != 0) {
            return currIdx;
        }
        int stepMask;

        if (sideDist.x < sideDist.y && sideDist.x < sideDist.z) {
            sideDist.x += abs(invDir.x);
            stepMask = 1;
        } else if (sideDist.y < sideDist.z) {
            sideDist.y += abs(invDir.y);
            stepMask = 16;
        } else {
            sideDist.z += abs(invDir.z);
            stepMask = 4;
        }
        int prevIdx = currIdx;
        currIdx += (stepMask ^ signMask) - signMask;

        // bounds check: stop if increment flips any bits outside axis mask
        //if (((prevIdx ^ currIdx) & (~0b11 << stepLog)) != 0) break;
        //if ((uint(prevIdx ^ currIdx) >> stepLog) >= 4) break;
        //if (uint(prevIdx ^ currIdx) >= (4 << stepLog)) break;
        if (uint(prevIdx ^ currIdx) >= (stepMask << 2)) break;
    }
    return -1;
}

void Draw_MaskDDA(float3 origin, float3 dir) {
    Save();
    Scale(CELL_SIZE);

    SetFillColor(1.0, 1.0, 1.0);
    Rect(0, 0, 4, 4);

    SetFillColor(0.7, 0.7, 0.7);

    for (uint32_t i = 0; i < 4; i++) {
        Line(float2(0, i), float2(4, i), 2);
        Line(float2(i, 0), float2(i, 4), 2);
    }

    static uint64_t mask = 0;
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        auto pos = GetMousePos();

        if (pos.x >= 0 && pos.x < 4 && pos.y >= 0 && pos.y < 4) {
            mask ^= 1ull << (int(pos.x) + int(pos.y) * 16);
        }
    }

    SetFillColor(1, 0.3, 0.3);
    for (uint64_t m = mask; m != 0; m &= m - 1) {
        int i = std::countr_zero(m);
        Rect(i & 3, i >> 4 & 3, 1, 1);
    }
    MaskDDA_PackedIdx(mask, origin, dir);
    Restore();
}
#endif

struct [[gnu::packed]] Node {
    uint32_t IsLeaf : 1 = 0;
    uint32_t ChildPtr : 31 = 0;
    uint64_t PopMask = 0;
};
std::vector<Node> g_TreeNodes = { { .IsLeaf = 1, .PopMask = 1ull << (2 + 3 * 16) }};

int GetNodeCellIndex(float3 pos, int scale_exp) {
    int3 cellPos = std::bit_cast<int3>(pos) >> scale_exp & 3;
    return cellPos.x + cellPos.z * 4 + cellPos.y * 16;
}
// RoundToCellScale(pos, scale * 4.0) + UnpackIndex(cellIdx) * scale
float3 GetPosAtCellIndex(float3 pos, int cellIdx, int scale_exp) {
    int3 cellPos = cellIdx >> int3(0, 4, 2) & 3;
    return std::bit_cast<float3>((std::bit_cast<int3>(pos) & (~3 << scale_exp)) | cellPos << scale_exp);
}
// floor(pos / scale) * scale
float3 FloorToCellScale(float3 pos, int scale_exp) {
    int mask = ~0 << scale_exp;
    return std::bit_cast<float3>(std::bit_cast<int3>(pos) & mask);
}
void Traversal(float3 origin, float3 dir) {
    Save();
    Scale(CANVAS_SIZE);

    Translate(-1, -1);

    SetFillColor(0, 0, 0);
    Text(GRID_SIZE / 2.0f, 0.1, "Origin: %.4f %.4f", origin.x, origin.y);

    uint stack[11];
    int scale_exp = 21;  // 0.25

    Node* node = &g_TreeNodes.back();
    stack[scale_exp >> 1] = node - &g_TreeNodes.front();

    if (abs(dir.x) < 0.0001) dir.x = 0.0001;
    if (abs(dir.y) < 0.0001) dir.y = 0.0001;
    if (abs(dir.z) < 0.0001) dir.z = 0.0001;

    int signMask = 0, stepIdx = 0;
    if (dir.x < 0) signMask |= 0b11 << 0;
    if (dir.y < 0) signMask |= 0b11 << 4;
    if (dir.z < 0) signMask |= 0b11 << 2;

    float3 invDir = 1.0f / dir;
    float3 pos = FloorToCellScale(origin, scale_exp);

    // Ensure start pos is inside the grid
    // This is not correct but prevents the traversal from completely breaking down.
    pos = clamp(pos, 1.0f, 1.9999999f);

    float tmin;

    for (int j = 0; j < 32; j++) {
        float scale = std::bit_cast<float>((scale_exp - 23 + 127) << 23);  // 2 ^ -(23 - scale_exp)
        float3 sideDist = (step(0.0f, dir) * scale + (pos - origin)) * invDir;

        int currIdx = GetNodeCellIndex(pos, scale_exp);
        uint64_t mask = node->PopMask;

        // DDA takes at worst N*3-1 iterations to traverse a N³ grid
        for (int i = 0; i < 11; i++) {
            SetFillColor(0.3, 0.9, 0.5, 0.5);
            SetStrokeColor(0, 0, 0);

            float3 currPos = GetPosAtCellIndex(pos, currIdx, scale_exp);
            Rect(currPos, float2(scale));
            SetFillColor(0, 0, 0);
            //Text(currPos, "%d  %d %d", i, currIdx & 3, currIdx>>4&3);

            float tmin = min(min(sideDist.x, sideDist.y), sideDist.z);
            SetFillColor(1.0f, 0.2f, 0.2f);
            Circle(origin + tmin * dir, 0.005f);

            // TODO: could we do 2D DDA and then test full rows somehow? maybe tzcnt loop or whatever voxlap does?
            //       would reduce max iters to 7 but def increase complexity, so maybe not worth at all
            if ((mask >> currIdx & 1) != 0) break;

            if (sideDist.x < sideDist.y && sideDist.x < sideDist.z) {
                sideDist.x += abs(invDir.x) * scale;
                stepIdx = 1;
            } else if (sideDist.y < sideDist.z) {
                sideDist.y += abs(invDir.y) * scale;
                stepIdx = 16;
            } else {
                sideDist.z += abs(invDir.z) * scale;
                stepIdx = 4;
            }
            // Step position in selected axis, same as:
            //   currIdx += (dir[stepAxis] < 0 ? -stepIdx : +stepIdx)
            int prevIdx = currIdx;
            currIdx += (stepIdx ^ signMask) - signMask;

            // Bounds check: stop if increment flips any bit outside axis mask
            if (uint(prevIdx ^ currIdx) >= uint(stepIdx << 2)) { currIdx = -1; break; }
            //if ((uint(prevIdx ^ currIdx) & ~(stepIdx | (stepIdx<<1))) != 0) { currIdx = -1; break; }
        }

        // Undo last step
        if (stepIdx == 1) {
            sideDist.x -= abs(invDir.x) * scale;
        } else if (stepIdx == 16) {
            sideDist.y -= abs(invDir.y) * scale;
        } else if (stepIdx == 4) {
            sideDist.z -= abs(invDir.z) * scale;
        } else {
            sideDist.x = 0;
        }

        // Compute hit pos
        tmin = min(min(sideDist.x, sideDist.y), sideDist.z);
        float3 hitPos = origin + tmin * dir;

        // Descend if we hit any child
        if (currIdx >= 0) {
            int3 childPos = currIdx >> int3(0, 4, 2) & 3;
            childPos = (std::bit_cast<int3>(pos) & (~3 << scale_exp)) | childPos << scale_exp;

            if (node->IsLeaf) {
                SetFillColor(1, 0, 0.2, 0.7);
                Rect(std::bit_cast<float3>(childPos), float2(scale));
                break;
            }
            // Clamp hit pos to be inside child bounding box to workaround float precision errors
            int child_scale_exp = scale_exp - 2;
            int3 subPos = clamp(std::bit_cast<int3>(hitPos), childPos, childPos | (3 << child_scale_exp));
            subPos &= ~0u << child_scale_exp; // floor

            // TODO: can we figure out `childPos + subPos` more efficiently?
            pos = std::bit_cast<float3>(subPos);
          //  Rect(pos, float2(scale*0.25));

            stack[scale_exp >> 1] = node - &g_TreeNodes.front(); // push
            scale_exp -= 2;

            int childIdx = std::popcount(node->PopMask & ((1ull << currIdx) - 1));
            node = &g_TreeNodes[node->ChildPtr + childIdx];

          // SetFillColor(1, 0, 0.2, 0.7);
          // Circle(hitPos, 0.015);
            SetFillColor(0, 0, 0);
            Text(pos, "Descend: %d %d", currIdx & 3, currIdx >> 4 & 3);
        } else {
            uint3 parentPos = std::bit_cast<uint3>(pos);

            int parent_scale_exp = scale_exp + 2;
            parentPos &= ~0u << parent_scale_exp;

            int3 delta = int3(0);
            if (stepIdx == 1) delta.x = (dir.x < 0 ? -1 : +1);
            if (stepIdx == 4) delta.z = (dir.z < 0 ? -1 : +1);
            if (stepIdx == 16) delta.y = (dir.y < 0 ? -1 : +1);

            uint diff;

            if (stepIdx == 1) diff = parentPos.x ^ (parentPos.x + (delta.x << parent_scale_exp));
            if (stepIdx == 4) diff = parentPos.z ^ (parentPos.z + (delta.z << parent_scale_exp));
            if (stepIdx == 16) diff = parentPos.y ^ (parentPos.y + (delta.y << parent_scale_exp));

            // If any exponent changes, stop because we are going outside of root
            if ((diff & 0x3F800000) != 0) break;

            scale_exp = 31 - std::countl_zero(diff);
            if (scale_exp % 2 == 0) scale_exp--;  // dunno why diff gets misaligned, but this seems to work

            parentPos &= ~0u << scale_exp;
            parentPos += delta<<scale_exp;
            pos = std::bit_cast<float3>(parentPos);

            SetFillColor(1,  0, 0);
            Rect(pos,float2(scale));

            node = &g_TreeNodes[stack[scale_exp >> 1]];


            SetFillColor(0, 0, 0);
            Text(pos, "Ascend  %d (p=%d)", scale_exp, parent_scale_exp);
        }
    }

    SetFillColor(1.0f, 1.0f, 0.2f);
    Circle(origin + tmin * dir, 0.01f);

    Restore();
}

static Node GenerateTree(bool grid[GRID_SIZE][GRID_SIZE], std::vector<Node>& data, uint32_t scale, glm::uvec3 pos = {}) {
    assert(scale % 2 == 0 && scale >= 2);
    Node node;

    // Create leaf
    if (scale == 2) {
        node.IsLeaf = true;
        node.PopMask = 0;

        for (uint32_t i = 0; i < 64; i++) {
            glm::uvec3 subPos = pos + (i >> glm::uvec3(0, 4, 2) & 3u);
            if(subPos.z==0)
            node.PopMask |= uint64_t(grid[subPos.x][subPos.y]) << i;
        }
        return node;
    }

    // Descend
    Node child[64];
    uint64_t childLeafMask = 0;

    scale -= 2;

    for (uint32_t i = 0; i < 64; i++) {
        glm::uvec3 childPos = i >> glm::uvec3(0, 4, 2) & 3u;
        child[i] = GenerateTree(grid, data, scale, pos + (childPos << scale));

        if (child[i].PopMask != 0) {
            node.PopMask |= 1ull << i;
        }
        if (child[i].IsLeaf && child[i].PopMask == ~0ull) {
            childLeafMask |= 1ull << i;
        }
    }

    // If all children are fully populated leafs, prune and make this node a full leaf too
    /*if (childLeafMask == ~0ull) {
        node.PopMask = ~0ull;
        node.IsLeaf = true;
        return node;
    }*/

    node.ChildPtr = data.size();

    // Encode children
    for (uint32_t i = 0; i < 64; i++) {
        if (node.PopMask >> i & 1) {
            data.push_back(child[i]);
        }
    }
    return node;
}

void DrawGrid() {
    Save();
    Scale(CELL_SIZE);

    struct GridData {
        bool Cells[GRID_SIZE][GRID_SIZE];
    };
    auto& grid = GetPersistData<GridData>().Cells;

    if (ImGui::IsKeyDown(ImGuiKey_ModCtrl)) {
        uint2 pos = floor(GetMousePos());

        if (pos.x < GRID_SIZE && pos.y < GRID_SIZE) {
            static bool fill;

            if (ImGui::IsKeyPressed(ImGuiKey_ModCtrl, false)) {
                fill = !grid[pos.x][pos.y];
            }
            grid[pos.x][pos.y] = fill;
        }
    }

    g_TreeNodes.clear();
    g_TreeNodes.push_back(GenerateTree(grid, g_TreeNodes, (int)log2f(GRID_SIZE)));

    SetFillColor(0, 0, 0);
    Text(0, -1.5, "Nodes: %zu", g_TreeNodes.size());

    for (int y = 0; y < GRID_SIZE; y++) {
        for (int x = 0; x < GRID_SIZE; x++) {
            if (grid[x][y]) {
                SetFillColor(0.2, 0.5, 1.0);
                Rect(x, y, 1, 1);
            }
        }
    }

    // Grid lines
    for (int i = 1; i < GRID_SIZE; i++) {
        float thickness = i % 4 == 0 ? 1.5 : 1;
        SetFillColor(0, 0, 0, i % 16 == 0 ? 0.7 : 0.4);
        Line(float2(i, 0), float2(i, GRID_SIZE), thickness);
        Line(float2(0, i), float2(GRID_SIZE, i), thickness);
    }

    Restore();
}

void Paint() {
    Save();
    Translate(50, 50);


    static float2 startPos = float2(260, 540);
    static float2 endPos = float2(650, 330);
    // endPos = float2(425, 529);
    // endPos = float2(852, 381);

    // startPos=float2(67,130),endPos = float2(701, 550);
    //startPos = float2(7, 313), endPos = float2(597, 439);

    if (IsMouseDragging()) {
        auto mousePos = GetMousePos();
        if (distance(mousePos, startPos) < distance(mousePos, endPos)) {
            startPos = mousePos;
        } else {
            endPos = mousePos;
        }
    }

    DrawGrid();

    Traversal(float3(startPos / float(CANVAS_SIZE), 0) + 1.0f, float3(normalize((endPos - startPos) / float(CANVAS_SIZE)), 0));
    //Draw_MaskDDA(float3(startPos / float(CELL_SIZE), 0), normalize(float3(endPos - startPos, 0) / float(CELL_SIZE)));


    SetFillColor(0.0, 0.0, 0.0);
    Line(startPos, endPos);

    SetFillColor(0.0, 1.0, 0.0);
    Circle(startPos, 5.0f);

    Restore();
}
