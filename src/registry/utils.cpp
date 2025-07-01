#include <registry/utils.hpp>
#include <string>
#include <fstream>


bool 
check_file_modified(const std::string& path, 
                    fs::file_time_type last_modified_time) {
    try {
        auto current_time = fs::last_write_time(path);
        return current_time > last_modified_time;
    } catch (const std::exception& e) {
        return false;
    }
}

unsigned int 
count_file_lines(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return 0; 
    }

    unsigned int count = 0;
    std::string line;
    
    while (std::getline(file, line)) {
        bool is_empty = true;
        for (char c : line) {
            if (!std::isspace(static_cast<unsigned char>(c))) {
                is_empty = false;
                break;
            }
        }
        if (!is_empty) {
            count++;
        }
    }
    return count;
}
