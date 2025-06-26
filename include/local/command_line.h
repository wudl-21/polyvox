#pragma once
#include <string>

// 命令行参数结构 - 移除 note_align8
struct CommandLineArgs {
    std::string input_file;     // OBJ输入文件
    std::string texture_file;   // 纹理文件（可选，默认自动检测）
    std::string output_file;    // VOX输出文件（可选，默认为输入文件同名）
    float voxel_size = 0.1f;    // 体素尺寸（可选，默认0.1）
    std::string lang = "zh";
};

// 命令行参数解析
CommandLineArgs parse_command_line(int argc, char** argv);