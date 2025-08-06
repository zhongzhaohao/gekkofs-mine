#ifndef FSINFO_H
#define FSINFO_H
#include <string>
#include <vector>
#include <sstream>
#include <cstdint>
#include <limits>

struct fs_info {
    std::string flowname;
    std::vector<uint32_t> fs_size_seq;
    uint32_t priority;

    // 序列化方法，包含priority
    std::string serialize() const {
        std::stringstream ss;
        ss << flowname << ":" << priority << ":";
        for (size_t i = 0; i < fs_size_seq.size(); ++i) {
            if (i > 0) ss << ",";
            ss << fs_size_seq[i];
        }
        return ss.str();
    }

    // 反序列化方法，格式正确返回true，否则返回false
    // 结果通过引用参数info传出
    static bool deserialize(const std::string& str, fs_info& info) {
        // 先清空输出对象，确保状态干净
        info.flowname.clear();
        info.fs_size_seq.clear();
        info.priority = 0;

        std::stringstream ss(str);
        std::vector<std::string> parts;
        std::string part;

        // 按":"分割字符串，必须正好得到3个部分
        while (std::getline(ss, part, ':')) {
            parts.push_back(part);
        }

        // 校验1：必须包含3个部分（flowname:priority:fs_size_seq）
        if (parts.size() != 3) {
            return false;
        }

        // 解析flowname（不能为空）
        if (parts[0].empty()) {
            return false;
        }
        info.flowname = parts[0];

        // 解析priority（必须是有效的无符号整数）
        try {
            size_t pos;
            unsigned long val = std::stoul(parts[1], &pos);
            // 校验：必须完全解析（不能有多余字符）且值在uint32_t范围内
            if (pos != parts[1].size() || val > std::numeric_limits<uint32_t>::max()) {
                return false;
            }
            info.priority = static_cast<uint32_t>(val);
        } catch (...) {
            // 转换失败（非数字、溢出等）
            return false;
        }

        // 解析fs_size_seq（每个元素必须是有效的无符号整数）
        std::stringstream fs_size_seqtream(parts[2]);
        std::string fs_size_str;
        while (std::getline(fs_size_seqtream, fs_size_str, ',')) {
            // 空字符串（如连续逗号造成的）视为无效
            if (fs_size_str.empty()) {
                return false;
            }
            try {
                size_t pos;
                unsigned long val = std::stoul(fs_size_str, &pos);
                // 校验：必须完全解析且值在uint32_t范围内
                if (pos != fs_size_str.size() || val > std::numeric_limits<uint32_t>::max()) {
                    return false;
                }
                info.fs_size_seq.push_back(static_cast<uint32_t>(val));
            } catch (...) {
                // 单个元素解析失败，整体视为格式错误
                return false;
            }
        }

        // 所有校验通过
        return true;
    }
};


#endif // FSINFO_H