#include "third_party/cxxopts.hpp"
#include "command_line.h"
#include "logger.h"
#include "message.h"

// 命令行参数解析
CommandLineArgs parse_command_line(int argc, char** argv) {
    CommandLineArgs args;
    cxxopts::Options options("polyvox", "OBJ/MTL/PNG to VOX/Teardown工具");
    options.add_options()
        ("i,input", "OBJ输入文件", cxxopts::value<std::string>())
        ("t,texture", "纹理文件/目录", cxxopts::value<std::string>()->default_value(""))
        ("o,output", "VOX输出文件", cxxopts::value<std::string>()->default_value(""))
        ("s,size", "单位体素尺寸（米）", cxxopts::value<float>()->default_value("0.1"))
        ("l,lang", "语言(zh/en)", cxxopts::value<std::string>()->default_value("zh"))
        ("h,help", "显示帮助");

    auto result = options.parse(argc, argv);

    if (result.count("help") || argc < 2) {
        Logger::info(Message::get("CMD_HELP", {{"help", options.help()}}));
        exit(0);
    }

    if (result.count("input")) args.input_file = result["input"].as<std::string>();
    else {
        Logger::error(Message::get("CMD_MUST_INPUT"));
        exit(1);
    }
    args.texture_file = result["texture"].as<std::string>();
    args.output_file = result["output"].as<std::string>();
    args.voxel_size = result["size"].as<float>();
    args.lang = result["lang"].as<std::string>();
    return args;
}