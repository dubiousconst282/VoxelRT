#include "../Canvas.h"

#include <bit>
#include <unordered_set>

const int CANVAS_SIZE = 768, GRID_SIZE = 16, CELL_SIZE = CANVAS_SIZE / GRID_SIZE;


struct [[gnu::packed]] Node {
    uint32_t IsLeaf : 1 = 0;
    uint32_t ChildPtr : 31 = 0;
    uint64_t PopMask = 0;

    glm::int3 GridPos; // debug
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
float3 FloorToCellScale(float3 pos, int3 scale_exp) {
    int3 mask = ~0 << scale_exp;
    return std::bit_cast<float3>(std::bit_cast<int3>(pos) & mask);
}
Node* GetNodeAt(float3 pos, int& scale_exp) {
    Node* node = &g_TreeNodes.back();
    scale_exp = 21;

    while (true) {
        int childIdx = GetNodeCellIndex(pos, scale_exp);

        if (node->IsLeaf || (node->PopMask >> childIdx & 1) == 0) return node;

        int slotIdx = std::popcount(node->PopMask & ((1ull << childIdx) - 1));
        node = &g_TreeNodes[node->ChildPtr + slotIdx];
        scale_exp -= 2;
    }
    return node;
}

void Traversal(float3 origin, float3 dir) {
    Save();
    Scale(CANVAS_SIZE);

    Translate(-1, -1);

    SetFillColor(0, 0, 0);
    Text(GRID_SIZE / 2.0f, 0.1, "Origin: %.4f %.4f", origin.x, origin.y);

    Node* stack[11];
    int scaleExp = 21;  // 0.25

    Node* node = &g_TreeNodes.back();
    stack[scaleExp >> 1] = node;

    if (abs(dir.x) < 0.0001) dir.x = 0.0001;
    if (abs(dir.y) < 0.0001) dir.y = 0.0001;
    if (abs(dir.z) < 0.0001) dir.z = 0.0001;

    float3 invDir = 1.0f / dir;
    float3 pos = origin;

    // Ensure start pos is inside the grid
    // This is not correct but prevents the traversal from completely breaking down.
   // pos = clamp(pos, 1.0f, 1.9999999f);

    float tmin = 0;

    for (int j = 0; j < 16; j++) {
        int childIdx = GetNodeCellIndex(pos, scaleExp);

        // Descend
        while (scaleExp >= 2) {
            if (node->IsLeaf || (node->PopMask >> childIdx & 1) == 0) break;

            stack[scaleExp >> 1] = node;

            int slotIdx = std::popcount(node->PopMask & ((1ull << childIdx) - 1));
            node = &g_TreeNodes[node->ChildPtr + slotIdx];

            scaleExp -= 2;
            childIdx = GetNodeCellIndex(pos, scaleExp);
        }
        if (node->IsLeaf && (node->PopMask >> childIdx & 1) != 0) break;

        int3 adv_scale = int3(scaleExp);

        // 2-voxel anisotropic steps
        if ((node->PopMask >> (childIdx & 0b101010) & 0x00330033) == 0) adv_scale++;
        else if ((node->PopMask >> (childIdx & 0b111110) & 0x00000003) == 0) adv_scale.x++;
        else if ((node->PopMask >> (childIdx & 0b101111) & 0x00000011) == 0) adv_scale.y++;
        else if ((node->PopMask >> (childIdx & 0b101011) & 0x00010001) == 0) adv_scale.z++;

        // Compute next hit pos
        float3 scale = std::bit_cast<float3>((adv_scale - 23 + 127) << 23);  // 2 ^ -(23 - adv_scale)

        // TODO: floor and step(dir) can be combined into a single step
        pos = FloorToCellScale(pos, adv_scale);
        float3 sideDist = (step(0.0f, dir) * scale + (pos - origin)) * invDir;

        SetStrokeColor(0, 0, 0);
        SetFillColor(0.3, 0.9, 0.5, 0.5);
        Rect(pos, float2(scale));

        SetFillColor(0, 0, 0);
        Text(pos, "%d", j, scaleExp);

        SetFillColor(1.0f, 0.2f, 0.2f);
        //Circle(origin + tmin * dir, 0.003f);

        tmin = min(min(sideDist.x, sideDist.y), sideDist.z)+0.00001;

        float3 prevPos = pos;
        float3 hitPos = origin + tmin * dir;


        pos = hitPos;

        SetFillColor(1.0f, 0.2, 1.0);
        Circle(pos, 0.004f);

        // Find carry bit to tell how far we need to ascend the tree
        uint3 diffPos = std::bit_cast<uint3>(pos) ^ std::bit_cast<uint3>(prevPos);
        int diffExp = 31 - std::countl_zero(diffPos.x | diffPos.y | diffPos.z);
        if (diffExp % 2 == 0) diffExp--;  // dunno why diff gets misaligned, but this seems to work

        if (diffExp > scaleExp) {
            if (diffExp > 21) break;  // going out of root?

            scaleExp = diffExp;
            node = stack[scaleExp >> 1];
        }
    }

    SetFillColor(1.0f, 1.0f, 0.2f);
    //Circle(origin + tmin * dir, 0.01f);

    Restore();
}

static Node GenerateTree(bool grid[GRID_SIZE][GRID_SIZE], std::vector<Node>& data, uint32_t scale, glm::uvec3 pos = {}) {
    assert(scale % 2 == 0 && scale >= 2);
    Node node;
    node.GridPos = pos;
    
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

   // startPos = float2(366, 196), endPos = float2(1031, 185);

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

    SetFillColor(0.0, 1.0, 0.0, 0.7);
    Circle(startPos, 5.0f);

    Restore();
}
