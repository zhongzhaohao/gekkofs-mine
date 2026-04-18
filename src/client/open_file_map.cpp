/*
  Copyright 2018-2024, Barcelona Supercomputing Center (BSC), Spain
  Copyright 2015-2024, Johannes Gutenberg Universitaet Mainz, Germany

  This software was partially supported by the
  EC H2020 funded project NEXTGenIO (Project ID: 671951, www.nextgenio.eu).

  This software was partially supported by the
  ADA-FS project under the SPPEXA project funded by the DFG.

  This file is part of GekkoFS' POSIX interface.

  GekkoFS' POSIX interface is free software: you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation, either version 3 of the License,
  or (at your option) any later version.

  GekkoFS' POSIX interface is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with GekkoFS' POSIX interface.  If not, see
  <https://www.gnu.org/licenses/>.

  SPDX-License-Identifier: LGPL-3.0-or-later
*/

#include <client/open_file_map.hpp>
#include <client/open_dir.hpp>
#include <client/preload.hpp>
#include <client/preload_util.hpp>
#include <client/logging.hpp>

/* --FGAP-- */
#include <cstdlib>  // getenv
#include <fstream>  // read config
/* --FGAP-- */

extern "C" {
#include <fcntl.h>
}

using namespace std;

namespace gkfs::filemap {

OpenFile::OpenFile(const string& path, const int flags, FileType type)
    : type_(type), path_(path) {
    // set flags to OpenFile
    if(flags & O_CREAT)
        flags_[gkfs::utils::to_underlying(OpenFile_flags::creat)] = true;
    if(flags & O_APPEND)
        flags_[gkfs::utils::to_underlying(OpenFile_flags::append)] = true;
    if(flags & O_TRUNC)
        flags_[gkfs::utils::to_underlying(OpenFile_flags::trunc)] = true;
    if(flags & O_RDONLY)
        flags_[gkfs::utils::to_underlying(OpenFile_flags::rdonly)] = true;
    if(flags & O_WRONLY)
        flags_[gkfs::utils::to_underlying(OpenFile_flags::wronly)] = true;
    if(flags & O_RDWR)
        flags_[gkfs::utils::to_underlying(OpenFile_flags::rdwr)] = true;

    pos_ = 0; // If O_APPEND flag is used, it will be used before each write.
}

OpenFileMap::OpenFileMap() : fd_idx(10000), fd_validation_needed(false) {}

string
OpenFile::path() const {
    return path_;
}

void
OpenFile::path(const string& path) {
    OpenFile::path_ = path;
}

unsigned long
OpenFile::pos() {
    lock_guard<mutex> lock(pos_mutex_);
    return pos_;
}

void
OpenFile::pos(unsigned long pos) {
    lock_guard<mutex> lock(pos_mutex_);
    OpenFile::pos_ = pos;
}

bool
OpenFile::get_flag(OpenFile_flags flag) {
    lock_guard<mutex> lock(pos_mutex_);
    return flags_[gkfs::utils::to_underlying(flag)];
}

void
OpenFile::set_flag(OpenFile_flags flag, bool value) {
    lock_guard<mutex> lock(flag_mutex_);
    flags_[gkfs::utils::to_underlying(flag)] = value;
}

FileType
OpenFile::type() const {
    return type_;
}

// OpenFileMap starts here

shared_ptr<OpenFile>
OpenFileMap::get(int fd) {
    lock_guard<recursive_mutex> lock(files_mutex_);
    auto f = files_.find(fd);
    if(f == files_.end()) {
        return nullptr;
    } else {
        return f->second;
    }
}

shared_ptr<OpenDir>
OpenFileMap::get_dir(int dirfd) {
    auto f = get(dirfd);
    if(f == nullptr || f->type() != FileType::directory) {
        return nullptr;
    }
    return static_pointer_cast<OpenDir>(f);
}

bool
OpenFileMap::exist(const int fd) {
    lock_guard<recursive_mutex> lock(files_mutex_);
    auto f = files_.find(fd);
    return !(f == files_.end());
}

int
OpenFileMap::safe_generate_fd_idx_() {
    auto fd = generate_fd_idx();
    /*
     * Check if fd is still in use and generate another if yes
     * Note that this can only happen once the all fd indices within the int has
     * been used to the int::max Once this limit is exceeded, we set fd_idx back
     * to 3 and begin anew. Only then, if a file was open for a long time will
     * we have to generate another index.
     *
     * This situation can only occur when all fd indices have been given away
     * once and we start again, in which case the fd_validation_needed flag is
     * set. fd_validation is set to false, if
     */
    if(fd_validation_needed) {
        while(exist(fd)) {
            fd = generate_fd_idx();
        }
    }
    return fd;
}

int
OpenFileMap::add(std::shared_ptr<OpenFile> open_file) {
    auto fd = safe_generate_fd_idx_();
    lock_guard<recursive_mutex> lock(files_mutex_);
    files_.insert(make_pair(fd, open_file));
    return fd;
}

bool
OpenFileMap::remove(const int fd) {
    lock_guard<recursive_mutex> lock(files_mutex_);
    auto f = files_.find(fd);
    if(f == files_.end()) {
        return false;
    }
    files_.erase(fd);
    if(fd_validation_needed && files_.empty()) {
        fd_validation_needed = false;
        LOG(DEBUG, "fd_validation flag reset");
    }
    return true;
}

bool
OpenFileMap::path_open(const std::string& path, FileType type) {
    lock_guard<recursive_mutex> lock(files_mutex_);
    for(const auto& open_file : files_) {
        if(open_file.second->type() == type && open_file.second->path() == path) {
            return true;
        }
    }
    return false;
}

int
OpenFileMap::dup(const int oldfd) {
    lock_guard<recursive_mutex> lock(files_mutex_);
    auto open_file = get(oldfd);
    if(open_file == nullptr) {
        errno = EBADF;
        return -1;
    }
    auto newfd = safe_generate_fd_idx_();
    files_.insert(make_pair(newfd, open_file));
    return newfd;
}

int
OpenFileMap::dup2(const int oldfd, const int newfd) {
    lock_guard<recursive_mutex> lock(files_mutex_);
    auto open_file = get(oldfd);
    if(open_file == nullptr) {
        errno = EBADF;
        return -1;
    }
    if(oldfd == newfd)
        return newfd;
    // remove newfd if exists in filemap silently
    if(exist(newfd)) {
        remove(newfd);
    }
    // to prevent duplicate fd idx in the future. First three fd are reservered
    // by os streams that we do not overwrite
    if(get_fd_idx() < newfd && newfd != 0 && newfd != 1 && newfd != 2)
        fd_validation_needed = true;
    files_.insert(make_pair(newfd, open_file));
    return newfd;
}

/**
 * Generate new file descriptor index to be used as an fd within one process
 * @return fd_idx
 */
int
OpenFileMap::generate_fd_idx() {
    // We need a mutex here for thread safety
    std::lock_guard<std::mutex> inode_lock(fd_idx_mutex);
    if(fd_idx == std::numeric_limits<int>::max()) {
        LOG(WARNING,
            "File descriptor index exceeded ints max value. Setting it back to 100000");
        /*
         * Setting fd_idx back to 3 could have the effect that fd are given
         * twice for different path. This must not happen. Instead a flag is set
         * which tells can tell the OpenFileMap that it should check if this fd
         * is really safe to use.
         */
        fd_idx = 10000;
        fd_validation_needed = true;
    }
    return fd_idx++;
}

int
OpenFileMap::get_fd_idx() {
    std::lock_guard<std::mutex> inode_lock(fd_idx_mutex);
    return fd_idx;
}

} // namespace gkfs::filemap


/* --FGAP-- */
namespace gkfs::filetagmap {
	
FileTagMap::FileTagMap() {
    //std::cout<< "[fgap_debug] FileTagMap initialized." << std::endl;
    LOG(INFO, "[fgap_debug] FileTagMap initialized.");
}	
    
//void FileTagMap::get_tags_by_env() {
//	char* fileTagStr = std::getenv("FGAP_FILE_TAG"); // get env "FGAP_FILE_TAG"
//	std::cout<< "[fgap_debug] in env: " << fileTagStr << std::endl;
//        if (fileTagStr != nullptr) { 
//	    std::cout << "[fgap_debug] parse env" << std::endl;
//            std::istringstream stream(fileTagStr);
//            std::string pair;
//
//            while (std::getline(stream, pair, ',')) {
//                size_t colonPos = pair.find(':');
//                if (colonPos != std::string::npos) {
//                    std::string filename = pair.substr(0, colonPos);
//                    std::string tag = pair.substr(colonPos + 1);
//                    mapping_[filename] = tag; // store filename and tag
//                }
//            }
//        } else {
//		std::cout << "[fgap_debug] Environment variable FGAP_FILE_TAG is not set." << std::endl;
//        }
//}


// fgap: set file tag using config file
// usage: export FGAP_FILE_TAG=/path-to/file_tag.config
// 	[/path-to/file_tag.config]:
// 	file1 tag1
// 	file2 tag2
void FileTagMap::get_tags_by_env() {
    char* filePath = std::getenv("FGAP_FILE_TAG"); // get env "FGAP_FILE_TAG"
    //std::cout << "[fgap_debug] in env: " << filePath << std::endl;

    if (filePath != nullptr) {
        //std::cout << "[fgap_debug] parsing file: " << filePath << std::endl;
        LOG(INFO, "[fgap_debug] parsing file: [{}]", filePath);
        std::ifstream file(filePath);

        if (!file) {
            std::cerr << "[fgap_debug] Error opening file: " << filePath << std::endl;
            return;
        }

        std::string line;
        while (std::getline(file, line)) {
            std::istringstream stream(line);
            std::string filename, tag;

            if (stream >> filename >> tag) {
                mapping_[filename] = tag; // store filename and tag
            } else {
                std::cerr << "[fgap_debug] Invalid format in line: " << line << std::endl;
            }
        }
    } else {
        LOG(INFO, "[fgap_debug] Environment variable FGAP_FILE_TAG is not set.");
    }
}

// Method to parse the FILE_TAG environment variable
void FileTagMap::parse(std::string& fileTagStr) {
    std::istringstream stream(fileTagStr);
    std::string pair;

    while (std::getline(stream, pair, ',')) {
        size_t colonPos = pair.find(':');
        if (colonPos != std::string::npos) {
            std::string filename = pair.substr(0, colonPos);
            std::string tag = pair.substr(colonPos + 1);
            mapping_[filename] = tag;
        }
    }
}

// Method to check if a filename exists in the map
bool FileTagMap::exist(std::string& filename) {
    return mapping_.find(filename) != mapping_.end();
}

// Method to get tag by filename
std::string FileTagMap::get_tag(std::string& filename) {
    auto it = mapping_.find(filename);
    if (it != mapping_.end()) {
        return it->second; // return tag
    } else {
        return ""; // if not found, return null
    }
}

// Method to print the contents of the map
void FileTagMap::print() {
    //std::cout << "[fgap_debug] Parsed FGAP_FILE_TAG:" << std::endl;
    LOG(INFO, "[fgap_debug] Parsed FGAP_FILE_TAG");
    for (const auto& entry : mapping_) {
        //std::cout << "[fgap_debug] Filename: " << entry.first << ", Tag: " << entry.second << std::endl;
    LOG(INFO, "[fgap_debug] Filename: [{}], Tag: {}", entry.first, entry.second);
    }
}

// get and set backend filesystems by getenv "FGAP_FS"
//void FileTagMap::get_set_fs_by_env() {
//    const char* env_var = std::getenv("FGAP_FS"); // get env "FGAP_FS"
//    if (!env_var) {
//        std::cerr << "[fgap_debug] Environment variable FGAP_FS is not set." << std::endl;
//        return;
//    }
//
//    std::string fs_path(env_var); // convert to std::string
//    std::istringstream ss(fs_path); // split string using istringstream
//    std::string token;
//    size_t index = 0;
//
//    // set token ',' to split string
//    while (std::getline(ss, token, ',') && index < fs_array_.size()) {
//        fs_array_[index++] = token; // store fs to fs_array
//    }
//
//    // set the remaining array space to null
//    for (; index < fs_array_.size(); ++index) {
//        fs_array_[index] = ""; 
//    }
//}


// fgap: set filesystems using config file
// usage: export FGAP_FS=/path-to/fs.config
//      [/path-to/fs.config]:
//      0 fs0
//      1 fs1
// NOTE: Some Index conflict judgments are missing [TODO]
void FileTagMap::get_set_fs_by_env() {
    char* env_var = std::getenv("FGAP_FS"); // get env "FGAP_FS"
    if (!env_var) {
        return;
    }

    std::string config_file_path(env_var); // convert to std::string
    //std::cout << "[fgap_debug] FGAP_FS in env: " << config_file_path << std::endl;
    LOG(INFO, "[fgap_debug] FGAP_FS in env: {}", config_file_path);
    std::ifstream file(config_file_path);

    if (!file) {
        std::cerr << "[fgap_debug] Error opening file: " << config_file_path << std::endl;
        return;
    }

    for (size_t i = 0; i < fs_array_.size(); ++i) {
        fs_array_[i] = "";
    }

    std::string line;
    size_t index = 0;

    while (std::getline(file, line)) {
        // Split line into index and path
        std::istringstream ss(line);
        std::string idx_str, path;

        if (ss >> idx_str >> path) {
            index = std::stoul(idx_str); // Convert the index from string to size_t
            if (index < fs_array_.size()) {
                fs_array_[index] = path; // Store the path in fs_array_
            } else {
                std::cerr << "[fgap_debug] Index out of bounds: " << index << std::endl;
            }
        } else {
            std::cerr << "[fgap_debug] Invalid format in line: " << line << std::endl;
        }
    }

}

std::string FileTagMap::get_fs_at_index(size_t index) const {
    if (index >= fs_array_.size()) {
        return ""; // return null for out of bounds
    }
    return fs_array_[index];
}

void FileTagMap::print_all_fs() {
    for (size_t i = 0; i < fs_array_.size(); ++i) {
        //std::cout << "[fgap_debug] fs[" << i << "] = " << fs_array_[i] << std::endl;
        LOG(INFO, "[fgap_debug] fs[{}] = {}", i, fs_array_[i]); 
    }
}


} // end namespace gkfs::filetagmap
/* --FGAP-- */
