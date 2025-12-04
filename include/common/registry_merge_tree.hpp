#include <iostream>
#include <map>
#include <vector>
#include <string>
#include <set>
#include <algorithm>
#include <fstream>
#include <cstdint>
#include <memory>
#include <numeric>
#include <registry/utils.hpp>
#include <common/fs_info.hpp>

struct TreeNode;
using TreeNodePtr = std::shared_ptr<TreeNode>;

struct TreeNode {
    std::string workflowname;
    uint32_t lines;
    std::string hostfilepath;
    std::vector<TreeNodePtr> childTreeNodePtr;
    std::vector<std::string> basehostfiles;
};

class TreeManager {
private:
    std::map<std::string, TreeNodePtr> nodeMap;

    std::map<std::string, std::vector<uint32_t>> getFileLineMap(
        const std::vector<std::string>& baseFiles) {
        
        std::map<std::string, std::vector<uint32_t>> fileLineMap;
        uint32_t currentLine = 0;

        for (const auto& file : baseFiles) {
            std::vector<uint32_t> lines;
            uint32_t lineCount = count_file_lines(file);
            
            for (uint32_t i = 0; i < lineCount; ++i) {
                lines.push_back(currentLine++);
            }
            
            fileLineMap[file] = lines;
        }

        return fileLineMap;
    }

public:
    bool AddTree(const std::string& workflowname, const std::string& hostfilepath) {
        std::string key = workflowname;
        
        if (nodeMap.find(key) != nodeMap.end()) {
            std::cerr << "节点已存在: " << workflowname << " - " << hostfilepath << std::endl;
            return false;
        }

        auto newNode = std::make_shared<TreeNode>();
        newNode->workflowname = workflowname;
        newNode->hostfilepath = hostfilepath;
        newNode->lines = count_file_lines(hostfilepath);
        newNode->basehostfiles = {hostfilepath};
        newNode->childTreeNodePtr.clear();

        nodeMap[key] = newNode;
        return true;
    }

    bool MergeTree(
        const std::string& workflowname, 
        const std::string& hostfilepath,
        const std::string& configfilepath,
        const std::vector<std::string>& childFlownames) {
        
        std::string currentKey = workflowname;
        if (nodeMap.find(currentKey) != nodeMap.end()) {
            std::cerr << "节点已存在: " << workflowname << " - " << hostfilepath << std::endl;
            return false;
        }

        // 查找子节点
        std::vector<TreeNodePtr> children;
        for (const auto& childFlow : childFlownames) {
            bool found = false;
            for (const auto& entry : nodeMap) {
                if (entry.second->workflowname == childFlow) {
                    children.push_back(entry.second);
                    found = true;
                    break;
                }
            }
            if (!found) {
                std::cerr << "找不到子节点: " << childFlow << std::endl;
                return false;
            }
        }

        // 收集所有子节点的basehostfiles（去重+排序）
        std::set<std::string> baseFilesSet;
        for (const auto& child : children) {
            for (const auto& file : child->basehostfiles) {
                baseFilesSet.insert(file);
            }
        }
        std::vector<std::string> baseFiles(baseFilesSet.begin(), baseFilesSet.end());
        std::sort(baseFiles.begin(), baseFiles.end());

        // 合并子节点文件内容到当前节点的hostfilepath
        std::ofstream outFile(hostfilepath, std::ios::trunc);
        if (!outFile.is_open()) {
            std::cerr << "无法打开输出文件: " << hostfilepath << std::endl;
            return false;
        }
        for (const auto& file : baseFiles) {
            std::ifstream inFile(file);
            if (inFile.is_open()) {
                outFile << inFile.rdbuf();
                inFile.close();
            } else {
                std::cerr << "警告：无法读取文件 " << file << "，将跳过" << std::endl;
            }
        }
        outFile.close();
        uint32_t currentLines = count_file_lines(hostfilepath);
        auto fileLineMap = getFileLineMap(baseFiles);

        std::vector<fs_info> fs_infos;
        std::vector<uint32_t> parent_lines(currentLines);
        std::iota(parent_lines.begin(), parent_lines.end(), 0);
        fs_info parent_info = {workflowname, parent_lines, 0};
        fs_infos.push_back(parent_info);
        std::cout << parent_info.serialize() << std::endl;
        uint32_t childPriority = 1;
        for (const auto& child : children) {
            fs_info info;
            info.flowname = child->workflowname;
            info.priority = childPriority++;  // 每个子节点优先级递增
            
            for (const auto& file : child->basehostfiles) {
                auto it = fileLineMap.find(file);
                if (it != fileLineMap.end()) {
                    info.fs_size_seq.insert(info.fs_size_seq.end(), it->second.begin(), it->second.end());
                }
            }

            std::cout << info.serialize() << std::endl;
            fs_infos.push_back(info);
        }

        // 写入配置文件
        std::ofstream hcfile(configfilepath, std::ios::trunc);
        if (!hcfile.is_open()) {
            std::cerr << "无法打开配置文件: " << configfilepath << std::endl;
            return false;
        }
        for (const auto& info : fs_infos) {
            hcfile << info.serialize() << std::endl;
        }
        hcfile.close();

        
        auto newNode = std::make_shared<TreeNode>();
        newNode->workflowname = workflowname;
        newNode->hostfilepath = hostfilepath;
        newNode->lines = currentLines;
        newNode->childTreeNodePtr = children;
        newNode->basehostfiles = baseFiles;

        nodeMap[currentKey] = newNode;
        return true;
    }
};
