#pragma once
#include <string>
#include <vector> // 确保包含 <vector>

// 命令行参数结构
struct CommandLineArgs {
    std::string input_file;
    std::string texture_file;
    std::string output_file;
    float voxel_size = 0.1f;
    std::string lang = "en";
    bool verbose = false;
    std::vector<std::string> material_maps;
    // --- 新增：存储自定义材质属性 ---
    std::vector<std::string> material_properties; 
};

// 命令行参数解析
CommandLineArgs parse_command_line(int argc, char** argv);