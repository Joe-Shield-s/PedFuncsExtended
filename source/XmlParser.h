#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <sstream>
#include <fstream>
#include <algorithm>

namespace XmlLite {

struct XmlNode {
    std::string tag;
    std::string text;
    std::unordered_map<std::string, std::string> attributes;
    std::vector<XmlNode> children;

    const std::string& GetAttr(const std::string& name, const std::string& defVal = "") const {
        auto it = attributes.find(name);
        return it != attributes.end() ? it->second : defVal;
    }

    int GetAttrInt(const std::string& name, int defVal = 0) const {
        auto it = attributes.find(name);
        if (it != attributes.end()) {
            try { return std::stoi(it->second, nullptr, 0); } catch (...) {}
        }
        return defVal;
    }

    const XmlNode* FindChild(const std::string& childTag) const {
        for (const auto& c : children) {
            if (c.tag == childTag) return &c;
        }
        return nullptr;
    }

    std::vector<const XmlNode*> FindChildren(const std::string& childTag) const {
        std::vector<const XmlNode*> result;
        for (const auto& c : children) {
            if (c.tag == childTag) result.push_back(&c);
        }
        return result;
    }
};

inline void SkipWhitespace(const std::string& str, size_t& pos) {
    while (pos < str.size() && (str[pos] == ' ' || str[pos] == '\t' || str[pos] == '\r' || str[pos] == '\n')) {
        pos++;
    }
}

inline bool ParseXml(const std::string& xml, XmlNode& root) {
    size_t pos = 0;
    std::vector<XmlNode*> stack;

    while (pos < xml.size()) {
        SkipWhitespace(xml, pos);
        if (pos >= xml.size()) break;

        if (xml[pos] == '<') {
            pos++;
            if (pos >= xml.size()) break;

            // Comment <!-- ... -->
            if (xml.compare(pos, 3, "!--") == 0) {
                pos += 3;
                size_t endComment = xml.find("-->", pos);
                if (endComment == std::string::npos) break;
                pos = endComment + 3;
                continue;
            }

            // XML Declaration <?xml ... ?>
            if (xml[pos] == '?') {
                size_t endDecl = xml.find("?>", pos);
                if (endDecl == std::string::npos) break;
                pos = endDecl + 2;
                continue;
            }

            // Closing tag </tag>
            if (xml[pos] == '/') {
                pos++;
                size_t endTag = xml.find('>', pos);
                if (endTag == std::string::npos) break;
                if (!stack.empty()) stack.pop_back();
                pos = endTag + 1;
                continue;
            }

            // Opening or Self-closing tag <tag ...> or <tag ... />
            size_t tagEnd = pos;
            while (tagEnd < xml.size() && xml[tagEnd] != ' ' && xml[tagEnd] != '>' && xml[tagEnd] != '/' && xml[tagEnd] != '\t' && xml[tagEnd] != '\r' && xml[tagEnd] != '\n') {
                tagEnd++;
            }
            std::string tagName = xml.substr(pos, tagEnd - pos);
            pos = tagEnd;

            XmlNode newNode;
            newNode.tag = tagName;

            // Parse attributes
            while (pos < xml.size() && xml[pos] != '>' && xml[pos] != '/') {
                SkipWhitespace(xml, pos);
                if (pos >= xml.size() || xml[pos] == '>' || xml[pos] == '/') break;

                size_t attrNameEnd = pos;
                while (attrNameEnd < xml.size() && xml[attrNameEnd] != '=' && xml[attrNameEnd] != ' ' && xml[attrNameEnd] != '>' && xml[attrNameEnd] != '/') {
                    attrNameEnd++;
                }
                std::string attrName = xml.substr(pos, attrNameEnd - pos);
                pos = attrNameEnd;
                SkipWhitespace(xml, pos);

                std::string attrVal = "";
                if (pos < xml.size() && xml[pos] == '=') {
                    pos++;
                    SkipWhitespace(xml, pos);
                    if (pos < xml.size() && (xml[pos] == '"' || xml[pos] == '\'')) {
                        char quote = xml[pos++];
                        size_t valEnd = xml.find(quote, pos);
                        if (valEnd != std::string::npos) {
                            attrVal = xml.substr(pos, valEnd - pos);
                            pos = valEnd + 1;
                        }
                    }
                }
                newNode.attributes[attrName] = attrVal;
            }

            SkipWhitespace(xml, pos);
            bool selfClosing = false;
            if (pos < xml.size() && xml[pos] == '/') {
                selfClosing = true;
                pos++;
            }
            if (pos < xml.size() && xml[pos] == '>') {
                pos++;
            }

            if (stack.empty()) {
                root = std::move(newNode);
                if (!selfClosing) stack.push_back(&root);
            } else {
                XmlNode* parent = stack.back();
                parent->children.push_back(std::move(newNode));
                if (!selfClosing) {
                    stack.push_back(&parent->children.back());
                }
            }
        } else {
            // Text content
            size_t nextTag = xml.find('<', pos);
            std::string textContent;
            if (nextTag != std::string::npos) {
                textContent = xml.substr(pos, nextTag - pos);
                pos = nextTag;
            } else {
                textContent = xml.substr(pos);
                pos = xml.size();
            }
            // Trim
            size_t first = textContent.find_first_not_of(" \t\r\n");
            if (first != std::string::npos) {
                size_t last = textContent.find_last_not_of(" \t\r\n");
                if (!stack.empty()) {
                    stack.back()->text = textContent.substr(first, (last - first + 1));
                }
            }
        }
    }
    return !root.tag.empty();
}

inline bool LoadXmlFromFile(const std::string& filePath, XmlNode& root) {
    std::ifstream f(filePath);
    if (!f.is_open()) return false;
    std::stringstream buffer;
    buffer << f.rdbuf();
    return ParseXml(buffer.str(), root);
}

} // namespace XmlLite
