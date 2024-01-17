#include <memory>
#include <vector>
#include <set>

// Simple octree color quantizer
// https://www.cubic.org/docs/octree.htm
// http://www.leptonica.org/papers/colorquant.pdf
struct PaletteBuilder {
    void AddColor(uint32_t color) { RootNode.Add(color); }

    std::vector<uint32_t> Build(uint32_t maxCount) {
        const auto NodeComparer = [](Node* a, Node* b) { return a->Count < b->Count || (a->Count == b->Count && a > b); };
        std::set<Node*, decltype(NodeComparer)> leafs;

        uint32_t numLeafs = 0;
        RootNode.FindLeafs([&](Node* node) {
            leafs.insert(node->Parent);
            numLeafs++;
        });

        // Merge leafs until the requested limit
        // (This seems to be a quite dumb way to reduce nodes, maybe faster and simpler
        //  to just pick the N most populated nodes and merge the remaining children,
        //  but I'm not sure if that would have an affect in the final quality.)
        while (numLeafs > maxCount) {
            Node* node = *leafs.begin();
            assert(!node->IsLeaf());

            node->ReduceChildren([&](Node* child) {
                if (child->IsLeaf()) {
                    numLeafs--;
                } else {
                    leafs.erase(child);
                }
            });
            numLeafs++;  // node turns into a leaf after Reduce()
            leafs.erase(node);
            leafs.insert(node->Parent);
        }

        // Build palette colors
        std::vector<uint32_t> colors;
        RootNode.FindLeafs([&](Node* node) {
            uint32_t r = node->RgbSum[0] / node->Count;
            uint32_t g = node->RgbSum[1] / node->Count;
            uint32_t b = node->RgbSum[2] / node->Count;

            node->Count = colors.size();
            colors.push_back(r << 0 | g << 8 | b << 16);
        });
        return colors;
    }

    uint32_t FindIndex(uint32_t color) const { return RootNode.Find(color).Count; }

private:
    struct Node {
        static const uint32_t MaxLevels = 6;

        std::unique_ptr<Node> Children[8];
        Node* Parent = nullptr;
        uint32_t RgbSum[3]{};   // Sum of added colors
        uint32_t Count = 0;     // Number of colors added across all parent paths. After Build(), contains the palette entry index.

        // Get child node index from a packed RGB888 color at the given level.
        static uint32_t GetChildIndex(uint32_t color, uint32_t level) {
            return (color >> (7 - level) & 1) << 0 |
                   (color >> (15 - level) & 1) << 1 |
                   (color >> (23 - level) & 1) << 2;
        }

        void Add(uint32_t color, uint32_t level = 0) {
            Count++;

            if (level >= MaxLevels) {
                RgbSum[0] += (color >> 0) & 255;
                RgbSum[1] += (color >> 8) & 255;
                RgbSum[2] += (color >> 16) & 255;
                return;
            }
            auto& child = Children[GetChildIndex(color, level)];

            if (child == nullptr) {
                child = std::make_unique<Node>();
                child->Parent = this;
            }
            child->Add(color, level + 1);
        }
        const Node& Find(uint32_t color, uint32_t level = 0) const {
            auto& child = Children[GetChildIndex(color, level)];
            return child == nullptr ? *this : child->Find(color, level + 1);
        }

        void FindLeafs(const std::function<void(Node*)>& visitor) {
            bool isLeaf = true;

            for (auto& child : Children) {
                if (child != nullptr) {
                    child->FindLeafs(visitor);
                    isLeaf = false;
                }
            }
            if (isLeaf) {
                visitor(this);
            }
        }

        void ReduceChildren(const std::function<void(Node*)>& visitChild) {
            for (auto& child : Children) {
                if (child == nullptr) continue;

                visitChild(child.get());

                if (!child->IsLeaf()) {
                    child->ReduceChildren(visitChild);
                }
                for (uint32_t j = 0; j < 3; j++) {
                    RgbSum[j] += child->RgbSum[j];
                }
                child = nullptr;
            }
        }

        bool IsLeaf() {
            for (auto& child : Children) {
                if (child != nullptr) {
                    return false;
                }
            }
            return true;
        }
    };

    Node RootNode;
};

/*
struct PaletteBuilder {
    uint16_t NumColors;     // Final palette size
    uint8_t PaletteR[256];
    uint8_t PaletteG[256];
    uint8_t PaletteB[256];

    uint32_t FindIndex(uint32_t color) const {
        auto targetR = _mm512_set1_epi16((color >> 0) & 255);
        auto targetG = _mm512_set1_epi16((color >> 8) & 255);
        auto targetB = _mm512_set1_epi16((color >> 16) & 255);

        auto closestDist = _mm512_set1_epi16((int16_t)65535);
        uint32_t closestIdx = 0;

        for (uint32_t i = 0; i < NumColors; i += 32) {
            auto deltaR = _mm512_sub_epi16(targetR, _mm512_cvtepi8_epi16(_mm256_loadu_epi8(&PaletteR[i])));
            auto deltaG = _mm512_sub_epi16(targetG, _mm512_cvtepi8_epi16(_mm256_loadu_epi8(&PaletteG[i])));
            auto deltaB = _mm512_sub_epi16(targetB, _mm512_cvtepi8_epi16(_mm256_loadu_epi8(&PaletteB[i])));

            // TODO: check if manhattan dist is too much worse than euclid
            auto dist = _mm512_abs_epi16(deltaR);
            dist = _mm512_add_epi16(dist, _mm512_abs_epi16(deltaG));
            dist = _mm512_add_epi16(dist, _mm512_abs_epi16(deltaB));

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

        minValue = _mm512_broadcastw_epi16(_mm_srli_epi32(c, 16));
        minIndex = (uint32_t)std::countr_zero(_mm512_cmpeq_epu16_mask(x, _mm512_broadcastw_epi16(c)));
    }
};
*/