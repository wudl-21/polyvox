#include "xml_parser.h"
#include "third_party/tinyxml2.h"
#include <filesystem> // 新增
#include <cstdio>     // 新增

static void append_xml_node(tinyxml2::XMLDocument& doc, tinyxml2::XMLElement* parent, const XmlNode& node) {
    using namespace tinyxml2;
    XMLElement* elem = doc.NewElement(node.tag.c_str());
    for (const auto& [k, v] : node.attributes) {
        elem->SetAttribute(k.c_str(), v.c_str());
    }
    for (const auto& child : node.children) {
        append_xml_node(doc, elem, child);
    }
    parent->InsertEndChild(elem);
}

bool generate_xml_from_tree(const std::string& filename, const XmlNode& root) {
    using namespace tinyxml2;
    XMLDocument doc;
    auto* decl = doc.NewDeclaration();
    doc.InsertFirstChild(decl);

    // 用一个临时根节点包裹，便于插入真实根节点
    XMLElement* tempRoot = doc.NewElement("root_placeholder");
    doc.InsertEndChild(tempRoot);
    append_xml_node(doc, tempRoot, root);

    // 移除临时根节点，只保留真实根节点
    XMLElement* realRoot = tempRoot->FirstChildElement();
    if (realRoot) {
        doc.InsertEndChild(realRoot->DeepClone(&doc));
        doc.DeleteChild(tempRoot);
    }

    // --- 修改：使用Unicode安全的方式保存文件 ---
    std::filesystem::path path(filename);
    FILE* f = nullptr;
#ifdef _WIN32
    // 在Windows上，使用_wfopen来正确处理非ASCII路径
    f = _wfopen(path.wstring().c_str(), L"w");
#else
    // 在其他系统上，标准fopen应该能处理UTF-8
    f = fopen(path.string().c_str(), "w");
#endif

    if (!f) {
        return false; // 文件打开失败
    }

    XMLError err = doc.SaveFile(f);
    fclose(f);

    return err == XML_SUCCESS;
}