/*
  Copyright 2018-2024, Barcelona Supercomputing Center (BSC), Spain
  Copyright 2015-2024, Johannes Gutenberg Universitaet Mainz, Germany
*/

#include <stage/stage_util.hpp>

using namespace std;

/*
* Representing bytes as "number + B,KB,MB,GB,TB"
*/
std::string convertBytes(size_t bytes) {
    const std::vector<std::pair<std::string, size_t>> units = {
        {"TB", 1024ULL * 1024 * 1024 * 1024},
        {"GB", 1024ULL * 1024 * 1024},
        {"MB", 1024ULL * 1024},
        {"KB", 1024ULL},
        {"B", 1}
    };

    for (const auto& unit : units) {
        if (bytes >= unit.second) {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(2) 
            << static_cast<double>(bytes) / unit.second << " " << unit.first;
            return oss.str();
        }
    }
    return std::to_string(bytes);
}

/*
* Representing "int number + B,KB,MB,GB,TB" as bytes
*/
size_t parseStorageSize(const std::string& str) {

    static const std::unordered_map<std::string, size_t> unitMultipliers = {
        {"", 1ULL},
        {"b", 1ULL},
        {"k", 1024ULL},
        {"kb", 1024ULL},
        {"m", 1024ULL * 1024},
        {"mb", 1024ULL * 1024},
        {"g", 1024ULL * 1024 * 1024},
        {"gb", 1024ULL * 1024 * 1024},
        {"t", 1024ULL * 1024 * 1024 * 1024},
        {"tb", 1024ULL * 1024 * 1024 * 1024}
    };
    std::string numberPart;
    std::string unitPart;
    for (char c : str) {
        if (std::isdigit(c)) {
            numberPart += c;
        } else if (std::isalpha(c)) {
            unitPart += std::tolower(c);
        }
    }
    size_t value;
    try {
        value = std::stoul(numberPart);
    } catch (const std::invalid_argument&) {
        throw std::invalid_argument(str + ": has no invalid number.");
    } catch (const std::out_of_range&) {
        throw std::out_of_range(str + ": the number part is too large.");
    }
    auto it = unitMultipliers.find(unitPart);
    if (it != unitMultipliers.end()) {
        return value * it->second;
    } else {
        throw std::invalid_argument(str + ": not support " + unitPart);
    }
}

size_t make_align(size_t size, size_t blk_size) {
    return ((size + blk_size - 1) / blk_size) * blk_size;
}

