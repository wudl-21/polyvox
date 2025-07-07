#include "color_quantizer.h"
#include <algorithm>
#include <cfloat>
#include <cmath>

static inline void unpack_color(uint32_t color, unsigned char& r, unsigned char& g, unsigned char& b) {
    r = (color >> 16) & 0xFF;
    g = (color >> 8) & 0xFF;
    b = color & 0xFF;
}

static inline uint32_t pack_color(unsigned char r, unsigned char g, unsigned char b) {
    return (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
}

static float color_distance(uint32_t c1, uint32_t c2) {
    unsigned char r1, g1, b1, r2, g2, b2;
    unpack_color(c1, r1, g1, b1);
    unpack_color(c2, r2, g2, b2);
    long dr = r1 - r2;
    long dg = g1 - g2;
    long db = b1 - b2;
    return (float)(dr*dr*0.3 + dg*dg*0.59 + db*db*0.11);
}

std::vector<uint32_t> ColorQuantizer::kmeans(const std::vector<uint32_t>& colors, int k, int max_iter) {
    if ((int)colors.size() <= k) return colors;

    std::vector<uint32_t> centers;
    for (int i = 0; i < k; i++) {
        centers.push_back(colors[i * colors.size() / k]);
    }
    for (int iter = 0; iter < max_iter; iter++) {
        std::vector<std::vector<uint32_t>> clusters(k);
        for (uint32_t color : colors) {
            float min_distance = FLT_MAX;
            int closest_center = 0;
            for (int j = 0; j < k; j++) {
                float dist = color_distance(color, centers[j]);
                if (dist < min_distance) {
                    min_distance = dist;
                    closest_center = j;
                }
            }
            clusters[closest_center].push_back(color);
        }
        bool centers_changed = false;
        for (int j = 0; j < k; j++) {
            if (clusters[j].empty()) continue;
            uint64_t sum_r = 0, sum_g = 0, sum_b = 0;
            for (uint32_t color : clusters[j]) {
                unsigned char r, g, b;
                unpack_color(color, r, g, b);
                sum_r += r; sum_g += g; sum_b += b;
            }
            uint32_t new_center = pack_color(
                (unsigned char)(sum_r / clusters[j].size()),
                (unsigned char)(sum_g / clusters[j].size()),
                (unsigned char)(sum_b / clusters[j].size())
            );
            if (new_center != centers[j]) {
                centers_changed = true;
                centers[j] = new_center;
            }
        }
        if (!centers_changed) break;
    }
    return centers;
}