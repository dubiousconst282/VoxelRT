#include <vector>
#include <set>
#include <span>
#include <functional>
#include "../SwRast/SIMD.h"

namespace glim {

// Simple octree color quantizer
// https://www.cubic.org/docs/octree.htm
// http://www.leptonica.org/papers/colorquant.pdf
struct PaletteBuilder {
    uint16_t NumColors = 0;  // Final palette size
    uint8_t ColorR[256];
    uint8_t ColorG[256];
    uint8_t ColorB[256];

    void AddColor(uint32_t color) {
        assert(NumColors == 0 && "Cannot add colors after Build()");
        Tree.Add(color);
    }

    void Build(uint32_t maxColors) {
        assert(NumColors == 0 && "Palette already built");
        assert(maxColors <= 256);

        const auto NodeComparer = [](Node* a, Node* b) { return a->Count < b->Count || (a->Count == b->Count && a > b); };
        std::set<Node*, decltype(NodeComparer)> leafs;

        uint32_t numLeafs = 0;
        Tree.FindLeafs([&](Node* node) {
            leafs.insert(Tree.GetParent(node));
            numLeafs++;
        });

        // Merge leafs until the requested limit
        // (This seems to be a quite dumb way to reduce nodes, maybe faster and simpler
        //  to just pick the N most populated nodes and merge the remaining children,
        //  but I'm not sure if that would have an affect in the final quality.)
        while (numLeafs > maxColors) {
            Node* node = *leafs.begin();
            leafs.erase(node);

            if (node->IsLeaf) continue; // this will happen after reducing nodes that have non-leaf children

            numLeafs -= Tree.Reduce(node);
            leafs.insert(Tree.GetParent(node));
        }

        // Build palette colors
        Tree.FindLeafs([&](Node* node) {
            uint32_t r = node->RgbSum[0] / node->Count;
            uint32_t g = node->RgbSum[1] / node->Count;
            uint32_t b = node->RgbSum[2] / node->Count;

            ColorR[NumColors] = r;
            ColorG[NumColors] = g;
            ColorB[NumColors] = b;
            NumColors++;
        });
        Tree.Storage = {}; // release unused memory
    }

    uint32_t FindIndex(uint32_t color) const {
        // Brute force search is a bit overkill since it would be easier to traverse the 
        // octree to find the color index, but there are some weird queries for colors
        // that were not originally added before Build() and consequently have no
        // corresponding leaf in the tree.
        //
        // This will actually result in slightly better quality because colors across
        // octree nodes are considered. It also only depends on the final palette, which
        // is tiny and cache friendly.

        auto targetR = _mm512_set1_epi16((color >> 0) & 255);
        auto targetG = _mm512_set1_epi16((color >> 8) & 255);
        auto targetB = _mm512_set1_epi16((color >> 16) & 255);

        auto closestDist = _mm512_set1_epi16(32767);
        uint32_t closestIdx = 0;

        for (uint32_t i = 0; i < NumColors; i += 32) {
            auto deltaR = _mm512_sub_epi16(targetR, _mm512_cvtepu8_epi16(_mm256_loadu_epi8(&ColorR[i])));
            auto deltaG = _mm512_sub_epi16(targetG, _mm512_cvtepu8_epi16(_mm256_loadu_epi8(&ColorG[i])));
            auto deltaB = _mm512_sub_epi16(targetB, _mm512_cvtepu8_epi16(_mm256_loadu_epi8(&ColorB[i])));

            // Manhattan dist appears to give identical results as euclidean
            auto dist = _mm512_add_epi16(_mm512_abs_epi16(deltaR), _mm512_abs_epi16(deltaG));
            dist = _mm512_add_epi16(dist, _mm512_abs_epi16(deltaB));

            // Mask off candidates for out-of-range entries with infinite dist
            if (NumColors - i < 32) {
                uint32_t mask = (1u << (NumColors - i)) - 1u;
                dist = _mm512_mask_mov_epi16(_mm512_set1_epi16(32767), mask, dist);
            }

            if (_mm512_cmplt_epu16_mask(dist, closestDist) != 0) {
                _mm512_minpos_epu16(dist, closestDist, closestIdx);
                closestIdx += i;
            }
        }
        return closestIdx;
    }

private:
    static void _mm512_minpos_epu16(__m512i x, __m512i& minValue, uint32_t& minIndex) {
        auto a = _mm256_min_epu16(_mm512_extracti32x8_epi32(x, 0), _mm512_extracti32x8_epi32(x, 1));
        auto b = _mm_min_epu16(_mm256_extracti128_si256(a, 0), _mm256_extracti128_si256(a, 1));
        auto c = _mm_minpos_epu16(b);

        minValue = _mm512_broadcastw_epi16(c);
        minIndex = (uint32_t)std::countr_zero(_mm512_cmpeq_epu16_mask(x, minValue));
    }

    struct Node {
        uint32_t RgbSum[3]{};     // Sum of added colors
        uint32_t Count : 31 = 0;  // Number of colors added across all children paths.
        bool IsLeaf : 1 = false;
    };
    struct Octree {
        static const uint32_t MaxLevels = 6;
        //   (root)
        //  1234'4567
        // 9.. 17.. 25.. ...
        std::vector<Node> Storage{ (1u << ((MaxLevels + 1) * 3)) + 1 };

        // Get child node index from a packed RGB888 color at the given level.
        static uint32_t GetChildIndex(uint32_t color, uint32_t level) {
            return (color >> (7 - level) & 1) << 0 | 
                   (color >> (15 - level) & 1) << 1 | 
                   (color >> (23 - level) & 1) << 2;
        }
        std::span<Node> GetChildren(Node* node) {
            assert(!node->IsLeaf);

            uint32_t index = node - Storage.data();
            return { &Storage[(index + 1) << 3], 8 };
        }
        Node* GetParent(Node* node) {
            assert(node != &Storage[0]); // Root

            uint32_t index = node - Storage.data();
            return &Storage[(index >> 3) - 1];
        }

        void Add(uint32_t color) {
            Node* node = &Storage[0];

            for (uint32_t level = 0; level < MaxLevels; level++) {
                node->Count++;
                node = &GetChildren(node)[GetChildIndex(color, level)];
            }
            node->Count++;
            node->IsLeaf = true;
            node->RgbSum[0] += (color >> 0) & 255;
            node->RgbSum[1] += (color >> 8) & 255;
            node->RgbSum[2] += (color >> 16) & 255;
        }

        void FindLeafs(const std::function<void(Node*)>& visitor, Node* node = nullptr) {
            if (node == nullptr) {
                node = &Storage[0];
            }
            if (node->IsLeaf) {
                visitor(node);
                return;
            }

            for (auto& child : GetChildren(node)) {
                if (child.Count > 0) {
                    FindLeafs(visitor, &child);
                }
            }
        }

        uint32_t Reduce(Node* node) {
            uint32_t numReduced = 0;

            for (auto& child : GetChildren(node)) {
                if (child.Count == 0) continue;

                if (!child.IsLeaf) {
                    Reduce(&child);
                }
                for (uint32_t j = 0; j < 3; j++) {
                    node->RgbSum[j] += child.RgbSum[j];
                }
                numReduced++;
            }
            node->IsLeaf = true;
            return numReduced - 1;
        }
    };

    Octree Tree;
};

}; // namespace glim