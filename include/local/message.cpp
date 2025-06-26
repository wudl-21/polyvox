#include <fstream>
#include <sstream>
#include <filesystem>
#include "local/message.h"
#include "third_party/json.hpp"

std::unordered_map<std::string, std::string> Message::messages;
std::string Message::current_lang = "zh";

bool Message::load(const std::string& lang) {
    current_lang = lang;
    // 获取可执行文件所在目录
    std::filesystem::path exePath = std::filesystem::current_path();
    std::filesystem::path localePath = exePath / "../locale/" / (lang + ".json");
    std::ifstream file(localePath);
    if (!file.is_open()) return false;
    nlohmann::json j;
    file >> j;
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