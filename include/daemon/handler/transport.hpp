/*
  Copyright 2018-2024, Barcelona Supercomputing Center (BSC), Spain
  Copyright 2015-2024, Johannes Gutenberg Universitaet Mainz, Germany
*/

#ifndef GEKKOFS_DAEMON_TRANSPORT_HPP
#define GEKKOFS_DAEMON_TRANSPORT_HPP

#include <ctime>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <cstring>
#include <sstream>
#include <vector>
#include <memory>
#include <stdexcept>
#include <variant>

#define Div(a, b) ( ((a) + (b) - 1) / (b))

struct Transport_options {
    size_t count;
    size_t offset = 0;
    size_t buffer_size = 8 * 1024 * 1024;
    int flag = 0;
    unsigned long threads = 4;
    unsigned long n_buffers = 2;
    size_t o_direct_blk_size = 4096;
    size_t block_size = 4 * 1024 * 1024;

    std::string serialize() const {
        return std::to_string(count) + "|" + std::to_string(offset) + "|" 
             + std::to_string(buffer_size) + "|" + std::to_string(flag) + "|" 
             + std::to_string(threads) + "|" + std::to_string(n_buffers) + "|" 
             + std::to_string(o_direct_blk_size) + "|" + std::to_string(block_size);
    }

    static Transport_options deserialize(const std::string& s) {
        std::vector<std::string> parts;
        std::string part;
        for (std::istringstream iss(s); std::getline(iss, part, '|'); parts.push_back(part));
        if (parts.size() != 8) throw std::invalid_argument("格式错误");
        return {
            std::stoull(parts[0]),
            std::stoull(parts[1]),
            std::stoull(parts[2]),
            std::stoi(parts[3]),
            std::stoul(parts[4]),
            std::stoul(parts[5]),
            std::stoull(parts[6]),
            std::stoull(parts[7])
        };
    }
};

struct AlignedDeleter {
    void operator()(void* ptr) const {
        std::free(ptr);
    }
};


class ScopedBuffer {
public:
    ScopedBuffer(size_t size, bool require_aligned, size_t alignment = 4096)
        : size_(size), aligned_(require_aligned) 
    {
        if (require_aligned) {
            void* ptr;
            if (posix_memalign(&ptr, alignment, size) != 0) {
                throw std::bad_alloc();
            }
            buffer_ = std::unique_ptr<void, AlignedDeleter>(ptr);
        } else {
            buffer_ = std::unique_ptr<char[]>(new char[size]);
        }
    }

    void* get() const noexcept { 
        return std::visit([](const auto& ptr) { 
            return static_cast<void*>(ptr.get()); 
        }, buffer_);
    }

private:
    size_t size_;
    bool aligned_;
    std::variant<
        std::unique_ptr<void, AlignedDeleter>,
        std::unique_ptr<char[]>> buffer_;
};

int forward_transport(const std::string& src, const std::string& dest, 
                    const std::string& opts_str);

#endif // GEKKOFS_STAGE_HPP