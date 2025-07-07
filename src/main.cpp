#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cfloat>
#include <cstring>
#include <map>
#include <unordered_map>
#include <filesystem>
#include <memory>
#include <windows.h>    // For GetCommandLineW
#include <shellapi.h>   // For CommandLineToArgvW

#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb_image.h"

#define OGT_VOX_IMPLEMENTATION
#include "third_party/ogt_vox.h"

#include "local/xml_parser.h"
#include "local/logger.h"
#include "local/file_utils.h"
#include "local/color_quantizer.h"
#include "local/command_line.h"
#include "local/message.h"
#include "local/string_utils.h"

const float EDGE_OFFSET_MULTIPLIER = 0.25f; // 边界偏移量乘数，用于计算体素条的偏移

// VOX 格式的尺寸限制
const int MAX_VOX_SIZE = 256;

// 3D向量结构
struct Vec3 {
    float x, y, z;
    Vec3(float _x = 0.0f, float _y = 0.0f, float _z = 0.0f) : x(_x), y(_y), z(_z) {}
};

// UV坐标结构
struct Vec2 {
    float u, v;
    Vec2(float _u = 0.0f, float _v = 0.0f) : u(_u), v(_v) {}
};

// 面结构
struct Face {
    std::vector<int> v; // 顶点索引
    std::vector<int> t; // 纹理坐标索引
    std::string material_name; // 面使用的材质
};

// MTL材质结构 - 增强以支持更多物理属性
struct MtlMaterial {
    std::string name;          // 材质名称
    std::string diffuse_map;   // 漫反射纹理路径
    Vec3 Kd = {1.0f, 1.0f, 1.0f}; // 新增：漫反射颜色（默认白色）
    Vec3 Ks = {0.0f, 0.0f, 0.0f}; // 镜面反射颜色
    float Ns = 10.0f;          // 镜面反射指数
    float d = 1.0f;            // 不透明度 (dissolve)
    Vec3 Ke = {0.0f, 0.0f, 0.0f}; // 自发光颜色
    float Ni = 1.0f;           // 折射率
};

// 表示多边形的一条边
struct Edge {
    int start_index;  // 起点在vertices数组中的索引
    int end_index;    // 终点在vertices数组中的索引
    float length;     // 边的长度
    bool is_aligned = false; // 新增：是否轴对齐
};

// 存储边的原始信息，用于XML生成
struct EdgeInfo {
    Vec3 startPos;       // 位置
    Vec3 endPos;         // 结束位置（原始坐标系）
    float length;        // 长度
    int parentFaceIndex; // 所属面索引
};

// OBJ模型结构
struct ObjModel {
    std::vector<Vec3> vertices;
    std::vector<Vec2> texcoords;
    std::vector<Face> faces;
    std::vector<Edge> original_edges; // 新增：存储原始边界边
    std::string mtl_filename;                  // MTL文件名
    std::map<std::string, MtlMaterial> materials; // 材质列表
    std::string current_material;              // 当前使用的材质
};

// 统一的体素条长度计算函数，支持可选最小值参数（默认为1）
inline int calc_voxel_strip_length(float edge_length, float voxel_size, int min_voxels = 1) {
    int n = static_cast<int>(std::floor(edge_length / voxel_size));
    if (n < min_voxels) n = min_voxels;
    return n;
}

struct VoxelStripPlacement {
    int voxel_count;
    float offset_along_edge; // 相对于原边中点的平移（正值为沿边方向，负值为反向）
};

inline VoxelStripPlacement calc_voxel_strip_placement(float edge_length, float voxel_size) {
    int n_floor = static_cast<int>(std::floor(edge_length / voxel_size));
    int n_ceil  = static_cast<int>(std::ceil(edge_length / voxel_size));
    float err_floor = edge_length - n_floor * voxel_size;
    float err_ceil  = n_ceil * voxel_size - edge_length;

    // 选择误差较小的方案
    int n = (err_ceil < err_floor) ? n_ceil : n_floor;
    if (n < 1) n = 1;
    float actual_length = n * voxel_size;
    float offset = (actual_length - edge_length) / 2.0f; // 体素条中心相对原边中点的平移
    return { n, offset };
}

// 新增：分割单条边（替换 split_long_edges）
std::vector<Edge> split_single_edge(const Edge& edge, ObjModel& obj_model, float voxel_size) {
    std::vector<Edge> segments;
    float maxLength = MAX_VOX_SIZE * voxel_size;

    if (edge.length <= maxLength) {
        segments.push_back(edge);
        return segments;
    }

    int num_segments = calc_voxel_strip_length(edge.length, maxLength * 1.0f);
    const Vec3& start_v = obj_model.vertices[edge.start_index];
    const Vec3& end_v = obj_model.vertices[edge.end_index];

    float dx = (end_v.x - start_v.x) / num_segments;
    float dy = (end_v.y - start_v.y) / num_segments;
    float dz = (end_v.z - start_v.z) / num_segments;

    int prev_v_index = edge.start_index;
    for (int i = 1; i <= num_segments; ++i) {
        int current_v_index;
        if (i == num_segments) {
            current_v_index = edge.end_index;
        } else {
            Vec3 new_vertex = { start_v.x + i * dx, start_v.y + i * dy, start_v.z + i * dz };
            current_v_index = obj_model.vertices.size();
            obj_model.vertices.push_back(new_vertex);
        }

        Edge new_segment;
        new_segment.start_index = prev_v_index;
        new_segment.end_index = current_v_index;
        
        const Vec3& seg_start = obj_model.vertices[new_segment.start_index];
        const Vec3& seg_end = obj_model.vertices[new_segment.end_index];
        float seg_dx = seg_end.x - seg_start.x;
        float seg_dy = seg_end.y - seg_start.y;
        float seg_dz = seg_end.z - seg_start.z;
        new_segment.length = std::sqrt(seg_dx*seg_dx + seg_dy*seg_dy + seg_dz*seg_dz);

        segments.push_back(new_segment);
        prev_v_index = current_v_index;
    }
    return segments;
}

// 修改：直接返回解析时存储的原始边，以避免三角化引入的内部边
std::vector<Edge> extract_edges_from_obj(const ObjModel& obj_model) {
    return obj_model.original_edges;
}

// 描述一个Teardown材质的完整画像
struct MaterialProfile {
    std::string td_note;      // Teardown物理关键字, 如 "$TD_wood"
    ogt_vox_matl vox_material; // MagicaVoxel渲染材质参数
};

// 颜色与物理材质的采样点
struct ColorSample {
    uint32_t color_rgb;
    std::string td_note;
};

// 返回 true 表示逆时针（法线朝上/内部），false 表示顺时针（法线朝下/外部）
bool is_face_ccw(const Face& face, const std::vector<Vec3>& vertices) {
    float area = 0.0f;
    for (size_t i = 0; i < face.v.size(); ++i) {
        const Vec3& v0 = vertices[face.v[i]];
        const Vec3& v1 = vertices[face.v[(i + 1) % face.v.size()]];
        area += (v1.x - v0.x) * (v1.y + v0.y);
    }
    return area < 0.0f; // <0为逆时针，>0为顺时针
}

// 获取边的2D外部法线（与面无关，始终为左手法则外侧）
Vec3 get_edge_outer_normal(const Edge& edge, const ObjModel& obj_model) {
    const Vec3& p1 = obj_model.vertices[edge.start_index];
    const Vec3& p2 = obj_model.vertices[edge.end_index];
    float dx = p2.x - p1.x;
    float dy = p2.y - p1.y;
    float len = std::sqrt(dx*dx + dy*dy);
    if (len > 1e-6f) {
        dx /= len;
        dy /= len;
    }
    // 外侧法线（左手法则：边的起点到终点，外侧为 (dy, -dx)）
    return {dy, -dx, 0.0f};
}

// 获取边的多边形外部法线（依赖于面朝向）
Vec3 get_edge_polygon_outer_normal(const Edge& edge, const Face& face, const std::vector<Vec3>& vertices) {
    const Vec3& p1 = vertices[edge.start_index];
    const Vec3& p2 = vertices[edge.end_index];
    float dx = p2.x - p1.x;
    float dy = p2.y - p1.y;
    float len = std::sqrt(dx*dx + dy*dy);
    if (len > 1e-6f) {
        dx /= len;
        dy /= len;
    }
    bool ccw = is_face_ccw(face, vertices);
    // 逆时针时，外部法线为 (dy, -dx)；顺时针时，外部法线为 (-dy, dx)
    return ccw ? Vec3{dy, -dx, 0.0f} : Vec3{-dy, dx, 0.0f};
}

// 智能调色板管理器
class PaletteManager {
public:
    // 构造函数
    PaletteManager();

    // 收集一个从模型表面采样到的点
    void collect_sample(uint32_t color_rgb, const std::string& td_note);

    // 执行颜色量化和调色板分配
    void process_and_quantize(
        const std::map<std::string, MaterialProfile>& profiles,
        const std::map<std::string, MtlMaterial>& mtl_materials,
        const CommandLineArgs& args);

    // 获取一个原始颜色在最终调色板中的索引
    uint8_t get_final_index(uint32_t original_color, const std::string& td_note) const;

    // 获取最终生成的调色板、注释和材质
    ogt_vox_palette get_palette() const;
    const std::map<uint8_t, std::string>& get_notes() const;
    const std::map<uint8_t, ogt_vox_matl>& get_materials() const;

private:
    // --- 修复：采样池现在需要存储原始材质名，而不仅仅是td_note ---
    struct ColorSample {
        uint32_t color_rgb;
        std::string material_name; // 使用原始材质名作为唯一标识
    };
    std::vector<ColorSample> sample_pool;
    
    ogt_vox_palette final_palette = {};
    std::map<uint8_t, std::string> final_notes;
    std::map<uint8_t, ogt_vox_matl> final_materials;
    
    // --- 修复：重映射表现在也需要使用原始材质名 ---
    std::map<std::pair<uint32_t, std::string>, uint8_t> remap_table;
};

// 将RGB颜色打包为32位整数
inline uint32_t pack_color(unsigned char r, unsigned char g, unsigned char b) {
    return (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
}

// 从32位整数解包RGB颜色
inline void unpack_color(uint32_t color, unsigned char& r, unsigned char& g, unsigned char& b) {
    r = (color >> 16) & 0xFF;
    g = (color >> 8) & 0xFF;
    b = color & 0xFF;
}

// 计算两个颜色之间的感知距离
float color_distance(uint32_t c1, uint32_t c2) {
    unsigned char r1, g1, b1, r2, g2, b2;
    unpack_color(c1, r1, g1, b1);
    unpack_color(c2, r2, g2, b2);
    
    // 人眼对绿色更敏感
    long dr = r1 - r2;
    long dg = g1 - g2;
    long db = b1 - b2;
    
    return (float)(dr*dr*0.3 + dg*dg*0.59 + db*db*0.11);
}

// 计算边的角度并检查是否与网格对齐shi
float calculate_edge_angle(const Vec3& start, const Vec3& end, bool* isAligned = nullptr) {
    float dx = end.x - start.x;
    float dy = end.y - start.y;
    
    // 计算角度 (弧度)，从正X轴顺时针方向
    float angle = atan2f(-dy, dx); // 注意Y轴需要反转
    
    // 转换为度
    float degrees = angle * 180.0f / 3.14159265f;
    
    // 调整范围为 [0, 360)
    if (degrees < 0) {
        degrees += 360.0f;
    }
    
    // 检查是否与网格对齐（如果需要）
    if (isAligned) {
        // 如果dx或dy接近于0，则边在垂直或水平方向上
        const float EPSILON = 0.001f;
        if (std::abs(dx) < EPSILON || std::abs(dy) < EPSILON) {
            *isAligned = true;
        }
        else {
            // 检查是否是90度的整数倍（允许小误差）
            const float ANGLE_EPSILON = 1.0f; // 允许1度的误差
            float mod90 = std::fmod(degrees, 90.0f);
            *isAligned = mod90 < ANGLE_EPSILON || mod90 > (90.0f - ANGLE_EPSILON);
        }
    }
    
    return degrees;
}

PaletteManager::PaletteManager() {
    // 索引0是保留给透明的
    // 索引1-8保留给车辆灯光
    // 索引254-255保留给Hole
    // 所以我们从9开始，到253结束
    final_palette.color[0] = {0,0,0,0};
}

// 新增：判断是否为保留色索引
inline bool is_reserved_palette_index(uint8_t palette_index) {
    // 0-8, 225-240, 254-255 都是保留
    return (palette_index <= 8) ||
           //(palette_index >= 225 && palette_index <= 240) ||
           (palette_index >= 254);
}

// 修改 PaletteManager::collect_sample 函数签名
void PaletteManager::collect_sample(uint32_t color_rgb, const std::string& material_name) {
    sample_pool.push_back({color_rgb, material_name});
}

// --- 核心修复：重构 PaletteManager::process_and_quantize 函数 ---
void PaletteManager::process_and_quantize(
    const std::map<std::string, MaterialProfile>& profiles,
    const std::map<std::string, MtlMaterial>& mtl_materials,
    const CommandLineArgs& args)
{
    if (sample_pool.empty()) {
        Logger::warn(Message::get("SAMPLE_POOL_EMPTY"));

        // 兜底逻辑：为无MTL材质且无图片纹理的模型提供默认颜色和物理标签
        // 1. 如果模型没有任何材质或纹理，所有面分配洋红色和空物理标签
        // 2. 如果有MTL材质但无纹理，使用MTL的漫反射色，物理标签若无匹配则为空字符串

        bool has_mtl = !profiles.empty() && !(profiles.size() == 1 && profiles.count("") == 1);
        bool has_any_color = false;

        if (!has_mtl) {
            // 没有任何材质，所有面分配洋红色
            uint32_t magenta = pack_color(255, 0, 255);
            sample_pool.clear();
            sample_pool.reserve(64);
            for (int i = 0; i < 32; ++i) { // 采样32次，保证量化有数据
                sample_pool.push_back({magenta, ""});
            }
            Logger::warn("No MTL/material found, using magenta as default color and empty physical tag.");
        } else {
            // 有MTL但无纹理，尝试用MTL的漫反射色
            for (const auto& [mat_name, profile] : profiles) {
                if (mat_name.empty()) continue;
                auto mtl_it = mtl_materials.find(mat_name);
                unsigned char r = 200, g = 200, b = 200;
                if (mtl_it != mtl_materials.end()) {
                    r = static_cast<unsigned char>(std::clamp(mtl_it->second.Kd.x * 255.0f, 0.0f, 255.0f));
                    g = static_cast<unsigned char>(std::clamp(mtl_it->second.Kd.y * 255.0f, 0.0f, 255.0f));
                    b = static_cast<unsigned char>(std::clamp(mtl_it->second.Kd.z * 255.0f, 0.0f, 255.0f));
                }
                uint32_t color = pack_color(r, g, b);
                std::string td_note = profile.td_note.empty() ? "" : profile.td_note;
                sample_pool.push_back({color, td_note});
                has_any_color = true;
            }
            if (!has_any_color) {
                // 兜底：仍然分配洋红色
                uint32_t magenta = pack_color(255, 0, 255);
                for (int i = 0; i < 32; ++i) {
                    sample_pool.push_back({magenta, ""});
                }
                Logger::warn("No valid color in MTL, using magenta as default color and empty physical tag.");
            } else {
                Logger::warn("No texture found, using MTL color and empty physical tag if not matched.");
            }
        }
    }

    std::map<std::string, MaterialProfile> profiles_by_note;
    for (const auto& pair : profiles) {
        profiles_by_note[pair.second.td_note] = pair.second;
    }

    // --- 修复：按原始材质名进行分组，而不是td_note ---
    std::map<std::string, std::vector<uint32_t>> colors_by_material_name;
    for (const auto& sample : sample_pool) {
        colors_by_material_name[sample.material_name].push_back(sample.color_rgb);
    }

    int total_available_slots = 0;
    for (int idx = 9; idx <= 253; ++idx) {
        if (!is_reserved_palette_index(idx)) {
            total_available_slots++;
        }
    }
    Logger::info(Message::get("TOTAL_PALETTE_SLOTS", { {"total", std::to_string(total_available_slots)} }));

    std::map<std::string, int> unique_color_counts;
    int total_unique_colors = 0;
    for (auto const& [mat_name, colors] : colors_by_material_name) {
        std::vector<uint32_t> temp_colors = colors;
        std::sort(temp_colors.begin(), temp_colors.end());
        int count = std::unique(temp_colors.begin(), temp_colors.end()) - temp_colors.begin();
        unique_color_counts[mat_name] = count;
        total_unique_colors += count;
    }

    // --- 修复：按原始材质名分配槽位 ---
    std::map<std::string, int> slots_for_material;
    int slots_assigned = 0;
    std::vector<std::string> material_order;
    for (auto const& [mat_name, count] : unique_color_counts) {
        material_order.push_back(mat_name);
        float proportion = (total_unique_colors > 0) ? ((float)count / (float)total_unique_colors) : 0.0f;
        int allocated_slots = std::max(1, static_cast<int>(proportion * total_available_slots));
        if (allocated_slots % 8 != 0) allocated_slots = ((allocated_slots / 8) + 1) * 8;
        if (allocated_slots == 0 && count > 0) allocated_slots = 8;
        slots_for_material[mat_name] = allocated_slots;
        slots_assigned += allocated_slots;
    }

    // 补齐或缩减逻辑，步长为8
    if (slots_assigned < total_available_slots && !material_order.empty()) {
        int remain = total_available_slots - slots_assigned;
        size_t idx = 0;
        while (remain > 0) {
            slots_for_material[material_order[idx % material_order.size()]] += 8;
            remain -= 8;
            idx++;
        }
    } else if (slots_assigned > total_available_slots) {
        int over = slots_assigned - total_available_slots;
        while (over > 0 && !material_order.empty()) {
            bool changed = false;
            for (auto& mat_name : material_order) {
                if (slots_for_material[mat_name] > 8 && over > 0) {
                    slots_for_material[mat_name] -= 8;
                    over -= 8;
                    changed = true;
                }
            }
            if (!changed) break; // 防止死循环
        }
    }


    slots_assigned = 0;
    for (auto const& [mat_name, slots] : slots_for_material) slots_assigned += slots;
    Logger::info(Message::get("PALETTE_SLOTS_ASSIGNED", { {"count", std::to_string(slots_assigned)} }));

    // 4. 对每组进行K-Means量化
    uint8_t current_palette_index = 9;
    for (auto const& [mat_name, original_colors] : colors_by_material_name) {
        int k = slots_for_material[mat_name];
        if (k == 0 || original_colors.empty()) continue;

        // --- 修复：日志现在显示原始材质名 ---
        auto profile_it = profiles.find(mat_name);
        const std::string& td_note_for_log = (profile_it != profiles.end()) ? profile_it->second.td_note : "Unknown";
        Logger::info(Message::get("PROCESS_MATERIAL", { {"note", td_note_for_log}, {"orig", std::to_string(unique_color_counts[mat_name])}, {"quant", std::to_string(k)} }));

        std::vector<uint32_t> unique_colors = original_colors;
        std::sort(unique_colors.begin(), unique_colors.end());
        unique_colors.erase(std::unique(unique_colors.begin(), unique_colors.end()), unique_colors.end());

        std::vector<uint32_t> centers = ColorQuantizer::kmeans(unique_colors, k);

        for (size_t i = 0; i < centers.size(); ++i) {
            while (current_palette_index <= 253 && is_reserved_palette_index(current_palette_index)) {
                current_palette_index++;
            }
            if (current_palette_index > 253) {
                Logger::error(Message::get("PALETTE_INDEX_OUT_OF_RANGE"));
                break;
            }
            uint32_t center_color = centers[i];
            
            // --- 核心修复：直接通过原始材质名查找正确的Profile ---
            if (profile_it != profiles.end()) {
                final_notes[current_palette_index] = profile_it->second.td_note;
                final_materials[current_palette_index] = profile_it->second.vox_material;
            }

            unsigned char r, g, b;
            unpack_color(center_color, r, g, b);
            final_palette.color[current_palette_index] = {r, g, b, 255};
            
            // --- 修复：更新重映射表 ---
            for (uint32_t original_color : unique_colors) {
                float min_dist = FLT_MAX;
                uint32_t best_center = 0;
                for (size_t j = 0; j < centers.size(); ++j) {
                    float dist = color_distance(original_color, centers[j]);
                    if (dist < min_dist) {
                        min_dist = dist;
                        best_center = centers[j];
                    }
                }
                if (best_center == center_color) {
                    remap_table[{original_color, mat_name}] = current_palette_index;
                }
            }
            current_palette_index++;
        }
    }
}

// 修改 PaletteManager::get_final_index 函数签名
uint8_t PaletteManager::get_final_index(uint32_t original_color, const std::string& material_name) const {
    auto it = remap_table.find({original_color, material_name});
    if (it != remap_table.end()) {
        return it->second;
    }
    // 兜底逻辑：如果找不到精确匹配，尝试只按颜色匹配
    for(const auto& remap_pair : remap_table) {
        if (remap_pair.first.first == original_color) {
            return remap_pair.second;
        }
    }
    return 0; 
}

ogt_vox_palette PaletteManager::get_palette() const { return final_palette; }
const std::map<uint8_t, std::string>& PaletteManager::get_notes() const { return final_notes; }
const std::map<uint8_t, ogt_vox_matl>& PaletteManager::get_materials() const { return final_materials; }

// 材质分类器
std::map<std::string, MaterialProfile> classify_materials(
    const std::map<std::string, MtlMaterial>& mtl_materials,
    const std::vector<std::string>& material_maps,
    const std::vector<std::string>& material_properties) // 新增参数
{
    std::map<std::string, MaterialProfile> profiles;
    
    // 1. 解析外部传入的自定义映射 (TD Note 和 VOX Type)
    std::map<std::string, std::string> custom_td_notes;
    std::map<std::string, std::string> custom_vox_types; // <-- 新增
    for (const auto& map_str : material_maps) {
        std::vector<std::string> parts;
        std::string current_part;
        std::stringstream ss(map_str);
        while(std::getline(ss, current_part, ':')) {
            parts.push_back(current_part);
        }

        if (parts.size() >= 2) {
            custom_td_notes[parts[0]] = parts[1];
            if (parts.size() >= 3) {
                custom_vox_types[parts[0]] = parts[2]; // <-- 新增：保存 vox_type
            }
        }
    }

    // --- 新增：解析外部传入的属性覆盖 ---
    std::map<std::string, std::map<std::string, float>> custom_properties;
    for (const auto& prop_str : material_properties) {
        std::vector<std::string> parts;
        std::string current_part;
        std::stringstream ss(prop_str);
        while(std::getline(ss, current_part, ':')) {
            parts.push_back(current_part);
        }
        if (parts.size() == 3) {
            std::string mat_name = parts[0];
            std::string prop_name = parts[1];
            try {
                float prop_value = std::stof(parts[2]);
                custom_properties[mat_name][prop_name] = prop_value;
            } catch (const std::invalid_argument& ia) {
                Logger::warn("Invalid property value: " + parts[2]);
            }
        }
    }

    for (const auto& pair : mtl_materials) {
        const std::string& mtl_name = pair.first;
        const MtlMaterial& mtl = pair.second;
        MaterialProfile profile;
        memset(&profile.vox_material, 0, sizeof(profile.vox_material));

        // --- 修复：重构逻辑顺序，确保用户设置拥有最高优先级 ---

        // 步骤 1: 首先应用所有来自 -p 参数的用户自定义属性
        auto props_it = custom_properties.find(mtl_name);
        if (props_it != custom_properties.end()) {
            for (const auto& prop_pair : props_it->second) {
                const std::string& prop_name = prop_pair.first;
                float prop_value = prop_pair.second;

                if (prop_name == "rough") {
                    profile.vox_material.rough = prop_value;
                    profile.vox_material.content_flags |= k_ogt_vox_matl_have_rough;
                } else if (prop_name == "spec") {
                    profile.vox_material.spec = prop_value;
                    profile.vox_material.content_flags |= k_ogt_vox_matl_have_spec;
                } else if (prop_name == "ior") {
                    profile.vox_material.ior = prop_value;
                    profile.vox_material.content_flags |= k_ogt_vox_matl_have_ior;
                } else if (prop_name == "trans") {
                    profile.vox_material.trans = prop_value;
                    profile.vox_material.alpha = 1.0f - prop_value;
                    profile.vox_material.content_flags |= k_ogt_vox_matl_have_trans;
                    profile.vox_material.content_flags |= k_ogt_vox_matl_have_alpha;
                } else if (prop_name == "emission") {
                    profile.vox_material.emit = prop_value;
                    profile.vox_material.content_flags |= k_ogt_vox_matl_have_emit;
                } else if (prop_name == "power") {
                    profile.vox_material.flux = prop_value;
                    profile.vox_material.content_flags |= k_ogt_vox_matl_have_flux;
                } else if (prop_name == "ldr") {
                    profile.vox_material.ldr = prop_value;
                    profile.vox_material.content_flags |= k_ogt_vox_matl_have_ldr;
                } else if (prop_name == "metal") {
                    profile.vox_material.metal = prop_value;
                    profile.vox_material.content_flags |= k_ogt_vox_matl_have_metal;
                }
            }
        }

        // 步骤 2: 确定 TD Note 和 VOX 渲染类型
        auto custom_it = custom_td_notes.find(mtl_name);
        if (custom_it != custom_td_notes.end() && custom_it->second != "$TD_auto") {
            // 用户手动指定了映射
            profile.td_note = custom_it->second;
            
            auto type_it = custom_vox_types.find(mtl_name);
            if (type_it != custom_vox_types.end()) {
                // 优先使用 -m 参数第三段指定的 vox_type
                const std::string& vox_type = type_it->second;
                if (vox_type == "glass") profile.vox_material.type = ogt_matl_type_glass;
                else if (vox_type == "metal") profile.vox_material.type = ogt_matl_type_metal;
                else if (vox_type == "emit") profile.vox_material.type = ogt_matl_type_emit;
                else profile.vox_material.type = ogt_matl_type_diffuse;
            } else {
                // 如果 -m 只有两段，则根据 TD Note 推断
                if (profile.td_note == "$TD_glass") profile.vox_material.type = ogt_matl_type_glass;
                else if (profile.td_note == "$TD_metal") profile.vox_material.type = ogt_matl_type_metal;
                else profile.vox_material.type = ogt_matl_type_diffuse;
            }
        } else {
            // 用户选择自动检测，或未提供映射
            // 在这里，我们根据已应用的属性和MTL原始数据进行推断
            std::string lower_name = mtl_name;
            std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
                           [](unsigned char c){ return std::tolower(c); });

            // 首先根据已应用的属性推断渲染类型
            if (profile.vox_material.content_flags & k_ogt_vox_matl_have_emit && profile.vox_material.emit > 0.0f) {
                profile.vox_material.type = ogt_matl_type_emit;
            } else if (mtl.d < 0.9f || (profile.vox_material.content_flags & k_ogt_vox_matl_have_trans && profile.vox_material.trans > 0.0f)) {
                profile.vox_material.type = ogt_matl_type_glass;
            } else if (profile.vox_material.content_flags & k_ogt_vox_matl_have_metal && profile.vox_material.metal > 0.5f) {
                profile.vox_material.type = ogt_matl_type_metal;
            } else {
                profile.vox_material.type = ogt_matl_type_diffuse; // 默认
            }
            
            // 然后根据渲染类型和名称推断 TD Note
            if (profile.vox_material.type == ogt_matl_type_glass) {
                profile.td_note = "$TD_glass";
            } else if (profile.vox_material.type == ogt_matl_type_metal || lower_name.find("metal") != std::string::npos) {
                profile.td_note = "$TD_metal";
            } else if (lower_name.find("wood") != std::string::npos) {
                profile.td_note = "$TD_wood";
            } else if (lower_name.find("brick") != std::string::npos || lower_name.find("concrete") != std::string::npos) {
                profile.td_note = "$TD_masonry";
            } else if (lower_name.find("vegetation") != std::string::npos) {
                profile.td_note = "$TD_foliage";
            } else if (lower_name.find("carpet") != std::string::npos) {
                profile.td_note = "$TD_plastic";
            } else {
                profile.td_note = "$TD_metal"; // 默认兜底
            }
        }
        
        // 步骤 3: 为尚未被用户覆盖的属性，从MTL文件应用默认值
        if (!(profile.vox_material.content_flags & k_ogt_vox_matl_have_rough)) {
            profile.vox_material.rough = std::max(0.001f, 1.0f - (mtl.Ns / 1000.0f));
            profile.vox_material.content_flags |= k_ogt_vox_matl_have_rough;
        }
        if (!(profile.vox_material.content_flags & k_ogt_vox_matl_have_spec)) {
            profile.vox_material.spec = (mtl.Ks.x + mtl.Ks.y + mtl.Ks.z) / 3.0f;
            if (profile.vox_material.spec > 0.0f) profile.vox_material.content_flags |= k_ogt_vox_matl_have_spec;
        }
        if (!(profile.vox_material.content_flags & k_ogt_vox_matl_have_ior)) {
            profile.vox_material.ior = mtl.Ni;
            profile.vox_material.content_flags |= k_ogt_vox_matl_have_ior;
        }
        if (!(profile.vox_material.content_flags & k_ogt_vox_matl_have_alpha)) {
            profile.vox_material.alpha = mtl.d;
            if (profile.vox_material.alpha < 1.0f) profile.vox_material.content_flags |= k_ogt_vox_matl_have_alpha;
        }
        if (!(profile.vox_material.content_flags & k_ogt_vox_matl_have_trans) && profile.vox_material.type == ogt_matl_type_glass) {
            profile.vox_material.trans = 1.0f - profile.vox_material.alpha;
            profile.vox_material.content_flags |= k_ogt_vox_matl_have_trans;
        }
        if (!(profile.vox_material.content_flags & k_ogt_vox_matl_have_emit)) {
            profile.vox_material.emit = (mtl.Ke.x + mtl.Ke.y + mtl.Ke.z) / 3.0f;
            if (profile.vox_material.emit > 0.0f) profile.vox_material.content_flags |= k_ogt_vox_matl_have_emit;
        }
        if (!(profile.vox_material.content_flags & k_ogt_vox_matl_have_flux) && (profile.vox_material.content_flags & k_ogt_vox_matl_have_emit)) {
            profile.vox_material.flux = profile.vox_material.emit * 4.0f;
            profile.vox_material.content_flags |= k_ogt_vox_matl_have_flux;
        }
        
        profiles[mtl_name] = profile;
    }
    
    // 添加一个默认profile，用于没有材质的面
    MaterialProfile default_profile;
    default_profile.td_note = "";
    memset(&default_profile.vox_material, 0, sizeof(default_profile.vox_material));
    default_profile.vox_material.type = ogt_matl_type_diffuse;
    default_profile.vox_material.rough = 0.8f;
    default_profile.vox_material.content_flags = k_ogt_vox_matl_have_rough;
    profiles[""] = default_profile;

    for (const auto& pair : profiles) {
        Logger::info(Message::get("MATERIAL_CLASSIFY", { {"name", pair.first}, {"td_note", pair.second.td_note} }));
    }

    return profiles;
}

// 添加一个HSV转RGB的辅助函数
struct RGB {
    unsigned char r, g, b;
};

// 用于存储量化后的颜色映射
struct ColorMap {
    std::vector<ogt_vox_rgba> colors;      // 量化后的颜色
    std::unordered_map<uint32_t, uint8_t> color_to_index; // 原始颜色到调色板索引的映射
};

// 解析MTL文件 - 增强以支持新属性
// 修改：函数签名接受 std::filesystem::path
bool parse_mtl_file(const std::filesystem::path& mtl_path, std::map<std::string, MtlMaterial>& materials) {
    std::ifstream file(mtl_path);
    if (!file.is_open()) {
        Logger::error(Message::get("CANNOT_OPEN_MTL", { {"filename", mtl_path.generic_string()} })); // <-- 修改
        return false;
    }
    
    MtlMaterial current_material;
    bool has_material = false;
    
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream iss(line);
        std::string token;
        iss >> token;
        
        if (token == "newmtl") {
            if (has_material) {
                materials[current_material.name] = current_material;
            }
            current_material = MtlMaterial();
            iss >> current_material.name;
            has_material = true;
        }
        else if (token == "map_Kd" && has_material) {
            std::string texture_path;
            std::getline(iss, texture_path);
            texture_path.erase(0, texture_path.find_first_not_of(" \t\r\n"));
            texture_path.erase(texture_path.find_last_not_of(" \t\r\n") + 1);
            current_material.diffuse_map = texture_path;
        }
        else if (token == "Kd" && has_material) {
            iss >> current_material.Kd.x >> current_material.Kd.y >> current_material.Kd.z;
        }
        else if (token == "Ks" && has_material) {
            iss >> current_material.Ks.x >> current_material.Ks.y >> current_material.Ks.z;
        }
        else if (token == "Ns" && has_material) {
            iss >> current_material.Ns;
        }
        else if (token == "d" && has_material) {
            iss >> current_material.d;
        }
        // --- 新增：解析 Ke 和 Ni ---
        else if (token == "Ke" && has_material) {
            iss >> current_material.Ke.x >> current_material.Ke.y >> current_material.Ke.z;
        }
        else if (token == "Ni" && has_material) {
            iss >> current_material.Ni;
        }
    }
    
    if (has_material) {
        materials[current_material.name] = current_material;
    }
    
    file.close();
    return !materials.empty();
}

// 解析OBJ文件
// 修改：函数签名接受 std::filesystem::path
bool parse_obj_file(const std::filesystem::path& obj_path, ObjModel& model) {
    std::ifstream file(obj_path);
    if (!file.is_open()) {
        Logger::error(Message::get("CANNOT_OPEN_OBJ", { {"filename", obj_path.generic_string()} })); // <-- 修改
        return false;
    }
    
    std::filesystem::path objPath(obj_path);
    std::string directory = objPath.parent_path().string();
    
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        
        std::istringstream iss(line);
        std::string token;
        iss >> token;
        
        if (token == "v") {
            Vec3 vertex;
            iss >> vertex.x >> vertex.y >> vertex.z;
            model.vertices.push_back(vertex);
        } 
        else if (token == "vt") {
            Vec2 texcoord;
            iss >> texcoord.u >> texcoord.v;
            model.texcoords.push_back(texcoord);
        }
        else if (token == "mtllib") {
            iss >> model.mtl_filename;
        }
        else if (token == "usemtl") {
            iss >> model.current_material;
        }
        else if (token == "f") {
            Face face;
            face.material_name = model.current_material;
            
            std::string vertex_str;
            while (iss >> vertex_str) {
                size_t slash1_pos = vertex_str.find('/');
                size_t slash2_pos = (slash1_pos == std::string::npos) ? std::string::npos : vertex_str.find('/', slash1_pos + 1);

                int v_idx = std::stoi(vertex_str.substr(0, slash1_pos));
                face.v.push_back((v_idx > 0) ? (v_idx - 1) : (model.vertices.size() + v_idx));

                if (slash1_pos != std::string::npos) {
                    if (slash1_pos + 1 != slash2_pos) {
                        int t_idx = std::stoi(vertex_str.substr(slash1_pos + 1, slash2_pos - (slash1_pos + 1)));
                        face.t.push_back((t_idx > 0) ? (t_idx - 1) : (model.texcoords.size() + t_idx));
                    } else {
                        face.t.push_back(0);
                    }
                } else {
                    face.t.push_back(0);
                }
            }
            
            if (face.v.size() > 1) {
                for (size_t i = 0; i < face.v.size(); ++i) {
                    size_t j = (i + 1) % face.v.size();
                    Edge edge;
                    edge.start_index = face.v[i];
                    edge.end_index = face.v[j];
                    const Vec3& start = model.vertices[edge.start_index];
                    const Vec3& end = model.vertices[edge.end_index];
                    edge.length = std::sqrt(pow(end.x - start.x, 2) + pow(end.y - start.y, 2) + pow(end.z - start.z, 2));
                    model.original_edges.push_back(edge);
                }
            }

            if (face.v.size() > 3) {
                for (size_t i = 1; i < face.v.size() - 1; i++) {
                    Face triangle_face;
                    triangle_face.material_name = face.material_name;
                    triangle_face.v = {face.v[0], face.v[i], face.v[i+1]};
                    triangle_face.t = {face.t[0], face.t[i], face.t[i+1]};
                    model.faces.push_back(triangle_face);
                }
            } else {
                model.faces.push_back(face);
            }
        }
    }
    
    file.close();
    
    if (!model.mtl_filename.empty()) {
        std::filesystem::path mtl_full_path = obj_path.parent_path() / model.mtl_filename;
        
        Logger::info(Message::get("TRY_LOAD_MTL", { {"filename", mtl_full_path.generic_string()} })); // <-- 修改
        if (!parse_mtl_file(mtl_full_path, model.materials)) {
            Logger::warn(Message::get("CANNOT_LOAD_MTL", { {"filename", mtl_full_path.generic_string()} })); // <-- 修改
        }
    }
    
    bool has_error = false;
    for (const auto& face : model.faces) {
        for (size_t i = 0; i < face.v.size(); i++) {
            if (face.v[i] < 0 || face.v[i] >= (int)model.vertices.size()) {
                Logger::error(Message::get("VERTEX_INDEX_OUT_OF_RANGE", { {"index", std::to_string(face.v[i])} }));
                has_error = true;
            }
            if (face.t[i] < 0 || face.t[i] >= (int)model.texcoords.size()) {
                Logger::warn(Message::get("TEXCOORD_INDEX_OUT_OF_RANGE", { {"index", std::to_string(face.t[i])} }));
                const_cast<Face&>(face).t[i] = 0;
            }
        }
    }
    
    if (has_error) {
        return false;
    }
    
    Logger::info(Message::get("LOAD_OBJ_SUCCESS", { 
        {"filename", obj_path.generic_string()}, // <-- 修改
        {"material_count", std::to_string(model.materials.size())},
        {"vertex_count", std::to_string(model.vertices.size())}, 
        {"texcoord_count", std::to_string(model.texcoords.size())}, 
        {"face_count", std::to_string(model.faces.size())} 
    }));
    
    return !model.vertices.empty() && !model.faces.empty();
}

// 查找纹理目录（优先-t参数，否则自动查找同名目录）
std::string find_texture_directory(const std::string& obj_path, const CommandLineArgs& args) {
    if (!args.texture_file.empty()) {
        return args.texture_file;
    }
    std::string obj_dir = FileUtils::getDirectory(obj_path);
    std::string obj_basename = FileUtils::getStem(obj_path);
    std::string candidate = FileUtils::join(obj_dir, obj_basename);
    if (std::filesystem::exists(candidate) && std::filesystem::is_directory(candidate)) {
        return candidate;
    }
    return obj_dir; // 兜底：返回obj所在目录
}

// 纹理图片缓存结构
struct TextureImage {
    int width = 0, height = 0, channels = 0;
    std::unique_ptr<unsigned char, decltype(&stbi_image_free)> data{nullptr, stbi_image_free};
};

using TextureMap = std::unordered_map<std::string, TextureImage>;

// 加载所有用到的纹理图片
TextureMap load_all_textures(const ObjModel& model, const std::string& texture_dir) {
    TextureMap tex_map;
    for (const auto& [mat_name, mat] : model.materials) {
        if (!mat.diffuse_map.empty()) {
            std::filesystem::path tex_path = std::filesystem::path(texture_dir) / mat.diffuse_map;
            
            if (!std::filesystem::exists(tex_path)) {
                // 尝试只用文件名查找
                tex_path = std::filesystem::path(texture_dir) / std::filesystem::path(mat.diffuse_map).filename();
            }

            if (std::filesystem::exists(tex_path)) {
                TextureImage img;
                
                // --- 修改：使用Unicode安全的方式加载纹理 ---
                FILE* f = nullptr;
                #ifdef _WIN32
                    f = _wfopen(tex_path.wstring().c_str(), L"rb");
                #else
                    f = fopen(tex_path.string().c_str(), "rb");
                #endif

                if (f) {
                    img.data.reset(stbi_load_from_file(f, &img.width, &img.height, &img.channels, 4));
                    fclose(f);
                }
                // --- 结束修改 ---

                if (img.data) {
                    tex_map[mat.diffuse_map] = std::move(img);
                    Logger::info(Message::get("TEXTURE_LOADED", { {"filename", tex_path.generic_string()} }));
                } else {
                    Logger::warn(Message::get("CANNOT_LOAD_TEXTURE", { {"filename", tex_path.generic_string()} }));
                }
            } else {
                Logger::warn(Message::get("TEXTURE_NOT_EXIST", { {"filename", tex_path.generic_string()} }));
            }
        }
    }
    return tex_map;
}

// 检查点是否在三角形内（使用重心坐标）
bool point_in_triangle(float x, float y, 
                      float x0, float y0, 
                      float x1, float y1, 
                      float x2, float y2, 
                      float& u, float& v, float& w) 
{
    float v0x = x1 - x0;
    float v0y = y1 - y0;
    float v1x = x2 - x0;
    float v1y = y2 - y0;
    float v2x = x - x0;
    float v2y = y - y0;
    
    float d00 = v0x*v0x + v0y*v0y;
    float d01 = v0x*v1x + v0y*v1y;
    float d11 = v1x*v1x + v1y*v1y;
    float d20 = v2x*v0x + v2y*v0y;
    float d21 = v2x*v1x + v2y*v1y;
    
    float denom = d00*d11 - d01*d01;
    if (std::abs(denom) < 1e-6) return false; // 三角形太小或变形
    
    v = (d11*d20 - d01*d21) / denom;
    w = (d00*d21 - d01*d20) / denom;
    u = 1.0f - v - w;
    
    // 容忍小的数值误差
    const float EPSILON = 1e-5f;
    return (u >= -EPSILON) && (v >= -EPSILON) && (w >= -EPSILON);
}

// 辅助函数：判断2D三角形与AABB是否相交
auto triangle_aabb_overlap_2d = [](float ax, float ay, float bx, float by, float cx, float cy,
                                   float min_x, float min_y, float max_x, float max_y) -> bool {
    // 1. 三角形任意顶点在AABB内
    if ((ax >= min_x && ax <= max_x && ay >= min_y && ay <= max_y) ||
        (bx >= min_x && bx <= max_x && by >= min_y && by <= max_y) ||
        (cx >= min_x && cx <= max_x && cy >= min_y && cy <= max_y))
        return true;
    // 2. AABB任意顶点在三角形内
    float u, v, w;
    auto point_in_tri = [&](float px, float py) {
        float v0x = bx - ax, v0y = by - ay;
        float v1x = cx - ax, v1y = cy - ay;
        float v2x = px - ax, v2y = py - ay;
        float d00 = v0x*v0x + v0y*v0y;
        float d01 = v0x*v1x + v0y*v1y;
        float d11 = v1x*v1x + v1y*v1y;
        float d20 = v2x*v0x + v2y*v0y;
        float d21 = v2x*v1x + v2y*v1y;
        float denom = d00*d11 - d01*d01;
        if (std::abs(denom) < 1e-6) return false;
        v = (d11*d20 - d01*d21) / denom;
        w = (d00*d21 - d01*d20) / denom;
        u = 1.0f - v - w;
        const float EPS = 1e-5f;
        return (u >= -EPS) && (v >= -EPS) && (w >= -EPS);
    };
    if (point_in_tri(min_x, min_y) || point_in_tri(max_x, min_y) ||
        point_in_tri(min_x, max_y) || point_in_tri(max_x, max_y))
        return true;
    // 3. 检查三角形三条边与AABB四条边是否有交点
    auto segs_intersect = [](float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4) {
        auto cross = [](float x0, float y0, float x1, float y1) { return x0*y1 - y0*x1; };
        float d1 = cross(x2-x1, y2-y1, x3-x1, y3-y1);
        float d2 = cross(x2-x1, y2-y1, x4-x1, y4-y1);
        float d3 = cross(x4-x3, y4-y3, x1-x3, y1-y3);
        float d4 = cross(x4-x3, y4-y3, x2-x3, y2-y3);
        return (d1*d2 < 0) && (d3*d4 < 0);
    };
    float tri[3][2] = { {ax, ay}, {bx, by}, {cx, cy} };
    float box[4][2] = { {min_x, min_y}, {max_x, min_y}, {max_x, max_y}, {min_x, max_y} };
    for (int i = 0; i < 3; ++i) {
        float x0 = tri[i][0], y0 = tri[i][1];
        float x1 = tri[(i+1)%3][0], y1 = tri[(i+1)%3][1];
        for (int j = 0; j < 4; ++j) {
            float bx0 = box[j][0], by0 = box[j][1];
            float bx1 = box[(j+1)%4][0], by1 = box[(j+1)%4][1];
            if (segs_intersect(x0, y0, x1, y1, bx0, by0, bx1, by1))
                return true;
        }
    }
    return false;
};

// --- 核心修复：使用健壮的射线投射法 ---
bool is_point_in_polygon(float px, float py, const std::vector<Vec3>& poly_vertices) {  // poly_vertices 是有序的多边形顶点索引
    int crossings = 0;
    size_t n = poly_vertices.size();
    for (size_t i = 0; i < n; ++i) {
        const Vec3& v1 = poly_vertices[i];
        const Vec3& v2 = poly_vertices[(i + 1) % n];
        if (((v1.y > py) != (v2.y > py)) &&
            (px < (v2.x - v1.x) * (py - v1.y) / (v2.y - v1.y + 1e-10f) + v1.x)) {
            crossings++;
        }
    }
    return (crossings % 2) == 1;
}


// 修改 get_voxel_color_and_note 函数，使其返回原始材质名和物理标签
inline void get_voxel_color_and_note(
    const ObjModel& obj_model,
    const TextureMap& texture_map,
    const std::map<std::string, MaterialProfile>& material_profiles,
    const std::string& material_name,
    float u, float v, float w,
    const std::vector<int>& t_indices,
    uint32_t& out_color_rgb,
    std::string& out_material_name, // <-- 输出参数1：原始材质名
    std::string& out_td_note)       // <-- 输出参数2：物理标签
{
    out_color_rgb = 0;
    out_material_name = material_name; // 直接将传入的材质名作为输出

    // 查找材质
    auto mat_it = obj_model.materials.find(material_name);
    if (mat_it != obj_model.materials.end()) {
        std::string tex_name = mat_it->second.diffuse_map;
        auto tex_it = texture_map.find(tex_name);
        bool valid_sample = false;

        if (tex_it != texture_map.end() && tex_it->second.data && t_indices.size() == 3) {
            const TextureImage& tex = tex_it->second;
            const Vec2& t0 = obj_model.texcoords[t_indices[0]];
            const Vec2& t1 = obj_model.texcoords[t_indices[1]];
            const Vec2& t2 = obj_model.texcoords[t_indices[2]];

            float tex_u = u * t0.u + v * t1.u + w * t2.u;
            float tex_v = u * t0.v + v * t1.v + w * t2.v;

            tex_u = std::fmod(tex_u, 1.0f); if (tex_u < 0) tex_u += 1.0f;
            tex_v = std::fmod(tex_v, 1.0f); if (tex_v < 0) tex_v += 1.0f;

            int tx = static_cast<int>(tex_u * (tex.width - 1));
            int ty = static_cast<int>((1.0f - tex_v) * (tex.height - 1));
            tx = std::max(0, std::min(tex.width - 1, tx));
            ty = std::max(0, std::min(tex.height - 1, ty));

            unsigned char* pixel = tex.data.get() + 4 * (ty * tex.width + tx);
            if (pixel[3] > 128) {
                out_color_rgb = pack_color(pixel[0], pixel[1], pixel[2]);
                valid_sample = true;
            }
        }

        // 兜底：无纹理或透明像素，使用MTL颜色
        if (!valid_sample) {
            unsigned char r = 200, g = 200, b = 200;
            r = static_cast<unsigned char>(std::clamp(mat_it->second.Kd.x * 255.0f, 0.0f, 255.0f));
            g = static_cast<unsigned char>(std::clamp(mat_it->second.Kd.y * 255.0f, 0.0f, 255.0f));
            b = static_cast<unsigned char>(std::clamp(mat_it->second.Kd.z * 255.0f, 0.0f, 255.0f));
            out_color_rgb = pack_color(r, g, b);
        }
    } else {
        // 没有材质，兜底洋红色
        out_color_rgb = pack_color(255, 0, 255);
    }

    // 获取物理标签
    auto profile_it = material_profiles.find(material_name);
    out_td_note = (profile_it != material_profiles.end()) ? profile_it->second.td_note : "";
}

// 新增：用于 std::unique_ptr 的自定义删除器结构体
// 这种方法比全局lambda更健壮，可以避免作用域和声明顺序问题
struct VoxModelDeleter {
    void operator()(ogt_vox_model* m) const {
        if (!m) return;
        // 必须先释放模型内部的体素数据，再释放模型本身
        if (m->voxel_data) {
            // 修正: 使用 const_cast 移除 voxel_data 的 const 属性以便 free()
            // 这是安全的，因为我们知道这块内存是我们自己用 malloc 分配的
            free(const_cast<uint8_t*>(m->voxel_data));
        }
        free(m);
    }
};

// 存储子模型及其变换
struct SubModel {
    // 使用新的自定义删除器结构体
    std::unique_ptr<ogt_vox_model, VoxModelDeleter> model;
    ogt_vox_transform transform;
    std::string name;
    int offsetX, offsetY;
    bool isEdge;
    int segmentIndex;
    int parentFaceIndex;
    EdgeInfo edgeInfo;
    
    // 构造函数，初始化 unique_ptr
    SubModel() : model(nullptr, VoxModelDeleter{}), isEdge(false), segmentIndex(0), parentFaceIndex(-1) {}

    // 允许移动构造和赋值，以支持在vector中存储和转移所有权
    SubModel(SubModel&&) = default;
    SubModel& operator=(SubModel&&) = default;

    // 禁止复制，因为 unique_ptr 是不可复制的
    SubModel(const SubModel&) = delete;
    SubModel& operator=(const SubModel&) = delete;
};

// 修改 collect_samples_from_model，传递原始材质名
void collect_samples_from_model(
    const ObjModel& obj_model,
    const TextureMap& texture_map,
    float voxel_size,
    const std::map<std::string, MaterialProfile>& material_profiles,
    PaletteManager& palette_manager,
    float min_x, float min_y, int total_voxel_x, int total_voxel_y)
{
    auto find_triangle_for_point = [&](float px, float py, float& out_u, float& out_v, float& out_w) -> const Face* {
        for (const auto& face : obj_model.faces) {
            const Vec3& v0 = obj_model.vertices[face.v[0]];
            const Vec3& v1 = obj_model.vertices[face.v[1]];
            const Vec3& v2 = obj_model.vertices[face.v[2]];
            if (point_in_triangle(px, py, v0.x, v0.y, v1.x, v1.y, v2.x, v2.y, out_u, out_v, out_w)) {
                return &face;
            }
        }
        return nullptr;
    };

    for (int gy = 0; gy < total_voxel_y; gy++) {
        for (int gx = 0; gx < total_voxel_x; gx++) {
            float world_x = min_x + (gx + 0.5f) * voxel_size;
            float world_y = min_y + (gy + 0.5f) * voxel_size;
            
            float u, v, w;
            const Face* center_face = find_triangle_for_point(world_x, world_y, u, v, w);

            if (center_face) {
                uint32_t color_rgb = 0;
                std::string mat_name;
                std::string td_note; // <-- 新增变量
                get_voxel_color_and_note(
                    obj_model,
                    texture_map,
                    material_profiles,
                    center_face->material_name,
                    u, v, w,
                    center_face->t,
                    color_rgb,
                    mat_name, // <-- 接收原始材质名
                    td_note   // <-- 接收物理标签
                );
                palette_manager.collect_sample(color_rgb, mat_name); // <-- 正确传递原始材质名
            }
        }
    }
}

// ==========================================================================================
// 开始：用于创建边缘模型的代码块
// ==========================================================================================

// 辅助函数：检查一条边是否与XY轴对齐（即水平或垂直）
bool is_edge_aligned(const Edge& edge, const std::vector<Vec3>& vertices) {
    const Vec3& start = vertices[edge.start_index];
    const Vec3& end = vertices[edge.end_index];
    
    const float ALIGNMENT_TOLERANCE = 1e-4f; // 用于容忍浮点数误差
    bool is_horizontal = std::abs(start.y - end.y) < ALIGNMENT_TOLERANCE;
    bool is_vertical = std::abs(start.x - end.x) < ALIGNMENT_TOLERANCE;
    
    return is_horizontal || is_vertical;
}

// 辅助函数：根据一个顶点索引找到它所属的边，并返回该边的材质名称
// 注意：这个实现假设一个边只属于一个面（即轮廓边），对于内部边可能不准确
std::string find_material_for_edge(const ObjModel& obj_model, const Edge& edge) {
    for (const auto& face : obj_model.faces) {
        bool start_found = false;
        bool end_found = false;
        for (const auto& vert_index : face.v) {
            if (vert_index == edge.start_index) start_found = true;
            if (vert_index == edge.end_index) end_found = true;
        }
        if (start_found && end_found) {
            return face.material_name;
        }
    }
    return ""; // 未找到
}

// 修改 collect_samples_from_edges，传递原始材质名
void collect_samples_from_edges(
    const ObjModel& obj_model,
    const std::vector<Edge>& boundary_edges, // 只传入轮廓边
    const TextureMap& texture_map,
    float voxel_size,
    const std::map<std::string, MaterialProfile>& material_profiles,
    PaletteManager& palette_manager)
{
    Logger::info(Message::get("START_EDGE_SAMPLING"));

    for (const auto& edge : boundary_edges) {
        // 1. 确定这条边使用的材质
        std::string material_name = find_material_for_edge(obj_model, edge);
        const auto& mat_it = obj_model.materials.find(material_name);
        if (mat_it == obj_model.materials.end()) continue;

        // 2. 获取物理标签
        auto profile_it = material_profiles.find(material_name);
        const std::string& td_note = (profile_it != material_profiles.end()) ? profile_it->second.td_note : "";
        if (td_note.empty()) continue;

        // 3. 获取边的顶点和UV坐标
        const Vec3& start_pos = obj_model.vertices[edge.start_index];
        const Vec3& end_pos = obj_model.vertices[edge.end_index];

        // 寻找对应的UV
        const Vec2* uv_start_ptr = nullptr;
        const Vec2* uv_end_ptr = nullptr;
        for (const auto& face : obj_model.faces) {
            auto it_start = std::find(face.v.begin(), face.v.end(), edge.start_index);
            auto it_end = std::find(face.v.begin(), face.v.end(), edge.end_index);
            if (it_start != face.v.end() && it_end != face.v.end()) {
                uv_start_ptr = &obj_model.texcoords[face.t[std::distance(face.v.begin(), it_start)]];
                uv_end_ptr = &obj_model.texcoords[face.t[std::distance(face.v.begin(), it_end)]];
                break;
            }
        }
        if (!uv_start_ptr || !uv_end_ptr) continue;
        const Vec2& uv_start = *uv_start_ptr;
        const Vec2& uv_end = *uv_end_ptr;

        // 4. 沿着边进行插值采样
        int num_samples = calc_voxel_strip_placement(edge.length, voxel_size).voxel_count;

        for (int i = 0; i < num_samples; ++i) {
            float t = (i + 0.5f) / num_samples;
            // 插值UV索引和权重
            // 这里我们只用两个端点的UV，等价于重心坐标(u, v, w) = (1-t, t, 0)
            std::vector<int> t_indices = { 
                static_cast<int>(uv_start_ptr - &obj_model.texcoords[0]), 
                static_cast<int>(uv_end_ptr - &obj_model.texcoords[0]), 
                0 // 第三个分量不用
            };
            uint32_t color_rgb = 0;
            std::string mat_name_sample;
            std::string td_note_sample; // <-- 新增变量
            get_voxel_color_and_note(
                obj_model,
                texture_map,
                material_profiles,
                material_name,
                1.0f - t, t, 0.0f, // u, v, w
                t_indices,
                color_rgb,
                mat_name_sample, // <-- 接收原始材质名
                td_note_sample   // <-- 接收物理标签
            );
            palette_manager.collect_sample(color_rgb, mat_name_sample); // <-- 正确传递原始材质名
        }
    }
}

/**
 * @brief 为单个边缘段创建体素条模型.
 * * 该函数负责对一条（可能被分割过的）短边进行体素化。它会沿着边进行纹理插值采样，
 * 然后使用全局的PaletteManager来查询每个采样点对应的、已量化且带有正确材质的颜色索引。
 * * @param obj_model 完整的OBJ模型数据.
 * @param segment 当前要处理的短边段.
 * @param original_edge 未被分割的原始长边，用于正确的UV插值.
 * @param voxel_size 体素的世界尺寸.
 * @param min_x, min_y 世界坐标的最小边界，用于坐标转换.
 * @param texture_map 包含所有已加载纹理的映射.
 * @param palette_manager 全局调色板和材质管理器.
 * @param material_profiles 所有材质的物理属性分类.
 * @param edge_group_index 原始长边的唯一ID，用于命名.
 * @return 包含一个或多个为该边段生成的SubModel的向量.
 */
// 修改 create_edge_models，传递原始材质名
std::vector<SubModel> create_edge_models(
    const ObjModel& obj_model,
    const Edge& segment,
    const Edge& original_edge,
    float voxel_size, float min_x, float min_y,
    const TextureMap& texture_map,
    const PaletteManager& palette_manager,
    const std::map<std::string, MaterialProfile>& material_profiles,
    int edge_group_index,
    int segment_index // 新增
) {
    std::vector<SubModel> subModels;

    // 1. 确定材质、纹理和物理标签 ($TD_note)
    std::string material_name = find_material_for_edge(obj_model, original_edge);
    const auto& mat_it = obj_model.materials.find(material_name);
    if (mat_it == obj_model.materials.end()) return subModels; // 边没有有效材质

    auto profile_it = material_profiles.find(material_name);
    if (profile_it == material_profiles.end()) return subModels; // 材质没有物理分类
    const std::string& td_note = profile_it->second.td_note;


    // 2. 获取原始长边的顶点和UV坐标
    const Vec3& orig_start_pos = obj_model.vertices[original_edge.start_index];
    const Vec3& seg_start_pos = obj_model.vertices[segment.start_index];

    const Vec2* uv_start_orig_ptr = nullptr;
    const Vec2* uv_end_orig_ptr = nullptr;
    for (const auto& face : obj_model.faces) {
        auto it_start = std::find(face.v.begin(), face.v.end(), original_edge.start_index);
        auto it_end = std::find(face.v.begin(), face.v.end(), original_edge.end_index);
        if (it_start != face.v.end() && it_end != face.v.end()) {
            uv_start_orig_ptr = &obj_model.texcoords[face.t[std::distance(face.v.begin(), it_start)]];
            uv_end_orig_ptr = &obj_model.texcoords[face.t[std::distance(face.v.begin(), it_end)]];
            break;
        }
    }
    if (!uv_start_orig_ptr || !uv_end_orig_ptr) return subModels; // 找不到UV坐标
    const Vec2& uv_start_orig = *uv_start_orig_ptr;
    const Vec2& uv_end_orig = *uv_end_orig_ptr;


    // 3. 计算当前短边段在原始长边上的起止比例 (t_start, t_end)
    float dist_from_start = std::sqrt(pow(seg_start_pos.x - orig_start_pos.x, 2) + pow(seg_start_pos.y - orig_start_pos.y, 2));
    float t_start = (original_edge.length > 1e-6) ? (dist_from_start / original_edge.length) : 0.0f;
    float t_end = (original_edge.length > 1e-6) ? ((dist_from_start + segment.length) / original_edge.length) : 1.0f;
    t_start = std::max(0.0f, std::min(1.0f, t_start));
    t_end = std::max(0.0f, std::min(1.0f, t_end));

    // 4. 计算体素条长度和中心偏移
    VoxelStripPlacement placement = calc_voxel_strip_placement(segment.length, voxel_size);
    int totalVoxels = placement.voxel_count;

    // 我们将把体素条分割成不超过MAX_VOX_SIZE的块
    int numSubModels = (totalVoxels + MAX_VOX_SIZE - 1) / MAX_VOX_SIZE;

    for (int i = 0; i < numSubModels; ++i) {
        int startVoxelOffset = i * MAX_VOX_SIZE;
        int subLength = std::min(MAX_VOX_SIZE, totalVoxels - startVoxelOffset);
        if (subLength <= 0) continue;

        uint8_t* voxelData = (uint8_t*)malloc(subLength);
        if (!voxelData) continue;
        
        // 为体素条中的每个体素进行UV插值和颜色采样
        for (int j = 0; j < subLength; ++j) {
            float t_local = (float)(startVoxelOffset + j + 0.5f) / (float)totalVoxels;
            float t_global = t_start + t_local * (t_end - t_start);

            // 插值UV索引和权重
            // 这里我们只用两个端点的UV，等价于重心坐标(u, v, w) = (1-t, t, 0)
            float u = 1.0f - t_global;
            float v = t_global;
            float w = 0.0f;
            std::vector<int> t_indices = {
                static_cast<int>(uv_start_orig_ptr - &obj_model.texcoords[0]),
                static_cast<int>(uv_end_orig_ptr - &obj_model.texcoords[0]),
                0 // 第三个分量不用
            };

            uint32_t color_rgb = 0;
            std::string mat_name_sample;
            std::string td_note_sample; // <-- 新增变量
            get_voxel_color_and_note(
                obj_model,
                texture_map,
                material_profiles,
                material_name,
                u, v, w,
                t_indices,
                color_rgb,
                mat_name_sample, // <-- 接收原始材质名
                td_note_sample   // <-- 接收物理标签
            );
            voxelData[j] = palette_manager.get_final_index(color_rgb, mat_name_sample); // <-- 正确传递原始材质名
        }
        
        // 5. 组装SubModel
        ogt_vox_model* model = (ogt_vox_model*)malloc(sizeof(ogt_vox_model));
        if (!model) {
            free(voxelData);
            continue;
        }
        memset(model, 0, sizeof(ogt_vox_model));
        model->size_x = subLength;
        model->size_y = 1;
        model->size_z = 1;
        model->voxel_data = voxelData;

        SubModel sub;
        sub.model.reset(model);

        // 设置变换、名称和其他属性
        sub.isEdge = true;
        char name_buffer[64];
        sprintf(name_buffer, "edge_%d_seg_%d", edge_group_index, i);
        sub.name = name_buffer;
        
        sub.edgeInfo.startPos = obj_model.vertices[segment.start_index];
        sub.edgeInfo.endPos = obj_model.vertices[segment.end_index];
        sub.edgeInfo.length = segment.length;

        // === 计算分段体素条的精确中心 ===
        // 1. 分段方向单位向量
        Vec3 dir = {
            sub.edgeInfo.endPos.x - sub.edgeInfo.startPos.x,
            sub.edgeInfo.endPos.y - sub.edgeInfo.startPos.y,
            0.0f
        };
        float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
        if (len > 1e-6f) {
            dir.x /= len;
            dir.y /= len;
        }
        // 2. 法线
        // --- 修复：使用依赖于面的法线，而不是固定的左手法则法线 ---
        // 首先找到这条边属于哪个面
        const Face* parent_face = nullptr;
        for (const auto& face : obj_model.faces) {
            bool start_found = false, end_found = false;
            for (const auto& v_idx : face.v) {
                if (v_idx == segment.start_index) start_found = true;
                if (v_idx == segment.end_index) end_found = true;
            }
            if (start_found && end_found) {
                parent_face = &face;
                break;
            }
        }
        // 如果找到了所属面，则计算正确的、依赖于面朝向的法线
        Vec3 normal = (parent_face) ? 
                      get_edge_polygon_outer_normal(segment, *parent_face, obj_model.vertices) :
                      get_edge_outer_normal(segment, obj_model); // 兜底

        // 计算本分块中心在分段上的t
        float center_offset = (float)startVoxelOffset + subLength / 2.0f;
        float t_center = center_offset / (float)totalVoxels;

        // 3. 分段体素条中心点
        Vec3 seg_center = {
            sub.edgeInfo.startPos.x + (sub.edgeInfo.endPos.x - sub.edgeInfo.startPos.x) * t_center,
            sub.edgeInfo.startPos.y + (sub.edgeInfo.endPos.y - sub.edgeInfo.startPos.y) * t_center,
            sub.edgeInfo.startPos.z + (sub.edgeInfo.endPos.z - sub.edgeInfo.startPos.z) * t_center
        };

        // --- 修复：偏移量修正，以消除拼接边上的棱和缝隙 ---
        // 边体素条沿边的外向法线方向移动，补偿体素条宽度
        float offset = voxel_size * EDGE_OFFSET_MULTIPLIER;
        Vec3 offset_mid = {
            seg_center.x + placement.offset_along_edge * dir.x + normal.x * offset,
            seg_center.y + placement.offset_along_edge * dir.y + normal.y * offset,
            seg_center.z
        };

        sub.transform = ogt_vox_transform_get_identity();
        sub.transform.m30 = (offset_mid.x - min_x) / voxel_size;
        sub.transform.m31 = (offset_mid.y - min_y) / voxel_size;
        sub.transform.m32 = 0.0f;

        subModels.push_back(std::move(sub));
    }

    return subModels;
}

// ==========================================================================================
// 结束：用于创建边缘模型的代码块
// ==========================================================================================

// 第三遍：创建最终的子模型
// 修改 create_final_models，传递原始材质名
std::vector<SubModel> create_final_models(
    ObjModel& obj_model,
    const TextureMap& texture_map,
    float voxel_size,
    const PaletteManager& palette_manager,
    const std::map<std::string, MaterialProfile>& material_profiles,
    const std::vector<Edge>& boundary_edges // 新增参数
)
{
    std::vector<SubModel> allSubModels;

    // 1. 计算边界盒
    float min_x = FLT_MAX, max_x = -FLT_MAX, min_y = FLT_MAX, max_y = -FLT_MAX;
    for (const auto& vertex : obj_model.vertices) {
        min_x = std::min(min_x, vertex.x); max_x = std::max(max_x, vertex.x);
        min_y = std::min(min_y, vertex.y); max_y = std::max(max_y, vertex.y);
    }
    min_x -= voxel_size; min_y -= voxel_size;
    max_x += voxel_size; max_y += voxel_size;

    int total_voxel_x = calc_voxel_strip_length((max_x - min_x) , voxel_size);
    int total_voxel_y = calc_voxel_strip_length((max_y - min_y) , voxel_size);

    // 2. 创建平面子模型
    int numSubModelsX = (total_voxel_x + MAX_VOX_SIZE - 1) / MAX_VOX_SIZE;
    int numSubModelsY = (total_voxel_y + MAX_VOX_SIZE - 1) / MAX_VOX_SIZE;

    // --- 核心修复：创建一个由轮廓边定义的有序顶点列表，用于凹多边形检测 ---
    std::vector<Vec3> boundary_polygon_vertices;
    if (!boundary_edges.empty()) {
        std::vector<Edge> sorted_edges = boundary_edges;
        std::vector<bool> used(sorted_edges.size(), false);
        
        boundary_polygon_vertices.push_back(obj_model.vertices[sorted_edges[0].start_index]);
        int current_vertex_idx = sorted_edges[0].end_index;
        used[0] = true;

        for (size_t i = 1; i < sorted_edges.size(); ++i) {
            bool found_next = false;
            for (size_t j = 1; j < sorted_edges.size(); ++j) {
                if (!used[j]) {
                    if (sorted_edges[j].start_index == current_vertex_idx) {
                        boundary_polygon_vertices.push_back(obj_model.vertices[sorted_edges[j].start_index]);
                        current_vertex_idx = sorted_edges[j].end_index;
                        used[j] = true;
                        found_next = true;
                        break;
                    } else if (sorted_edges[j].end_index == current_vertex_idx) {
                        boundary_polygon_vertices.push_back(obj_model.vertices[sorted_edges[j].end_index]);
                        current_vertex_idx = sorted_edges[j].start_index;
                        used[j] = true;
                        found_next = true;
                        break;
                    }
                }
            }
            if (!found_next) break; // 无法构成闭环
        }
    }
    // 如果无法从轮廓边构建多边形，则退回到使用所有顶点（旧的、不准确的方法）
    const std::vector<Vec3>& polygon_to_test = boundary_polygon_vertices.empty() ? obj_model.vertices : boundary_polygon_vertices;


    auto find_triangle_for_point = [&](float px, float py, float& out_u, float& out_v, float& out_w) -> const Face* {
        for (const auto& face : obj_model.faces) {
            const Vec3& v0 = obj_model.vertices[face.v[0]];
            const Vec3& v1 = obj_model.vertices[face.v[1]];
            const Vec3& v2 = obj_model.vertices[face.v[2]];
            if (point_in_triangle(px, py, v0.x, v0.y, v1.x, v1.y, v2.x, v2.y, out_u, out_v, out_w)) {
                return &face;
            }
        }
        return nullptr;
    };

    for (int subY = 0; subY < numSubModelsY; subY++) {
        for (int subX = 0; subX < numSubModelsX; subX++) {
            int startX = subX * MAX_VOX_SIZE;
            int startY = subY * MAX_VOX_SIZE;
            int subSizeX = std::min(MAX_VOX_SIZE, total_voxel_x - startX);
            int subSizeY = std::min(MAX_VOX_SIZE, total_voxel_y - startY);

            std::vector<uint8_t> tempVoxelData(subSizeX * subSizeY, 0);
            bool has_solid_voxel = false;

            for (int vy = 0; vy < subSizeY; ++vy) {
                for (int vx = 0; vx < subSizeX; ++vx) {
                    float cell_min_x = min_x + (startX + vx) * voxel_size;
                    float cell_min_y = min_y + (startY + vy) * voxel_size;
                    float cell_max_x = cell_min_x + voxel_size;
                    float cell_max_y = cell_min_y + voxel_size;
                    float cx = (cell_min_x + cell_max_x) * 0.5f;
                    float cy = (cell_min_y + cell_max_y) * 0.5f;

                    const Face* hit_face = nullptr;
                    float u = 0, v = 0, w = 0;
                    // --- 核心修复：使用正确的轮廓多边形进行内部判断 ---
                    if (is_point_in_polygon(cx, cy, polygon_to_test)) {
                        // 找到包含该点的三角形（用于采样颜色和材质）
                        hit_face = find_triangle_for_point(cx, cy, u, v, w);
                    }
                    
                    // === 核心修复：实现高精度修剪逻辑 ===
                    bool skip = false;
                    if (hit_face) {
                        const float voxel_bounding_radius = voxel_size * 0.70710678118f; // sqrt(2)/2

                        // 1. 遍历所有轮廓边，寻找任何一个导致“越界”的有效边
                        for (const auto& edge : boundary_edges) {
                            const Vec3& p1 = obj_model.vertices[edge.start_index];
                            const Vec3& p2 = obj_model.vertices[edge.end_index];

                            // a. 检查垂点是否在边上
                            float dx = p2.x - p1.x, dy = p2.y - p1.y;
                            float len2 = dx*dx + dy*dy;
                            if (len2 < 1e-10f) continue; // 边长为0
                            float t = ((cx - p1.x) * dx + (cy - p1.y) * dy) / len2;
                            if (t < 0.0f || t > 1.0f) {
                                continue; // 垂点在延长线上，此边无效
                            }
                            float proj_x = p1.x + t * dx;
                            float proj_y = p1.y + t * dy;

                            // b. 检查视线是否被遮挡
                            bool is_occluded = false;
                            for (const auto& other_edge : boundary_edges) {
                                if (&edge == &other_edge) continue; // 不与自身比较
                                const Vec3& op1 = obj_model.vertices[other_edge.start_index];
                                const Vec3& op2 = obj_model.vertices[other_edge.end_index];
                                // 简单的线段相交测试
                                auto cross_product = [](float x1, float y1, float x2, float y2) { return x1*y2 - x2*y1; };
                                float d1 = cross_product(op2.x-op1.x, op2.y-op1.y, cx-op1.x, cy-op1.y);
                                float d2 = cross_product(op2.x-op1.x, op2.y-op1.y, proj_x-op1.x, proj_y-op1.y);
                                float d3 = cross_product(proj_x-cx, proj_y-cy, op1.x-cx, op1.y-cy);
                                float d4 = cross_product(proj_x-cx, proj_y-cy, op2.x-cx, op2.y-cy);
                                if (d1 * d2 < 0 && d3 * d4 < 0) {
                                    is_occluded = true;
                                    break;
                                }
                            }
                            if (is_occluded) {
                                continue; // 视线被遮挡，此边无效
                            }

                            // c. 计算到虚拟边界的有符号距离并判断是否越界
                            const Face* edge_parent_face = nullptr;
                            for (const auto& face : obj_model.faces) {
                                bool start_found = false, end_found = false;
                                for (const auto& v_idx : face.v) {
                                    if (v_idx == edge.start_index) start_found = true;
                                    if (v_idx == edge.end_index) end_found = true;
                                }
                                if (start_found && end_found) { edge_parent_face = &face; break; }
                            }
                            if (!edge_parent_face) continue;

                            Vec3 normal = get_edge_polygon_outer_normal(edge, *edge_parent_face, obj_model.vertices);
                            Vec3 vec_to_center = {cx - proj_x, cy - proj_y, 0.0f};
                            float signed_dist = vec_to_center.x * normal.x + vec_to_center.y * normal.y;
                            float const EPSILON = 0.03f; // 容忍误差
                            float dist_to_virtual_edge = signed_dist - (voxel_size * EDGE_OFFSET_MULTIPLIER) - EPSILON;

                            if (dist_to_virtual_edge + voxel_bounding_radius > 0) {
                                skip = true; // 发现越界，立即判定修剪并跳出循环
                                break;
                            }
                        }
                    }

                    if (hit_face && !skip) {
                        uint32_t color_rgb = 0;
                        std::string mat_name;
                        std::string td_note; // <-- 新增变量
                        get_voxel_color_and_note(
                            obj_model,
                            texture_map,
                            material_profiles,
                            hit_face->material_name,
                            u, v, w,
                            hit_face->t,
                            color_rgb,
                            mat_name, // <-- 接收原始材质名
                            td_note   // <-- 接收物理标签
                        );
                        uint8_t final_index = palette_manager.get_final_index(color_rgb, mat_name); // <-- 正确传递原始材质名
                        if (final_index != 0) {
                            tempVoxelData[vx + vy * subSizeX] = final_index;
                            has_solid_voxel = true;
                        }
                    }
                }
            }

            if (has_solid_voxel) {
                int min_vx = subSizeX, max_vx = -1, min_vy = subSizeY, max_vy = -1;
                for (int vy = 0; vy < subSizeY; ++vy) {
                    for (int vx = 0; vx < subSizeX; ++vx) {
                        if (tempVoxelData[vx + vy * subSizeX] != 0) {
                            min_vx = std::min(min_vx, vx); max_vx = std::max(max_vx, vx);
                            min_vy = std::min(min_vy, vy); max_vy = std::max(max_vy, vy);
                        }
                    }
                }

                int finalSizeX = max_vx - min_vx + 1;
                int finalSizeY = max_vy - min_vy + 1;

                ogt_vox_model* model = (ogt_vox_model*)malloc(sizeof(ogt_vox_model));
                if (!model) {
                    Logger::error(Message::get("CANNOT_ALLOC_MODEL"));
                    continue;
                }
                memset(model, 0, sizeof(ogt_vox_model));
                model->size_x = finalSizeX;
                model->size_y = finalSizeY;
                model->size_z = 1;

                uint8_t* writable_voxel_data = (uint8_t*)malloc((size_t)finalSizeX * finalSizeY * 1);
                if (!writable_voxel_data) {
                    Logger::error(Message::get("CANNOT_ALLOC_VOXEL"));
                    free(model);
                    continue;
                }

                for (int vy = 0; vy < finalSizeY; ++vy) {
                    for (int vx = 0; vx < finalSizeX; ++vx) {
                        writable_voxel_data[vx + vy * finalSizeX] = tempVoxelData[(min_vx + vx) + (min_vy + vy) * subSizeX];
                    }
                }

                model->voxel_data = writable_voxel_data;

                SubModel sub;
                sub.model.reset(model);

                // 计算子模型的旋转中心和全局位置
                int pivotX = std::floor(finalSizeX / 2.0f);
                int pivotY = std::floor(finalSizeY / 2.0f);

                sub.transform = ogt_vox_transform_get_identity();
                sub.transform.m30 = static_cast<float>(startX + min_vx + pivotX);
                sub.transform.m31 = static_cast<float>(startY + min_vy + pivotY);

                sub.name = "plane_" + std::to_string(subX) + "_" + std::to_string(subY);
                sub.isEdge = false;
                allSubModels.push_back(std::move(sub));
            }
        }
    }

    Logger::info(Message::get("PLANE_MODEL_DONE", { {"count", std::to_string(allSubModels.size())} }));

    return allSubModels;
}

// 保存包含NOTE和MATL的VOX场景
bool save_vox_scene_with_notes_and_materials(
    const char* filename, 
    const std::vector<SubModel>& subModels, 
    const ogt_vox_palette& palette,
    const std::map<uint8_t, std::string>& notes,
    const std::map<uint8_t, ogt_vox_matl>& materials) 
{
    if (subModels.empty()) {
        Logger::error(Message::get("CANNOT_SAVE_EMPTY_SCENE"));
        return false;
    }

    ogt_vox_scene scene;
    memset(&scene, 0, sizeof(scene));

    std::vector<const ogt_vox_model*> model_pointers;
    std::vector<ogt_vox_instance>     scene_instances;
    std::vector<ogt_vox_layer>        scene_layers;
    std::vector<ogt_vox_group>        scene_groups;
    std::vector<std::string>          instance_name_storage;
    std::vector<std::string>          notes_storage;
    std::vector<const char*>          color_name_pointers;

    // 1. 填充模型数据
    scene.num_models = subModels.size();
    model_pointers.reserve(scene.num_models);
    for (const auto& sub_model : subModels) {
        model_pointers.push_back(sub_model.model.get());
    }
    scene.models = model_pointers.data();

    // 2. 填充实例数据
    scene.num_instances = subModels.size();
    scene_instances.resize(scene.num_instances);
    instance_name_storage.reserve(scene.num_instances);
    for (size_t i = 0; i < scene.num_instances; ++i) {
        instance_name_storage.push_back(subModels[i].name);
        
        ogt_vox_instance& instance = scene_instances[i];
        
        instance.model_index = i;
        instance.transform = subModels[i].transform;
        instance.layer_index = 0;
        instance.group_index = 0;
        instance.name = instance_name_storage.back().c_str();
        instance.hidden = false;
        memset(&instance.transform_anim, 0, sizeof(instance.transform_anim));
        memset(&instance.model_anim, 0, sizeof(instance.model_anim));
    }
    scene.instances = scene_instances.data();

    // 3. 填充调色板
    scene.palette = palette;

    // 4. 填充材质数据
    for (const auto& pair : materials) {
        uint8_t palette_index = pair.first;
        // 跳过所有保留色
        if (!is_reserved_palette_index(palette_index)) {
            scene.materials.matl[palette_index] = pair.second;
        }
    }

    // 5. 填充颜色名称 (NOTE块)
    if (!notes.empty()) {
        // --- 第1步: 预分配内存，并按“反向”顺序填充所有字符串 ---
        // 这是为了匹配 ogt_vox 库或Teardown的“反向写入”行为。
        // 首先清空，然后预分配内存，以避免循环中的内存重分配（这是上次修复的关键）。
        notes_storage.clear();
        notes_storage.reserve(32);

        // 使用原始的“反向”循环 (31 -> 0)，这才是符合库要求的正确顺序。
        for (int group = 31; group >= 0; group--) {
            std::string note_for_group;
            // 在该组的8个调色板索引中寻找一个有效的note
            for (int i = 1; i <= 8; i++) {
                int palette_index = group * 8 + i;
                if (palette_index > 255) continue;

                if (is_reserved_palette_index(palette_index)) {
                    continue;
                }

                auto it = notes.find((uint8_t)palette_index);
                if (it != notes.end() && !it->second.empty()) {
                    note_for_group = it->second;
                    break; // 找到即可，该组共用一个note
                }
            }
            // 依次将 group 31, 30, ..., 0 的note推入vector
            notes_storage.push_back(note_for_group);
        }

        // 经过上一步，notes_storage[0] 存的是 group 31 的note, 
        // notes_storage[1] 是 group 30 的note, ..., notes_storage[31] 是 group 0 的note。
        // 这正是库所期望的“颠倒的”顺序。

        // --- 第2步: 在所有字符串的内存地址都稳定后，安全地获取它们的指针 ---
        color_name_pointers.clear();
        color_name_pointers.reserve(32);
        for (const auto& note_str : notes_storage) {
            color_name_pointers.push_back(note_str.c_str());
        }

        // --- 第3步: 将最终结果赋给scene ---
        // 库会反向处理这个列表，将 color_name_pointers[31] (第0组的note) 作为文件中的第一个note。
        scene.num_color_names = color_name_pointers.size();
        scene.color_names = color_name_pointers.data();

        // 为了避免混淆，可以移除或修改循环内的打印语句，因为它的打印顺序与最终文件的逻辑顺序是相反的。
    }

    // 6. 填充层数据
    scene_layers.resize(1);
    scene_layers[0].name = "default_layer";
    scene_layers[0].hidden = false;
    memset(&scene_layers[0].color, 0, sizeof(scene_layers[0].color));
    scene.num_layers = scene_layers.size();
    scene.layers = scene_layers.data();

    // 7. 填充组数据
    scene_groups.resize(1);
    scene_groups[0].name = "default_group";
    scene_groups[0].hidden = false;
    scene_groups[0].layer_index = 0;
    scene_groups[0].parent_group_index = k_invalid_group_index;
    scene_groups[0].transform = ogt_vox_transform_get_identity();
    memset(&scene_groups[0].transform_anim, 0, sizeof(scene_groups[0].transform_anim));
    scene.num_groups = scene_groups.size();
    scene.groups = scene_groups.data();

    // 8. 创建场景并输出到文件
    uint32_t buffer_size = 0;
    uint8_t* buffer = ogt_vox_write_scene(&scene, &buffer_size);
    if (!buffer) {
        Logger::error(Message::get("CANNOT_GEN_VOX_SCENE"));
        return false;
    }
    
    std::filesystem::path out_path(filename);
    FILE* f = nullptr;
    #ifdef _WIN32
        f = _wfopen(out_path.wstring().c_str(), L"wb");
    #else
        f = fopen(out_path.string().c_str(), "wb");
    #endif
    if (!f) {
        Logger::error(Message::get("CANNOT_CREATE_OUTPUT", { {"filename", out_path.generic_string()} })); // <-- 修改
        ogt_vox_free(buffer);
        return false;
    }
    
    fwrite(buffer, 1, buffer_size, f);
    fclose(f);
    ogt_vox_free(buffer);
    
    Logger::info(Message::get("SAVE_VOX_SUCCESS", { {"filename", out_path.generic_string()} })); // <-- 修改
    return true;
}

int main(int, char**) {
    // --- MinGW/Windows Unicode Path Solution ---
    // 1. Get the command line as a wide (UTF-16) string
    LPWSTR command_line = GetCommandLineW();

    // 2. Parse the wide string into wide arguments (argv-style)
    int argc_w;
    LPWSTR* argv_w = CommandLineToArgvW(command_line, &argc_w);
    if (!argv_w) {
        // Fallback or error
        return 1;
    }

    // 3. Convert wide arguments to UTF-8 for internal use
    std::vector<std::string> utf8_args;
    std::vector<char*> utf8_argv;
    for (int i = 0; i < argc_w; ++i) {
        utf8_args.push_back(wstring_to_utf8(argv_w[i]));
    }
    for (int i = 0; i < argc_w; ++i) {
        utf8_argv.push_back(const_cast<char*>(utf8_args[i].c_str()));
    }
    
    // Free the memory allocated by CommandLineToArgvW
    LocalFree(argv_w);
    // --- End of Unicode Solution ---

    // 1.1 Parse command line (now using clean UTF-8 arguments)
    CommandLineArgs args = parse_command_line(argc_w, utf8_argv.data());

    Logger::verbose = args.verbose;

    // 1.2 Initialize multi-language environment
    Message::load(args.lang);

    // 2. Parse OBJ file
    ObjModel obj_model;
    // Use std::filesystem::path to handle the UTF-8 string correctly
    if (!parse_obj_file(std::filesystem::path(args.input_file), obj_model)) {
        return 1;
    }

    // 3. 材质分析
    Logger::info(Message::get("PHASE_1_ANALYZE_MATERIAL"));

    // --- 修改：传递新的参数给 classify_materials ---
    auto material_profiles = classify_materials(obj_model.materials, args.material_maps, args.material_properties);

    // 4. 处理纹理目录
    std::string texture_dir = find_texture_directory(args.input_file, args);

    // 5. 加载所有用到的纹理图片
    TextureMap texture_map = load_all_textures(obj_model, texture_dir);

    // 6. 初始化调色板管理器
    PaletteManager palette_manager;

    // 7. 全局采样阶段 (第一遍)
    Logger::info(Message::get("PHASE_2_GLOBAL_SAMPLING"));
    float min_x = FLT_MAX, max_x = -FLT_MAX, min_y = FLT_MAX, max_y = -FLT_MAX;
    for (const auto& vertex : obj_model.vertices) {
        min_x = std::min(min_x, vertex.x); max_x = std::max(max_x, vertex.x);
        min_y = std::min(min_y, vertex.y); max_y = std::max(max_y, vertex.y);
    }
    min_x -= args.voxel_size; min_y -= args.voxel_size;
    max_x += args.voxel_size; max_y += args.voxel_size;
    int total_voxel_x = calc_voxel_strip_length((max_x - min_x) , args.voxel_size);
    int total_voxel_y = calc_voxel_strip_length((max_y - min_y) , args.voxel_size);

    // 7.1 从平面采样
    collect_samples_from_model(obj_model, texture_map, args.voxel_size, material_profiles, palette_manager, min_x, min_y, total_voxel_x, total_voxel_y);
    
    // 7.2 << 新增：识别轮廓边 >>
    std::map<std::pair<int, int>, int> edge_counts;
    for (const auto& edge : obj_model.original_edges) {
        std::pair<int, int> key = {std::min(edge.start_index, edge.end_index), std::max(edge.start_index, edge.end_index)};
        edge_counts[key]++;
    }
    std::vector<Edge> boundary_edges;
    for (const auto& edge : obj_model.original_edges) {
        std::pair<int, int> key = {std::min(edge.start_index, edge.end_index), std::max(edge.start_index, edge.end_index)};
        if (edge_counts[key] == 1) {
            Edge e = edge;
            e.is_aligned = is_edge_aligned(edge, obj_model.vertices);
            boundary_edges.push_back(e);
        }
    }
    Logger::info(Message::get("FOUND_EDGES", { {"total_edges", std::to_string(obj_model.original_edges.size())}, {"boundary_edges", std::to_string(boundary_edges.size())} }));
    
    // 7.3 << 新增：从轮廓边采样 >>
    collect_samples_from_edges(obj_model, boundary_edges, texture_map, args.voxel_size, material_profiles, palette_manager);

    // 8. 量化阶段 (第二遍)
    Logger::info(Message::get("PHASE_3_COLOR_QUANTIZATION"));
    palette_manager.process_and_quantize(material_profiles, obj_model.materials, args);

    // 9. 创建最终模型 (第三遍)
    Logger::info(Message::get("PHASE_4_CREATE_FINAL_MODEL"));

    // 9.1 创建平面模型
    std::vector<SubModel> allSubModels = create_final_models(obj_model, texture_map, args.voxel_size, palette_manager, material_profiles, boundary_edges);

    // 9.2 << 新增：创建边缘模型 >>
    std::vector<SubModel> edgeSubModels;
    int edge_group_index = 0;
    // (这里的 boundary_edges 是第2步中已经识别出的轮廓边)
    for (const auto& edge : boundary_edges) {
        /*
         平面的轴对齐边在体素化后可能存在误差，仍旧需要边体素条来完善视觉效果
        */
        // if (is_edge_aligned(edge, obj_model.vertices)) {
        //      continue;
        // }

        std::vector<Edge> segments = split_single_edge(edge, obj_model, args.voxel_size);

        for (size_t seg_idx = 0; seg_idx < segments.size(); ++seg_idx) {
            const auto& segment = segments[seg_idx];
            std::vector<SubModel> models = create_edge_models(
                obj_model, segment, edge, args.voxel_size, min_x, min_y,
                texture_map, palette_manager, material_profiles,
                edge_group_index,
                static_cast<int>(seg_idx) // 传递唯一的segment_index
            );
            edgeSubModels.insert(edgeSubModels.end(),
                             std::make_move_iterator(models.begin()),
                             std::make_move_iterator(models.end()));
        }
        edge_group_index++;
    }
    Logger::info(Message::get("EDGE_MODEL_DONE", { {"count", std::to_string(edgeSubModels.size())} }));

    // 9.3 合并所有子模型
    allSubModels.insert(allSubModels.end(),
                        std::make_move_iterator(edgeSubModels.begin()),
                        std::make_move_iterator(edgeSubModels.end()));

    if (allSubModels.empty()) {
        Logger::error(Message::get("NO_VALID_SUBMODEL"));
        return 1;
    }
    
    // 10. 保存文件
    std::filesystem::path output_path(args.output_file);
    if (output_path.empty()) {
        std::filesystem::path input_path(args.input_file);
        output_path = input_path.parent_path() / (input_path.stem().string() + ".vox");
    }

    // 新增：自动创建输出目录
    {
        std::filesystem::path out_path(output_path);
        auto out_dir = out_path.parent_path();
        if (!out_dir.empty() && !std::filesystem::exists(out_dir)) {
            std::error_code ec;
            std::filesystem::create_directories(out_dir, ec);
            if (ec) {
                Logger::error_id("CANNOT_CREATE_OUTPUT_DIR", { {"dirname", out_dir.generic_string()} }); // <-- 修改
                return 1;
            }
        }
    }

    auto final_palette = palette_manager.get_palette();
    auto final_notes = palette_manager.get_notes();
    auto final_materials = palette_manager.get_materials();

    // === 新增：计算所有SubModel的世界坐标包围盒中心 ===
    // === 修改：用原始模型顶点的重心作为中心 ===
    float sum_x = 0.0f, sum_y = 0.0f;
    int vertex_count = static_cast<int>(obj_model.vertices.size());
    for (const auto& v : obj_model.vertices) {
        sum_x += v.x;
        sum_y += v.y;
    }
    float center_x = (vertex_count > 0) ? (sum_x / vertex_count) : 0.0f;
    float center_y = (vertex_count > 0) ? (sum_y / vertex_count) : 0.0f;

    if (save_vox_scene_with_notes_and_materials(output_path.string().c_str(), allSubModels, final_palette, final_notes, final_materials)) {

        std::string vox_file_path;
        {
            std::filesystem::path vox_path(output_path);
            std::string vox_basename = vox_path.filename().string();
            vox_file_path = "MOD/vox/" + vox_basename;
        }

        std::filesystem::path xml_path = output_path;
        xml_path.replace_extension(".xml");
        
        // 构造XmlNode树
        XmlNode root{
            "group",
            {{"name", "obj_model"}, {"pos", "0.0 0.0 0.0"}, {"rot", "0.0 0.0 0.0"}},
            [&]{
                std::vector<XmlNode> vox_nodes;
                // --- 新增：计算缩放比例 ---
                float scale_value = args.voxel_size / 0.1f;
                std::string scale_str = std::to_string(scale_value);

                for (const auto& sub : allSubModels) {
                    float world_x = min_x + sub.transform.m30 * args.voxel_size;
                    float world_y = min_y + sub.transform.m31 * args.voxel_size;
                    // The Z coordinate is not directly in the transform for 2D planes,
                    // but it's stored in edgeInfo for edges. For planes, we assume Z=0.
                    float world_z = sub.isEdge ? sub.edgeInfo.startPos.z : 0.0f;

                    // === 新增：应用中心偏移 ===
                    float posX = world_x - center_x;
                    float posY = world_z; // Editor's vertical axis (Y) should be OBJ's Z
                    float posZ = -(world_y - center_y); // Editor's Z is the negative of OBJ's Y
                    if (sub.isEdge) {
                        float edgeAngle = calculate_edge_angle(sub.edgeInfo.startPos, sub.edgeInfo.endPos);
                        vox_nodes.push_back(XmlNode{
                            "vox",
                            {
                                {"pos", std::to_string(posX) + " " + std::to_string(posY) + " " + std::to_string(posZ)},
                                {"rot", "0 " + std::to_string(-edgeAngle) + " 0"},
                                {"scale", scale_str},
                                {"file", vox_file_path},
                                {"object", sub.name}
                            },
                            {}
                        });
                    } else {
                        vox_nodes.push_back(XmlNode{
                            "vox",
                            {
                                {"pos", std::to_string(posX) + " " + std::to_string(posY) + " " + std::to_string(posZ)},
                                {"rot", "0 0 0"},
                                {"scale", scale_str},
                                {"file", vox_file_path},
                                {"object", sub.name}
                            },
                            {}
                        });
                    }
                }
                return vox_nodes;
            }()
        };

        // 生成XML文件
        if (generate_xml_from_tree(xml_path.string(), root)) {
            Logger::info(Message::get("SAVE_XML_SUCCESS", { {"filename", xml_path.generic_string()} })); // <-- 修改
        } else {
            Logger::error(Message::get("CANNOT_GEN_XML"));
        }
    } else {
        Logger::error(Message::get("SAVE_VOX_FAIL"));
        return 1;
    }

    return 0;
}
