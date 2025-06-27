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

// 智能调色板管理器
class PaletteManager {
public:
    // 构造函数
    PaletteManager();

    // 收集一个从模型表面采样到的点
    void collect_sample(uint32_t color_rgb, const std::string& td_note);

    // 执行颜色量化和调色板分配
    void process_and_quantize(const std::map<std::string, MaterialProfile>& profiles, const CommandLineArgs& args);

    // 获取一个原始颜色在最终调色板中的索引
    uint8_t get_final_index(uint32_t original_color, const std::string& td_note) const;

    // 获取最终生成的调色板、注释和材质
    ogt_vox_palette get_palette() const;
    const std::map<uint8_t, std::string>& get_notes() const;
    const std::map<uint8_t, ogt_vox_matl>& get_materials() const;

private:
    std::vector<ColorSample> sample_pool;
    
    ogt_vox_palette final_palette = {};
    std::map<uint8_t, std::string> final_notes;
    std::map<uint8_t, ogt_vox_matl> final_materials;
    
    // 重映射表: {原始颜色, 物理Note} -> 最终调色板索引
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

void PaletteManager::collect_sample(uint32_t color_rgb, const std::string& td_note) {
    sample_pool.push_back({color_rgb, td_note});
}

// --- 2. PaletteManager::process_and_quantize 强制每个材质分配槽位向上对齐到8的倍数 ---
void PaletteManager::process_and_quantize(const std::map<std::string, MaterialProfile>& profiles, const CommandLineArgs& args) {
    if (sample_pool.empty()) {
        Logger::warn(Message::get("SAMPLE_POOL_EMPTY"));
        return;
    }

    std::map<std::string, MaterialProfile> profiles_by_note;
    for (const auto& pair : profiles) {
        profiles_by_note[pair.second.td_note] = pair.second;
    }

    std::map<std::string, std::vector<uint32_t>> colors_by_note;
    for (const auto& sample : sample_pool) {
        colors_by_note[sample.td_note].push_back(sample.color_rgb);
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
    for (auto const& [note, colors] : colors_by_note) {
        std::vector<uint32_t> temp_colors = colors;
        std::sort(temp_colors.begin(), temp_colors.end());
        int count = std::unique(temp_colors.begin(), temp_colors.end()) - temp_colors.begin();
        unique_color_counts[note] = count;
        total_unique_colors += count;
    }

    // 强制每个材质分配槽位向上对齐到8的倍数，至少8个
    std::map<std::string, int> slots_for_note;
    int slots_assigned = 0;
    std::vector<std::string> notes_order;
    for (auto const& [note, count] : unique_color_counts) {
        notes_order.push_back(note);
        float proportion = (float)count / (float)total_unique_colors;
        int allocated_slots = std::max(1, static_cast<int>(proportion * total_available_slots));
        // 强制向上对齐到8的倍数
        if (allocated_slots % 8 != 0) allocated_slots = ((allocated_slots / 8) + 1) * 8;
        if (allocated_slots == 0 && count > 0) allocated_slots = 8;
        slots_for_note[note] = allocated_slots;
        slots_assigned += allocated_slots;
    }

    // 补齐或缩减逻辑，步长为8
    if (slots_assigned < total_available_slots) {
        int remain = total_available_slots - slots_assigned;
        size_t idx = 0;
        while (remain > 0) {
            slots_for_note[notes_order[idx % notes_order.size()]] += 8;
            remain -= 8;
            idx++;
        }
    } else if (slots_assigned > total_available_slots) {
        int over = slots_assigned - total_available_slots;
        while (over > 0) {
            for (auto& note : notes_order) {
                if (slots_for_note[note] > 8 && over > 0) {
                    slots_for_note[note] -= 8;
                    over -= 8;
                }
            }
        }
    }

    slots_assigned = 0;
    for (auto const& [note, slots] : slots_for_note) slots_assigned += slots;
    Logger::info(Message::get("PALETTE_SLOTS_ASSIGNED", { {"count", std::to_string(slots_assigned)} }));

    // 4. 对每组进行K-Means量化
    uint8_t current_palette_index = 9;
    for (auto const& [note, original_colors] : colors_by_note) {
        int k = slots_for_note[note];
        if (k == 0 || original_colors.empty()) continue;

        Logger::info(Message::get("PROCESS_MATERIAL", { {"note", note.empty() ? "Default" : note}, {"orig", std::to_string(unique_color_counts[note])}, {"quant", std::to_string(k)} }));

        std::vector<uint32_t> unique_colors = original_colors;
        std::sort(unique_colors.begin(), unique_colors.end());
        unique_colors.erase(std::unique(unique_colors.begin(), unique_colors.end()), unique_colors.end());

        // 只调用 ColorQuantizer
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
            final_notes[current_palette_index] = note;
            unsigned char r, g, b;
            unpack_color(center_color, r, g, b);
            final_palette.color[current_palette_index] = {r, g, b, 255};
            auto it = profiles_by_note.find(note);
            if (it != profiles_by_note.end()) {
                final_materials[current_palette_index] = it->second.vox_material;
            }
            for (uint32_t original_color : unique_colors) {
                float min_dist = FLT_MAX;
                uint8_t best_index = 0;
                uint32_t best_center = 0;
                for (size_t j = 0; j < centers.size(); ++j) {
                    float dist = color_distance(original_color, centers[j]);
                    if (dist < min_dist) {
                        min_dist = dist;
                        best_center = centers[j];
                    }
                }
                if (best_center == center_color) {
                    remap_table[{original_color, note}] = current_palette_index;
                }
            }
            current_palette_index++;
        }
    }
}

uint8_t PaletteManager::get_final_index(uint32_t original_color, const std::string& td_note) const {
    auto it = remap_table.find({original_color, td_note});
    if (it != remap_table.end()) {
        return it->second;
    }
    return 0; 
}

ogt_vox_palette PaletteManager::get_palette() const { return final_palette; }
const std::map<uint8_t, std::string>& PaletteManager::get_notes() const { return final_notes; }
const std::map<uint8_t, ogt_vox_matl>& PaletteManager::get_materials() const { return final_materials; }

// 材质分类器
std::map<std::string, MaterialProfile> classify_materials(const std::map<std::string, MtlMaterial>& mtl_materials) {
    std::map<std::string, MaterialProfile> profiles;

    for (const auto& pair : mtl_materials) {
        const std::string& mtl_name = pair.first;
        const MtlMaterial& mtl = pair.second;
        MaterialProfile profile;
        
        // 为材质属性清零，确保一个干净的开始
        memset(&profile.vox_material, 0, sizeof(profile.vox_material));

        // 修正: 改进材质分类逻辑
        // 1. 将材质名称转为小写以便不区分大小写地进行比较
        std::string lower_name = mtl_name;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
                       [](unsigned char c){ return std::tolower(c); });

        // 2. 根据名称和属性推断物理类型 ($TD_note) 和基础渲染类型
        if (mtl.d < 0.9f) {
            profile.td_note = "$TD_glass";
            profile.vox_material.type = ogt_matl_type_glass;
        } else if (lower_name.find("wood") != std::string::npos) {
            profile.td_note = "$TD_wood";
            profile.vox_material.type = ogt_matl_type_diffuse;
        } else if (lower_name.find("metal") != std::string::npos) { // 优先根据名称判断金属
            profile.td_note = "$TD_metal";
            profile.vox_material.type = ogt_matl_type_metal;
        } else if (mtl.Ks.x > 0.5f && mtl.Ns > 50.0f) { // 其次，放宽对高光属性的判断
            profile.td_note = "$TD_metal";
            profile.vox_material.type = ogt_matl_type_metal;
        }else if (lower_name.find("brick") != std::string::npos) {
            profile.td_note = "$TD_masonry";
            profile.vox_material.type = ogt_matl_type_diffuse;
        }else if (lower_name.find("concrete") != std::string::npos) {
            profile.td_note = "$TD_masonry";
            profile.vox_material.type = ogt_matl_type_diffuse;
        } else if (lower_name.find("vegetation") != std::string::npos) {
            profile.td_note = "$TD_foliage";
            profile.vox_material.type = ogt_matl_type_diffuse;
        } else {
            profile.td_note = "$TD_metal"; // 默认材质，假设为金属
            profile.vox_material.type = ogt_matl_type_diffuse;
        }

        // 3. 转换渲染属性 -> ogt_vox_matl，并设置 content_flags
        if (profile.vox_material.type == ogt_matl_type_metal) {
            profile.vox_material.metal = 1.0f; // 完全金属
            profile.vox_material.content_flags |= k_ogt_vox_matl_have_metal;
        }

        // 粗糙度 (适用于多数类型)
        profile.vox_material.rough = std::max(0.001f, 1.0f - (mtl.Ns / 1000.0f));
        profile.vox_material.content_flags |= k_ogt_vox_matl_have_rough;

        // 镜面反射 (适用于多数类型)
        profile.vox_material.spec = (mtl.Ks.x + mtl.Ks.y + mtl.Ks.z) / 3.0f;
        if (profile.vox_material.spec > 0.0f) {
            profile.vox_material.content_flags |= k_ogt_vox_matl_have_spec;
        }

        // 折射率 (适用于玻璃/透明材质)
        profile.vox_material.ior = mtl.Ni;
        profile.vox_material.content_flags |= k_ogt_vox_matl_have_ior;

        // Alpha/不透明度
        profile.vox_material.alpha = mtl.d;
        if (profile.vox_material.alpha < 1.0f) {
            profile.vox_material.content_flags |= k_ogt_vox_matl_have_alpha;
        }
        
        // 对于玻璃材质，设置透明度属性
        if (profile.vox_material.type == ogt_matl_type_glass) {
            profile.vox_material.trans = 1.0f - profile.vox_material.alpha;
            profile.vox_material.content_flags |= k_ogt_vox_matl_have_trans;
        }

        // 自发光
        profile.vox_material.emit = (mtl.Ke.x + mtl.Ke.y + mtl.Ke.z) / 3.0f;
        if (profile.vox_material.emit > 0.0f) {
            profile.vox_material.type = ogt_matl_type_emit; // 如果发光，则覆盖类型为自发光
            profile.vox_material.content_flags |= k_ogt_vox_matl_have_emit;
            // flux 是辐射通量，让它和发光强度成正比
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
    default_profile.vox_material.rough = 0.8f; // 为通用材质设置一个合理的默认值
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
bool parse_mtl_file(const std::string& mtl_path, std::map<std::string, MtlMaterial>& materials) {
    std::ifstream file(mtl_path);
    if (!file.is_open()) {
        Logger::error(Message::get("CANNOT_OPEN_MTL", { {"filename", mtl_path} }));
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
        else if (token == "Ks" && has_material) {
            iss >> current_material.Ks.x >> current_material.Ks.y >> current_material.Ks.z;
        }
        else if (token == "Ns" && has_material) {
            iss >> current_material.Ns;
        }
        else if (token == "d" && has_material) {
            iss >> current_material.d;
        }
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
bool parse_obj_file(const std::string& filename, ObjModel& model) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        Logger::error(Message::get("CANNOT_OPEN_OBJ", { {"filename", filename} }));
        return false;
    }
    
    std::filesystem::path objPath(filename);
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
        // 修正: 使用 std::filesystem 的路径拼接操作符来安全地构建路径。
        // 这种方法能正确处理 OBJ 文件在当前目录或子目录中的情况。
        std::filesystem::path mtl_full_path = objPath.parent_path() / model.mtl_filename;
        
        Logger::info(Message::get("TRY_LOAD_MTL", { {"filename", mtl_full_path.string()} }));
        if (!parse_mtl_file(mtl_full_path.string(), model.materials)) {
            Logger::warn(Message::get("CANNOT_LOAD_MTL", { {"filename", mtl_full_path.string()} }));
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
    
    Logger::info(Message::get("LOAD_OBJ_SUCCESS", { {"vertex_count", std::to_string(model.vertices.size())}, {"texcoord_count", std::to_string(model.texcoords.size())}, {"face_count", std::to_string(model.faces.size())} }));
    
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
            std::string tex_path_str = tex_path.string();
            if (!std::filesystem::exists(tex_path)) {
                // 尝试只用文件名查找
                tex_path = std::filesystem::path(texture_dir) / std::filesystem::path(mat.diffuse_map).filename();
                tex_path_str = tex_path.string();
            }
            if (std::filesystem::exists(tex_path)) {
                TextureImage img;
                img.data.reset(stbi_load(tex_path_str.c_str(), &img.width, &img.height, &img.channels, 4));
                if (img.data) {
                    tex_map[mat.diffuse_map] = std::move(img);
                    Logger::info(Message::get("TEXTURE_LOADED", { {"filename", tex_path_str} }));
                } else {
                    Logger::warn(Message::get("CANNOT_LOAD_TEXTURE", { {"filename", tex_path_str} }));
                }
            } else {
                Logger::warn(Message::get("TEXTURE_NOT_EXIST", { {"filename", tex_path_str} }));
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

// 第一遍：从模型表面采样颜色和材质
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
                // 查找该面材质对应的纹理
                const auto& mat_it = obj_model.materials.find(center_face->material_name);
                std::string tex_name = (mat_it != obj_model.materials.end()) ? mat_it->second.diffuse_map : "";
                auto tex_it = texture_map.find(tex_name);
                if (tex_it == texture_map.end() || !tex_it->second.data) continue;
                const TextureImage& tex = tex_it->second;

                const Vec2& t0 = obj_model.texcoords[center_face->t[0]];
                const Vec2& t1 = obj_model.texcoords[center_face->t[1]];
                const Vec2& t2 = obj_model.texcoords[center_face->t[2]];
                
                float tex_u = u * t0.u + v * t1.u + w * t2.u;
                float tex_v = u * t0.v + v * t1.v + w * t2.v;
                
                tex_u = std::fmod(tex_u, 1.0f); if (tex_u < 0) tex_u += 1.0f;
                tex_v = std::fmod(tex_v, 1.0f); if (tex_v < 0) tex_v += 1.0f;
                
                int tx = static_cast<int>(tex_u * (tex.width - 1));
                int ty = static_cast<int>((1.0f - tex_v) * (tex.height - 1));
                
                if (tx < 0) tx = 0; if (tx >= tex.width) tx = tex.width - 1;
                if (ty < 0) ty = 0; if (ty >= tex.height) ty = tex.height - 1;
                
                unsigned char* pixel = tex.data.get() + 4 * (ty * tex.width + tx);
                if (pixel[3] > 128) {
                    uint32_t color_rgb = pack_color(pixel[0], pixel[1], pixel[2]);
                    auto profile_it = material_profiles.find(center_face->material_name);
                    const std::string& td_note = (profile_it != material_profiles.end()) ? profile_it->second.td_note : "";
                    palette_manager.collect_sample(color_rgb, td_note);
                }
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

// 沿着所有轮廓边采样颜色和材质
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
        // 1. 确定这条边使用的材质和纹理
        std::string material_name = find_material_for_edge(obj_model, edge);
        const auto& mat_it = obj_model.materials.find(material_name);
        if (mat_it == obj_model.materials.end()) continue;

        std::string tex_name = mat_it->second.diffuse_map;
        auto tex_it = texture_map.find(tex_name);
        if (tex_it == texture_map.end() || !tex_it->second.data) continue;

        const TextureImage& tex = tex_it->second;

        // 2. 获取这条边的物理标签 ($TD_note)
        auto profile_it = material_profiles.find(material_name);
        const std::string& td_note = (profile_it != material_profiles.end()) ? profile_it->second.td_note : "";
        if (td_note.empty()) continue; // 没有物理标签的边不处理

        // 3. 获取边的顶点和UV坐标
        const Vec3& start_pos = obj_model.vertices[edge.start_index];
        const Vec3& end_pos = obj_model.vertices[edge.end_index];

        // 寻找对应的UV（此逻辑来自 texture-edge.cpp）
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
        if (!uv_start_ptr) continue;
        const Vec2& uv_start = *uv_start_ptr;
        const Vec2& uv_end = *uv_end_ptr;

        // 4. 沿着边进行插值采样
        int num_samples = calc_voxel_strip_placement(edge.length, voxel_size).voxel_count;

        for (int i = 0; i < num_samples; ++i) {
            float t = (i + 0.5f) / num_samples;
            // 插值UV
            float tex_u = (1.0f - t) * uv_start.u + t * uv_end.u;
            float tex_v = (1.0f - t) * uv_start.v + t * uv_end.v;

            // 纹理采样
            tex_u = std::fmod(tex_u, 1.0f); if (tex_u < 0) tex_u += 1.0f;
            tex_v = std::fmod(tex_v, 1.0f); if (tex_v < 0) tex_v += 1.0f;

            int tx = static_cast<int>(tex_u * (tex.width - 1));
            int ty = static_cast<int>((1.0f - tex_v) * (tex.height - 1));
            tx = std::max(0, std::min(tex.width - 1, tx));
            ty = std::max(0, std::min(tex.height - 1, ty));

            unsigned char* pixel = tex.data.get() + 4 * (ty * tex.width + tx);
            if (pixel[3] > 128) { // 如果非透明
                uint32_t color_rgb = pack_color(pixel[0], pixel[1], pixel[2]);
                palette_manager.collect_sample(color_rgb, td_note);
            }
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

    std::string tex_name = mat_it->second.diffuse_map;
    auto tex_it = texture_map.find(tex_name);
    if (tex_it == texture_map.end() || !tex_it->second.data) return subModels; // 材质没有有效纹理
    const TextureImage& tex = tex_it->second;

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

            // 插值UV
            float tex_u = (1.0f - t_global) * uv_start_orig.u + t_global * uv_end_orig.u;
            float tex_v = (1.0f - t_global) * uv_start_orig.v + t_global * uv_end_orig.v;

            tex_u = std::fmod(tex_u, 1.0f); if (tex_u < 0) tex_u += 1.0f;
            tex_v = std::fmod(tex_v, 1.0f); if (tex_v < 0) tex_v += 1.0f;

            int tx = static_cast<int>(tex_u * (tex.width - 1));
            int ty = static_cast<int>((1.0f - tex_v) * (tex.height - 1));
            tx = std::max(0, std::min(tex.width - 1, tx));
            ty = std::max(0, std::min(tex.height - 1, ty));

            unsigned char* pixel = tex.data.get() + 4 * (ty * tex.width + tx);
            if (pixel[3] > 128) { // 如果非透明
                uint32_t color_rgb = pack_color(pixel[0], pixel[1], pixel[2]);
                voxelData[j] = palette_manager.get_final_index(color_rgb, td_note);
            } else {
                voxelData[j] = 0; // 透明
            }
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
        sprintf(name_buffer, "edge_%d_seg_%d", edge_group_index, segment_index);
        sub.name = name_buffer;
        
        sub.edgeInfo.startPos = obj_model.vertices[segment.start_index];
        sub.edgeInfo.endPos = obj_model.vertices[segment.end_index];
        sub.edgeInfo.length = segment.length;

        // === 计算体素条放置的精确中心 ===
        // 1. 边方向单位向量
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
        // 2. 法线（逆时针多边形，法线指向内部）
        Vec3 normal = { -dir.y, dir.x, 0.0f };

        // 3. 体素条中心点 = 原边中点 + placement.offset_along_edge * dir + normal * (0.5 * voxel_size)
        Vec3 edge_mid = {
            (sub.edgeInfo.startPos.x + sub.edgeInfo.endPos.x) / 2.0f,
            (sub.edgeInfo.startPos.y + sub.edgeInfo.endPos.y) / 2.0f,
            (sub.edgeInfo.startPos.z + sub.edgeInfo.endPos.z) / 2.0f
        };
        float offset = voxel_size * 0.5f;
        Vec3 offset_mid = {
            edge_mid.x + placement.offset_along_edge * dir.x + normal.x * offset,
            edge_mid.y + placement.offset_along_edge * dir.y + normal.y * offset,
            edge_mid.z
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
                    float world_x = min_x + (startX + vx + 0.5f) * voxel_size;
                    float world_y = min_y + (startY + vy + 0.5f) * voxel_size;

                    // 1. 计算到最近轮廓边的距离
                    float min_dist = FLT_MAX;
                    for (const auto& edge : boundary_edges) {
                        const Vec3& p1 = obj_model.vertices[edge.start_index];
                        const Vec3& p2 = obj_model.vertices[edge.end_index];
                        // 只用XY平面
                        float x0 = p1.x, y0 = p1.y;
                        float x1 = p2.x, y1 = p2.y;
                        float dx = x1 - x0, dy = y1 - y0;
                        float len2 = dx*dx + dy*dy;
                        float t = ((world_x - x0) * dx + (world_y - y0) * dy) / (len2 + 1e-10f);
                        t = std::max(0.0f, std::min(1.0f, t));
                        float proj_x = x0 + t * dx;
                        float proj_y = y0 + t * dy;
                        float dist = std::sqrt((world_x - proj_x)*(world_x - proj_x) + (world_y - proj_y)*(world_y - proj_y));
                        if (dist < min_dist) min_dist = dist;
                    }

                    // 2. 如果距离小于0.5*sqrt(2)*voxel_size + EDGE_EPSILON，则跳过（不填充）
                    const float EDGE_EPSILON = 0.04f;
                    float edge_threshold = 0.5f * 1.41421356f * voxel_size + EDGE_EPSILON;
                    if (min_dist < edge_threshold) continue;

                    // 3. 原有判定与采样逻辑
                    float u, v, w;
                    const Face* center_face = find_triangle_for_point(world_x, world_y, u, v, w);

                    if (center_face) {
                        // 查找该面材质对应的纹理
                        const auto& mat_it = obj_model.materials.find(center_face->material_name);
                        std::string tex_name = (mat_it != obj_model.materials.end()) ? mat_it->second.diffuse_map : "";
                        auto tex_it = texture_map.find(tex_name);
                        if (tex_it == texture_map.end() || !tex_it->second.data) continue;
                        const TextureImage& tex = tex_it->second;

                        const Vec2& t0 = obj_model.texcoords[center_face->t[0]];
                        const Vec2& t1 = obj_model.texcoords[center_face->t[1]];
                        const Vec2& t2 = obj_model.texcoords[center_face->t[2]];

                        float tex_u = u * t0.u + v * t1.u + w * t2.u;
                        float tex_v = u * t0.v + v * t1.v + w * t2.v;

                        tex_u = std::fmod(tex_u, 1.0f); if (tex_u < 0) tex_u += 1.0f;
                        tex_v = std::fmod(tex_v, 1.0f); if (tex_v < 0) tex_v += 1.0f;

                        int tx = static_cast<int>(tex_u * (tex.width - 1));
                        int ty = static_cast<int>((1.0f - tex_v) * (tex.height - 1));

                        if (tx < 0) tx = 0; if (tx >= tex.width) tx = tex.width - 1;
                        if (ty < 0) ty = 0; if (ty >= tex.height) ty = tex.height - 1;

                        unsigned char* pixel = tex.data.get() + 4 * (ty * tex.width + tx);
                        if (pixel[3] > 128) {
                            uint32_t color_rgb = pack_color(pixel[0], pixel[1], pixel[2]);
                            auto profile_it = material_profiles.find(center_face->material_name);
                            const std::string& td_note = (profile_it != material_profiles.end()) ? profile_it->second.td_note : "";

                            if (td_note.empty()) {
                                Logger::warn(Message::get("NO_PHYS_TAG", { {"material", center_face->material_name} }));
                            }

                            uint8_t final_index = palette_manager.get_final_index(color_rgb, td_note);
                            if (final_index != 0) {
                                tempVoxelData[vx + vy * subSizeX] = final_index;
                                has_solid_voxel = true;
                            }
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
    
    FILE* f = fopen(filename, "wb");
    if (!f) {
        Logger::error(Message::get("CANNOT_CREATE_OUTPUT", { {"filename", filename} }));
        ogt_vox_free(buffer);
        return false;
    }
    
    fwrite(buffer, 1, buffer_size, f);
    fclose(f);
    ogt_vox_free(buffer);
    
    Logger::info(Message::get("SAVE_VOX_SUCCESS", { {"filename", filename} }));
    return true;
}

int main(int argc, char** argv) {
    // 1. 解析命令行参数
    CommandLineArgs args = parse_command_line(argc, argv);

    Logger::verbose = args.verbose; // 新增：根据参数设置日志模式

    // 1.1 初始化多语言环境
    Message::load(args.lang);

    // 2. 解析OBJ文件
    ObjModel obj_model;
    if (!parse_obj_file(args.input_file, obj_model)) {
        return 1;
    }

    // 3. 材质分析
    Logger::info(Message::get("PHASE_1_ANALYZE_MATERIAL"));

    auto material_profiles = classify_materials(obj_model.materials);

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
            boundary_edges.push_back(edge);
        }
    }
    Logger::info(Message::get("FOUND_EDGES", { {"total_edges", std::to_string(obj_model.original_edges.size())}, {"boundary_edges", std::to_string(boundary_edges.size())} }));
    
    // 7.3 << 新增：从轮廓边采样 >>
    collect_samples_from_edges(obj_model, boundary_edges, texture_map, args.voxel_size, material_profiles, palette_manager);

    // 8. 量化阶段 (第二遍)
    Logger::info(Message::get("PHASE_3_COLOR_QUANTIZATION"));
    palette_manager.process_and_quantize(material_profiles, args);

    // 9. 创建最终模型 (第三遍)
    Logger::info(Message::get("PHASE_4_CREATE_FINAL_MODEL"));

    // 9.1 创建平面模型
    std::vector<SubModel> allSubModels = create_final_models(obj_model, texture_map, args.voxel_size, palette_manager, material_profiles, boundary_edges);

    // 9.2 << 新增：创建边缘模型 >>
    std::vector<SubModel> edgeSubModels;
    int edge_group_index = 0;
    // (这里的 boundary_edges 是第2步中已经识别出的轮廓边)
    for (const auto& edge : boundary_edges) {
        if (is_edge_aligned(edge, obj_model.vertices)) {
             continue;
        }

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

    // 9.3 合并所有子模型
    allSubModels.insert(allSubModels.end(),
                        std::make_move_iterator(edgeSubModels.begin()),
                        std::make_move_iterator(edgeSubModels.end()));

    if (allSubModels.empty()) {
        Logger::error(Message::get("NO_VALID_SUBMODEL"));
        return 1;
    }
    
    // 10. 保存文件
    std::string output_path = args.output_file;
    if (output_path.empty()) {
        output_path = FileUtils::join(
            FileUtils::getDirectory(args.input_file),
            FileUtils::getStem(args.input_file) + ".vox"
        );
    }

    // 新增：自动创建输出目录
    {
        std::filesystem::path out_path(output_path);
        auto out_dir = out_path.parent_path();
        if (!out_dir.empty() && !std::filesystem::exists(out_dir)) {
            std::error_code ec;
            std::filesystem::create_directories(out_dir, ec);
            if (ec) {
                Logger::error_id("CANNOT_CREATE_OUTPUT_DIR", { {"dirname", out_dir.string()} });
                return 1;
            }
        }
    }

    auto final_palette = palette_manager.get_palette();
    auto final_notes = palette_manager.get_notes();
    auto final_materials = palette_manager.get_materials();

    if (save_vox_scene_with_notes_and_materials(output_path.c_str(), allSubModels, final_palette, final_notes, final_materials)) {

        std::string vox_file_path;
        {
            std::filesystem::path vox_path(output_path);
            std::string vox_basename = vox_path.filename().string();
            vox_file_path = "MOD/vox/" + vox_basename;
        }

        std::string xml_filename = output_path.substr(0, output_path.find_last_of('.')) + ".xml";
        
        // 构造XmlNode树
        XmlNode root{
            "group",
            {{"name", "obj_model"}, {"pos", "0.0 0.0 0.0"}, {"rot", "0.0 0.0 0.0"}},
            [&]{
                std::vector<XmlNode> vox_nodes;
                for (const auto& sub : allSubModels) {
                    if (sub.isEdge) {
                        const Vec3& start = sub.edgeInfo.startPos;
                        const Vec3& end = sub.edgeInfo.endPos;
                        float edgeAngle = calculate_edge_angle(start, end);
                        Vec3 dir = { end.x - start.x, end.y - start.y, 0.0f };
                        float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
                        if (len > 1e-6f) { dir.x /= len; dir.y /= len; }
                        Vec3 normal = { -dir.y, dir.x, 0.0f };
                        float offset = args.voxel_size * 0.5f;
                        float midX = (start.x + end.x) / 2.0f + normal.x * offset;
                        float midY = (start.y + end.y) / 2.0f + normal.y * offset;
                        float posX = (midX - min_x) / args.voxel_size * 0.1f;
                        float posY = -(midY - min_y) / args.voxel_size * 0.1f;
                        vox_nodes.push_back(XmlNode{
                            "vox",
                            {
                                {"pos", std::to_string(posX) + " 0.0 " + std::to_string(posY)},
                                {"rot", "0 " + std::to_string(-edgeAngle) + " 0"},
                                {"file", vox_file_path},
                                {"object", sub.name}
                            },
                            {}
                        });
                    } else {
                        float posX = sub.transform.m30 * 0.1f;
                        float posZ = 0.0f;
                        float posY = -sub.transform.m31 * 0.1f;
                        vox_nodes.push_back(XmlNode{
                            "vox",
                            {
                                {"pos", std::to_string(posX) + " " + std::to_string(posZ) + " " + std::to_string(posY)},
                                {"rot", "0 0 0"},
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
        if (generate_xml_from_tree(xml_filename, root)) {
            Logger::info(Message::get("SAVE_XML_SUCCESS", { {"filename", xml_filename} }));
        } else {
            Logger::error(Message::get("CANNOT_GEN_XML"));
        }
    } else {
        Logger::error(Message::get("SAVE_VOX_FAIL"));
        return 1;
    }

    return 0;
}
