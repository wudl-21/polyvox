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
            std::cout << msg << std::endl;
    }
    static void warn(const std::string& msg) {
        if (verbose)
            std::cerr << msg << std::endl;
    }
    static void error(const std::string& msg) {
        std::cerr << msg << std::endl;
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