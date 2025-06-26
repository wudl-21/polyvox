#pragma once
#include <string>
#include <map>
#include <vector>

// 通用XML节点结构
struct XmlNode {
    std::string tag;
    std::map<std::string, std::string> attributes;
    std::vector<XmlNode> children;
};

// 通用XML生成函数
bool generate_xml_from_tree(const std::string& filename, const XmlNode& root);