/*
  Copyright 2018-2024, Barcelona Supercomputing Center (BSC), Spain
  Copyright 2015-2024, Johannes Gutenberg Universitaet Mainz, Germany

  This software was partially supported by the
  EC H2020 funded project NEXTGenIO (Project ID: 671951, www.nextgenio.eu).

  This software was partially supported by the
  ADA-FS project under the SPPEXA project funded by the DFG.

  This file is part of GekkoFS.

  GekkoFS is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  GekkoFS is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with GekkoFS.  If not, see <https://www.gnu.org/licenses/>.

  SPDX-License-Identifier: GPL-3.0-or-later
*/

#ifndef GEKKOFS_STAGE_HPP
#define GEKKOFS_STAGE_HPP

#include <unordered_set>
#include <regex>
#include <algorithm>
#include <cctype>

#define STAGE_IN (1<<0)
#define STAGE_RECURSIVE (1<<1)
#define STAGE_O_DIRECT (1<<2)
#define STAGE_FORCE (1<<3)
#define STAGE_MMAP (1<<4)

struct Stage_options {
    std::string src;
    std::string dest;
    std::string buffer_size;
    std::string block_size;
    bool stage_in;
    unsigned long threads;
    unsigned long n_buffers;
    size_t o_direct_size;
    unsigned long nodes;
    std::string nodelists;
};

class FastNodeChecker {
private:
    std::unordered_set<std::string> node_set;
    std::unordered_map<std::string, std::vector<std::pair<int, int>>> prefix_ranges;

    // 快速解析节点前缀和编号
    static bool parse_node(const std::string& s, std::string& prefix, int& num) {
        prefix.clear();
        num = 0;
        
        size_t i = 0;
        // 提取前缀字母部分
        while (i < s.size() && std::isalpha(s[i])) {
            prefix += s[i++];
        }
        if (prefix.empty() || i == s.size()) return false;

        // 转换数字部分
        try {
            num = std::stoi(s.substr(i));
        } catch (...) {
            return false;
        }
        return true;
    }

public:
    // 在 parse 函数中修改为指针操作风格：
    void parse(const std::string& list) {
        node_set.clear();
        prefix_ranges.clear();

        const char* start = list.data() + 1; // 跳过[
        const char* end = list.data() + list.size() - 1; // 跳过]
        
        while (start < end) {
            const char* comma = std::find(start, end, ',');
            std::string entry(start, comma);
            
            // 使用C字符串函数查找'-'
            if (const char* dash = std::strchr(entry.c_str(), '-')) {
                std::string start_str(entry.c_str(), dash);
                std::string end_str(dash + 1);
                
                std::string sprefix, eprefix;
                int snum, enum_;
                if (parse_node(start_str, sprefix, snum) && 
                    parse_node(end_str, eprefix, enum_)) 
                {
                    if (eprefix.empty()) eprefix = sprefix;
                    if (sprefix == eprefix) {
                        prefix_ranges[sprefix].emplace_back(snum, enum_);
                    }
                }
            } else {
                node_set.insert(entry);
            }
            
            start = comma + (comma < end ? 1 : 0);
        }

        // 排序每个前缀的范围
        for (auto& [p, ranges] : prefix_ranges) {
            std::sort(ranges.begin(), ranges.end());
        }
    }

    bool contains(const std::string& target) const {
        // 快速检查哈希表
        if (node_set.count(target)) return true;
        
        // 解析目标节点
        std::string prefix;
        int num;
        if (!parse_node(target, prefix, num)) return false;
        
        auto it = prefix_ranges.find(prefix);
        if (it == prefix_ranges.end()) return false;
        
        // 二分查找范围
        const auto& ranges = it->second;
        auto pos = std::upper_bound(ranges.begin(), ranges.end(), num,
            [](int value, const auto& range) {
                return value < range.first;
            });
        
        if (pos != ranges.begin()) {
            --pos;
            if (num >= pos->first && num <= pos->second) {
                return true;
            }
        }
        return false;
    }
};


#endif // GEKKOFS_STAGE_HPP
