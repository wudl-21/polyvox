#include <fstream>
#include <sstream>
#include <filesystem>
#include <windows.h>
#include "local/message.h"
#include "local/logger.h" // 引入 Logger 以便打印调试信息
#include "third_party/json.hpp"

std::unordered_map<std::string, std::string> Message::messages;
std::string Message::current_lang = "en";

bool Message::load(const std::string& lang) {
    current_lang = lang;

    // 获取可执行文件所在目录
    wchar_t exe_path_w[MAX_PATH];
    GetModuleFileNameW(NULL, exe_path_w, MAX_PATH);
    std::filesystem::path exe_dir = std::filesystem::path(exe_path_w).parent_path();

    std::filesystem::path locale_file_path;

    // 探测路径 1: 打包后的结构 (locale/ 目录在 .exe 旁边)
    std::filesystem::path prod_path = exe_dir / "locale" / (lang + ".json");
    
    // 探测路径 2: 开发环境的结构 (locale/ 目录在 .exe 所在目录的上一级)
    std::filesystem::path dev_path = exe_dir.parent_path() / "locale" / (lang + ".json");

    if (std::filesystem::exists(prod_path)) {
        locale_file_path = prod_path;
    } else if (std::filesystem::exists(dev_path)) {
        locale_file_path = dev_path;
    } else {
        // 两个路径都找不到，打印错误并返回
        // 注意：此时Logger本身可能还未完全加载翻译，所以用英文硬编码错误信息
        Logger::error("Could not find locale file: " + lang + ".json");
        Logger::error("  - Probed (prod): " + prod_path.string());
        Logger::error("  - Probed (dev):  " + dev_path.string());
        return false;
    }
    
    std::ifstream file(locale_file_path);
    if (!file.is_open()) {
        Logger::error("Found but could not open locale file: " + locale_file_path.string());
        return false;
    }

    nlohmann::json j;
    try {
        file >> j;
    } catch (const nlohmann::json::parse_error& e) {
        Logger::error("JSON parse error in " + locale_file_path.string() + ": " + e.what());
        return false;
    }

    messages.clear();
    for (auto it = j.begin(); it != j.end(); ++it) {
        messages[it.key()] = it.value();
    }
    return true;
}

std::string Message::get(const std::string& id, const std::map<std::string, std::string>& params) {
    auto it = messages.find(id);
    if (it == messages.end()) return id;
    std::string msg = it->second;
    for (const auto& [key, value] : params) {
        std::string placeholder = "{" + key + "}";
        size_t pos = 0;
        while ((pos = msg.find(placeholder, pos)) != std::string::npos) {
            msg.replace(pos, placeholder.length(), value);
            pos += value.length();
        }
    }
    return msg;
}