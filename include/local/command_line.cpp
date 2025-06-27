#include "third_party/cxxopts.hpp"
#include "command_line.h"
#include "logger.h"
#include "message.h"

// 命令行参数解析
CommandLineArgs parse_command_line(int argc, char** argv) {
    CommandLineArgs args;

    // 1. 预解析语言参数
    std::string lang = "zh";
    for (int i = 1; i < argc; ++i) {
        if ((std::string(argv[i]) == "-l" || std::string(argv[i]) == "--lang") && i + 1 < argc) {
            lang = argv[i + 1];
            break;
        }
    }
    Message::load(lang); // 先加载语言

    cxxopts::Options options("polyvox", "OBJ/MTL/PNG to VOX/Teardown工具");
    options.add_options()
        ("i,input", Message::get("CMD_ARG_INPUT_DESC"), cxxopts::value<std::string>())
        ("t,texture", Message::get("CMD_ARG_TEXTURE_DESC"), cxxopts::value<std::string>()->default_value(""))
        ("o,output", Message::get("CMD_ARG_OUTPUT_DESC"), cxxopts::value<std::string>()->default_value(".\\output\\output.vox"))
        ("s,size", Message::get("CMD_ARG_SIZE_DESC"), cxxopts::value<float>()->default_value("0.1"))
        ("l,lang", Message::get("CMD_ARG_LANG_DESC"), cxxopts::value<std::string>()->default_value("zh"))
        ("v,verbose", Message::get("CMD_ARG_VERBOSE_DESC"), cxxopts::value<bool>()->default_value("false"))
        ("h,help", Message::get("CMD_ARG_HELP_DESC"));

    auto result = options.parse(argc, argv);

    if (result.contains("help") || argc < 2) {
        std::cout << Message::get("CMD_HELP", { {"help", options.help()} }) << std::endl;
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
    args.verbose = result["verbose"].as<bool>();
    return args;
}