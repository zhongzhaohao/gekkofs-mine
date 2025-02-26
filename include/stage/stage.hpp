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

    static bool parse_node(const std::string& s, std::string& prefix, int& num) {
        prefix.clear();
        num = 0;

        size_t i = 0;
        while (i < s.size() && std::isalpha(s[i])) {
            prefix += s[i++];
        }
        if (prefix.empty() || i == s.size()) return false;

        try {
            num = std::stoi(s.substr(i));
        } catch (...) {
            return false;
        }
        return true;
    }

public:
    void parse(const std::string& list) {
        node_set.clear();
        prefix_ranges.clear();

        size_t bracket_pos = list.find('[');
        if (bracket_pos == std::string::npos || list.back() != ']')
            return;

        std::string prefix = list.substr(0, bracket_pos);
        std::string content = list.substr(bracket_pos + 1, list.size() - bracket_pos - 2);

        const char* start = content.data();
        const char* end = content.data() + content.size();

        while (start < end) {
            const char* comma = std::find(start, end, ',');
            std::string entry(start, comma);

            if (const char* dash = std::strchr(entry.c_str(), '-')) {
                std::string start_str(entry.c_str(), dash);
                std::string end_str(dash + 1);
                try {
                    int snum = std::stoi(start_str);
                    int enum_ = std::stoi(end_str);
                    prefix_ranges[prefix].emplace_back(snum, enum_);
                } catch (...) {  }
            }
            else {
                try {
                    node_set.insert(prefix + entry); 
                } catch (...) {  }
            }

            start = comma + (comma < end ? 1 : 0);
        }

        for (auto& [p, ranges] : prefix_ranges) {
            std::sort(ranges.begin(), ranges.end());
        }
    }

    bool contains(const std::string& target) const {
        if (node_set.count(target)) return true;

        std::string prefix;
        int num;
        if (!parse_node(target, prefix, num)) return false;

        auto it = prefix_ranges.find(prefix);
        if (it == prefix_ranges.end()) return false;

        const auto& ranges = it->second;
        auto pos = std::upper_bound(ranges.begin(), ranges.end(), num,
            [](int value, const auto& range) { return value < range.first; });

        return (pos != ranges.begin() && num <= (--pos)->second);
    }
};


#endif // GEKKOFS_STAGE_HPP
