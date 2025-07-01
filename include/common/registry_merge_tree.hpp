#ifndef MERGE_TREE_H
#define MERGE_TREE_H

#include <string>
#include <vector>
#include <memory>
#include <map>
#include <fstream>
#include <stdexcept>
#include <sstream>
#include <set>
#include <ctime>
#include "fs_info.hpp"

class TreeNode;
using TreeNodePtr = std::shared_ptr<TreeNode>;

class TreeNode {
public:
    std::string flowname;
    unsigned int priority;
    unsigned int lines;
    time_t life_start;
    time_t life_end;
    std::vector<TreeNodePtr> children;

    TreeNode(const std::string& flowname, unsigned int priority, unsigned int lines)
        : flowname(flowname), priority(priority), lines(lines),
          life_start(TimeMin), 
          life_end(TimeMax) {}

    int countNodes() const {
        int count = 1;
        for (const auto& child : children) {
            count += child->countNodes();
        }
        return count;
    }

    void generatePositionVectors(std::vector<fs_info>& fs_vector, unsigned int prefix = 0) const {
        if(lines == 0) return;
        fs_info fs_conf = {prefix, lines, priority, life_start, life_end};
        fs_vector.push_back(fs_conf);
        
        unsigned int currentPrefix = prefix;
        for (const auto& child : children) {
            child->generatePositionVectors(fs_vector, currentPrefix);
            currentPrefix += child->lines;
        }
    }
};

class TreeSerializer {
public:
    static std::string serialize(const TreeNodePtr& root) {
        if (!root) return "null";
        std::stringstream ss;
        ss << "{";
        ss << "\"flowname\":\"" << root->flowname << "\",";
        ss << "\"priority\":" << root->priority << ",";
        ss << "\"lines\":" << root->lines << ",";
        ss << "\"life_start\":" << root->life_start << ",";
        ss << "\"life_end\":" << root->life_end << ",";
        ss << "\"children\":[";
        for (size_t i = 0; i < root->children.size(); ++i) {
            ss << serialize(root->children[i]);
            if (i != root->children.size() - 1) {
                ss << ",";
            }
        }
        ss << "]}";
        return ss.str();
    }

    static TreeNodePtr deserialize(const std::string& json) {
        size_t pos = 0;
        return parseNode(json, pos);
    }

    std::string serializeStructure(const TreeNodePtr& root) {
        if (!root) return "[]";
        
        std::stringstream ss;
        ss << "[";
        serializeStructureRecursive(root, ss, 0);
        ss << "]";
        return ss.str();
    }

    void serializeTreeToFile(const TreeNodePtr& root, const std::string& filename) {
        if (!root) throw std::runtime_error("Root node cannot be null");
        
        std::ofstream file(filename);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open file: " + filename);
        }

        try {
            std::string standardJson = serialize(root);
            file << standardJson << std::endl;
            file << "=============flow structure=============" << std::endl;
            file << serializeStructure(root) << std::endl;
            file.close();
        } catch (const std::exception& e) {
            file.close();
            throw std::runtime_error("Serialization error: " + std::string(e.what()));
        }
    }

    TreeNodePtr deserializeTreeFromFile(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            return std::make_shared<TreeNode>("", 0, 0);
        }

        try {
            std::stringstream buffer;
            buffer << file.rdbuf();
            file.close();

            std::string content = buffer.str();
            size_t delimiterPos = content.find("=============flow structure=============");

            if (delimiterPos == std::string::npos) {
                return std::make_shared<TreeNode>("", 0, 0);
            }

            std::string standardJson = content.substr(0, delimiterPos);
            if (!standardJson.empty() && standardJson.back() == '\n') {
                standardJson.pop_back();
            }

            return deserialize(standardJson);
        } catch (const std::exception&) {
            return std::make_shared<TreeNode>("", 0, 0);
        }
    }

private:
    void serializeStructureRecursive(const TreeNodePtr& node, std::stringstream& ss, int depth) {
        for (int i = 0; i < depth; ++i) ss << "  ";
        ss << "\"" << node->flowname << "\"";
        if (!node->children.empty()) {
            ss << " {\n";
            for (size_t i = 0; i < node->children.size(); ++i) {
                serializeStructureRecursive(node->children[i], ss, depth + 1);
                if (i != node->children.size() - 1) ss << ",\n";
            }
            for (int i = 0; i < depth; ++i) ss << "  ";
            ss << "}";
        }
        if (depth == 0 && node->children.size() > 1) {
            ss << ", ";
        }
    }

    static TreeNodePtr parseNode(const std::string& json, size_t& pos) {
        skipWhitespace(json, pos);
        if (pos >= json.size() || json[pos] == 'n') {
            pos += 4; // "null"
            return nullptr;
        }

        if (json[pos] != '{') {
            throw std::runtime_error("Invalid JSON format");
        }
        ++pos;

        std::string flowname;
        unsigned int priority = 0;
        unsigned int lines = 0;
        time_t life_start = TimeMin;
        time_t life_end = TimeMax;
        std::vector<TreeNodePtr> children;

        while (pos < json.size() && json[pos] != '}') {
            skipWhitespace(json, pos);
            std::string key = parseString(json, pos);
            skipWhitespace(json, pos);
            if (json[pos] != ':') throw std::runtime_error("Invalid JSON format");
            ++pos;
            skipWhitespace(json, pos);

            if (key == "flowname") {
                flowname = parseString(json, pos);
            } else if (key == "priority") {
                priority = parseUnsignedInt(json, pos);  // 使用无符号解析
            } else if (key == "lines") {
                lines = parseUnsignedInt(json, pos);     // 使用无符号解析
            } else if (key == "life_start") {
                life_start = (time_t)parseInt(json, pos);
            } else if (key == "life_end") {
                life_end = (time_t)parseInt(json, pos);
            } else if (key == "children") {
                if (json[pos] != '[') throw std::runtime_error("Invalid JSON format");
                ++pos;
                while (pos < json.size() && json[pos] != ']') {
                    children.push_back(parseNode(json, pos));
                    skipWhitespace(json, pos);
                    if (json[pos] == ',') ++pos;
                }
                ++pos;
            }

            skipWhitespace(json, pos);
            if (json[pos] == ',') ++pos;
        }
        ++pos;

        auto node = std::make_shared<TreeNode>(flowname, priority, lines);
        node->life_start = life_start;
        node->life_end = life_end;
        node->children = std::move(children);
        return node;
    }

    static void skipWhitespace(const std::string& json, size_t& pos) {
        while (pos < json.size() && 
               (json[pos] == ' ' || json[pos] == '\t' || 
                json[pos] == '\n' || json[pos] == '\r')) {
            ++pos;
        }
    }

    static std::string parseString(const std::string& json, size_t& pos) {
        if (pos >= json.size() || json[pos] != '"') {
            throw std::runtime_error("Invalid string format");
        }
        ++pos;
        std::string result;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == '\\') {
                ++pos;
                if (pos < json.size()) {
                    result += json[pos];
                }
            } else {
                result += json[pos];
            }
            ++pos;
        }
        ++pos;
        return result;
    }

    // 解析有符号整数（用于time_t）
    static long long parseInt(const std::string& json, size_t& pos) {
        long long result = 0;
        bool isNegative = false;
        
        if (pos < json.size() && json[pos] == '-') {
            isNegative = true;
            ++pos;
        }
        
        if (pos >= json.size() || !isdigit(json[pos])) {
            throw std::runtime_error("Invalid number format");
        }
        
        while (pos < json.size() && isdigit(json[pos])) {
            result = result * 10 + (json[pos] - '0');
            ++pos;
        }
        
        return isNegative ? -result : result;
    }

    // 解析无符号整数（用于unsigned int）
    static unsigned int parseUnsignedInt(const std::string& json, size_t& pos) {
        unsigned long long result = 0;
        
        if (pos >= json.size() || !isdigit(json[pos])) {
            throw std::runtime_error("Invalid unsigned integer format");
        }
        
        while (pos < json.size() && isdigit(json[pos])) {
            result = result * 10 + (json[pos] - '0');
            ++pos;
            
            // 检查溢出
            if (result > UINT_MAX) {
                throw std::runtime_error("Integer overflow while parsing unsigned int");
            }
        }
        
        return static_cast<unsigned int>(result);
    }
};

class TreeManager {
public:
    std::map<std::string, TreeNodePtr> flownameToRoot;  

    void addTree(const TreeNodePtr& root) {
        if (!root) return;
        buildFlownameMap(root, root);
    }

    TreeNodePtr mergeTrees(const std::string& parentFlowname, unsigned int parentPriority,
                           time_t parent_life_start,
                           const std::vector<std::string>& childFlownames) {
        std::vector<TreeNodePtr> children;
        std::set<TreeNodePtr> seenRoots;
        unsigned int totalLines = 0;

        for (const auto& flowname : childFlownames) {
            auto it = flownameToRoot.find(flowname);
            if (it == flownameToRoot.end()) {
                std::cerr << "Error ignored: Flowname not found: " + flowname << std::endl;
                continue;
            }

            TreeNodePtr root = it->second;
            if (seenRoots.insert(root).second) {
                children.push_back(root);
                totalLines += root->lines;
            }
        }

        auto parent = std::make_shared<TreeNode>(parentFlowname, parentPriority, totalLines);
        parent->life_start = parent_life_start;
        parent->life_end = TimeMax;
        parent->children = std::move(children);

        buildFlownameMap(parent, parent);

        unsigned int currentPriority = parentPriority + 1;
        for (const auto& childRoot : parent->children) {
            childRoot->life_end = parent_life_start;
            updateSubtreePriority(childRoot, currentPriority);
            currentPriority += childRoot->countNodes();
        }

        return parent;
    }

private:
    void buildFlownameMap(const TreeNodePtr& node, const TreeNodePtr& root) {
        if (!node) return;
        flownameToRoot[node->flowname] = root;
        for (const auto& child : node->children) {
            buildFlownameMap(child, root);
        }
    }

    void updateSubtreePriority(const TreeNodePtr& node, unsigned int& currentPriority) {
        if (!node) return;
        node->priority = currentPriority++;
        for (const auto& child : node->children) {
            updateSubtreePriority(child, currentPriority);
        }
    }
};

#endif // MERGE_TREE_H