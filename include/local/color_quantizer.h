#pragma once
#include <vector>
#include <cstdint>

static inline void unpack_color(uint32_t color, unsigned char& r, unsigned char& g, unsigned char& b);

static inline uint32_t pack_color(unsigned char r, unsigned char g, unsigned char b);

static float color_distance(uint32_t c1, uint32_t c2);

// 支持多种量化算法，默认K-Means
class ColorQuantizer {
public:
    // 输入原始颜色，输出聚类中心
    static std::vector<uint32_t> kmeans(const std::vector<uint32_t>& colors, int k, int max_iter = 10);

    // 你可以扩展其它算法，如 median cut、均值聚类等
};