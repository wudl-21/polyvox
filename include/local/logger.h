#pragma once
#include <iostream>
#include <string>
#include <map>
#include "message.h"

class Logger {
public:
    static bool verbose;

    static void info(const std::string& msg) {
        if (verbose)
            std::cout << "\033[1;32m[INFO] \033[0m" << msg << std::endl;
    }
    static void warn(const std::string& msg) {
        if (verbose)
            std::cerr << "\033[1;33m[WARN] \033[0m" << msg << std::endl;
    }
    static void error(const std::string& msg) {
        std::cerr << "\033[1;31m[ERROR] \033[0m" << msg << std::endl;
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

inline bool Logger::verbose = false;