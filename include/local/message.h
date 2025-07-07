#pragma once
#include <string>
#include <unordered_map>
#include <map>

class Message {
public:
    // 加载指定语言的消息资源（如"zh-cn"、"en"）
    static bool load(const std::string& lang);

    // 获取消息文本，支持简单占位符替换
    static std::string get(const std::string& id, const std::map<std::string, std::string>& params = {});

private:
    static std::unordered_map<std::string, std::string> messages;
    static std::string current_lang;
};