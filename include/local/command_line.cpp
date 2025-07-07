#include "third_party/cxxopts.hpp"
#include "command_line.h"
#include "logger.h"
#include "message.h"

// 命令行参数解析
CommandLineArgs parse_command_line(int argc, char** argv) {
    // 1. 预解析语言参数，以便正确显示帮助信息
    std::string lang = "en";
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-l" || arg == "--lang") && i + 1 < argc) {
            lang = argv[++i];
            break; // 找到即可
        }
    }
    Message::load(lang); // 先加载语言

    cxxopts::Options options("polyvox", "OBJ/MTL/PNG to VOX/Teardown工具");
    options.add_options()
        ("i,input", Message::get("CMD_ARG_INPUT_DESC"), cxxopts::value<std::string>())
        ("t,texture", Message::get("CMD_ARG_TEXTURE_DESC"), cxxopts::value<std::string>()->default_value(""))
        ("o,output", Message::get("CMD_ARG_OUTPUT_DESC"), cxxopts::value<std::string>()->default_value(""))
        ("s,size", Message::get("CMD_ARG_SIZE_DESC"), cxxopts::value<float>()->default_value("0.1"))
        ("l,lang", Message::get("CMD_ARG_LANG_DESC"), cxxopts::value<std::string>()->default_value("en"))
        ("v,verbose", Message::get("CMD_ARG_VERBOSE_DESC"), cxxopts::value<bool>()->default_value("false"))
        // --- 新增：在这里定义 -m/--map 参数 ---
        ("m,map", "Material to TD_note mapping (e.g. \"mat_name:$TD_wood\")", cxxopts::value<std::vector<std::string>>())
        // --- 新增：在这里定义 -p/--property 参数 ---
        ("p,property", "Material property override (e.g. \"mat_name:rough:0.8\")", cxxopts::value<std::vector<std::string>>())
        ("h,help", Message::get("CMD_ARG_HELP_DESC"));

    auto result = options.parse(argc, argv);

    if (result.count("help") || argc < 2) {
        std::cout << Message::get("CMD_HELP", { {"help", options.help()} }) << std::endl;
        exit(0);
    }

    CommandLineArgs args;
    if (result.count("input")) args.input_file = result["input"].as<std::string>();
    else {
        Logger::error(Message::get("CMD_MUST_INPUT"));
        exit(1);
    }
    args.texture_file = result["texture"].as<std::string>();
    args.output_file = result["output"].as<std::string>();
    args.voxel_size = result["size"].as<float>();
    args.lang = result["lang"].as<std::string>();
    args.verbose = result["verbose"].as<bool>();
    
    // --- 新增：从解析结果中获取映射 ---
    if (result.count("map")) {
        args.material_maps = result["map"].as<std::vector<std::string>>();
    }
    // --- 新增：从解析结果中获取属性 ---
    if (result.count("property")) {
        args.material_properties = result["property"].as<std::vector<std::string>>();
    }

    return args;
}