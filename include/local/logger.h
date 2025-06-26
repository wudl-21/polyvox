#pragma once
#include <iostream>
#include <string>
#include <map>
#include "message.h"

class Logger {
public:
    // 支持直接输出字符串
    static void info(const std::string& msg) {
        std::cout << "\033[1;32m[INFO] \033[0m" << msg << std::endl; // 绿色
    }
    static void warn(const std::string& msg) {
        std::cerr << "\033[1;33m[WARN] \033[0m" << msg << std::endl; // 黄色
    }
    static void error(const std::string& msg) {
        std::cerr << "\033[1;31m[ERROR] \033[0m" << msg << std::endl; // 红色
    }

    // 新增：支持消息ID和参数
    static void info_id(const std::string& id, const std::map<std::string, std::string>& params = {}) {
        info(Message::get(id, params));
    }
    static void warn_id(const std::string& id, const std::map<std::string, std::string>& params = {}) {
        warn(Message::get(id, params));
    }
    static void error_id(const std::string& id, const std::map<std::string, std::string>& params = {}) {
        error(Message::get(id, params));
    }
};