#ifndef GEKKOFS_REGISTRY_UTIL_HPP
#define GEKKOFS_REGISTRY_UTIL_HPP

#include <string>
#include <filesystem>
namespace fs = std::filesystem;

bool
check_file_modified(const std::string& path, 
    fs::file_time_type last_modified_time);

unsigned int
count_file_lines(const std::string& path);

#endif // GEKKOFS_REGISTRY_UTIL_HPP
