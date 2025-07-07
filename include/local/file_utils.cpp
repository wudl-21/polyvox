#include "file_utils.h"

namespace FileUtils {

std::string getDirectory(const std::string& filepath) {
    return std::filesystem::path(filepath).parent_path().string();
}

std::string getStem(const std::string& filepath) {
    return std::filesystem::path(filepath).stem().string();
}

std::string getExtension(const std::string& filepath) {
    return std::filesystem::path(filepath).extension().string().substr(1); // 去掉点
}

std::string join(const std::string& dir, const std::string& filename) {
    return (std::filesystem::path(dir) / filename).string();
}

bool exists(const std::string& path) {
    return std::filesystem::exists(path);
}

bool isDirectory(const std::string& path) {
    return std::filesystem::is_directory(path);
}

}