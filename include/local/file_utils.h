#pragma once
#include <string>
#include <filesystem>

namespace FileUtils {

    // 获取文件所在目录
    std::string getDirectory(const std::string& filepath);

    // 获取文件名（不含扩展名）
    std::string getStem(const std::string& filepath);

    // 获取文件扩展名（不含点）
    std::string getExtension(const std::string& filepath);

    // 拼接路径
    std::string join(const std::string& dir, const std::string& filename);

    // 判断文件/目录是否存在
    bool exists(const std::string& path);

    // 判断是否为目录
    bool isDirectory(const std::string& path);

}