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

#ifndef GEKKOFS_OPEN_FILE_MAP_HPP
#define GEKKOFS_OPEN_FILE_MAP_HPP

#include <map>
#include <mutex>
#include <memory>
#include <atomic>
#include <array>
#include <string>
#include <unordered_map> /* --FGAP-- */
namespace gkfs::filemap {

/* Forward declaration */
class OpenDir;


enum class OpenFile_flags {
    append = 0,
    creat,
    trunc,
    rdonly,
    wronly,
    rdwr,
    cloexec,
    flag_count // this is purely used as a size variable of this enum class
};

enum class FileType { regular, directory };

class OpenFile {
protected:
    FileType type_;
    std::string path_;
    std::array<bool, static_cast<int>(OpenFile_flags::flag_count)> flags_ = {
            {false}};
    unsigned long pos_;
    std::mutex pos_mutex_;
    std::mutex flag_mutex_;

public:
    // multiple threads may want to update the file position if fd has been
    // duplicated by dup()

    OpenFile(const std::string& path, int flags,
             FileType type = FileType::regular);

    ~OpenFile() = default;

    // getter/setter
    std::string
    path() const;

    void
    path(const std::string& path_);

    unsigned long
    pos();

    void
    pos(unsigned long pos_);

    bool
    get_flag(OpenFile_flags flag);

    void
    set_flag(OpenFile_flags flag, bool value);

    FileType
    type() const;
};


class OpenFileMap {

private:
    std::map<int, std::shared_ptr<OpenFile>> files_;
    std::recursive_mutex files_mutex_;

    int
    safe_generate_fd_idx_();

    /*
     * TODO: Setting our file descriptor index to a specific value is dangerous
     * because we might clash with the kernel. E.g., if we would passthrough and
     * not intercept and the kernel assigns a file descriptor but we will later
     * use the same fd value, we will intercept calls that were supposed to be
     * going to the kernel. This works the other way around too. To mitigate
     * this issue, we set the initial fd number to a high value. We "hope" that
     * we do not clash but this is no permanent solution. Note: This solution
     * will probably work well already for many cases because kernel fd values
     * are reused, unlike to ours. The only case where we will clash with the
     * kernel is, if one process has more than 100000 files open at the same
     * time.
     */
    int fd_idx;
    std::mutex fd_idx_mutex;
    std::atomic<bool> fd_validation_needed;

public:
    OpenFileMap();

    std::shared_ptr<OpenFile>
    get(int fd);

    std::shared_ptr<OpenDir>
    get_dir(int dirfd);

    bool
    exist(int fd);

    int add(std::shared_ptr<OpenFile>);

    bool
    remove(int fd);

    bool
    path_open(const std::string& path, FileType type);

    int
    dup(int oldfd);

    int
    dup2(int oldfd, int newfd);

    int
    generate_fd_idx();

    int
    get_fd_idx();
};

} // namespace gkfs::filemap

/* --FGAP-- 
* define namespace gkfs::filetagmap
*/
namespace gkfs::filetagmap {

class FileTagMap {
    
private:
    std::unordered_map<std::string, std::string> mapping_;
    std::mutex map_mutex; // lock to protect the map accessing concurrently

    std::array<std::string, 30> fs_array_; // 30 fs types



public:

    FileTagMap();

    ~FileTagMap() = default;

    // Method to parse the FILE_TAG environment variable
    void get_tags_by_env();
    
    // Method to parse the input string
    void parse(std::string& fileTagStr);

    // Method to check if a filename exists in the map
    bool exist(std::string& filename);

    // Method to get tag of filenamme
    std::string get_tag(std::string& filename);

    // Method to print the contents of the map
    void print();

    // set fs_array_ by "FGAP_FS" env
    void get_set_fs_by_env();

    // return fs_array_[index]
    std::string get_fs_at_index(size_t index) const;

    void print_all_fs();

};
    
    
} // end namespace gkfs::filetagmap
/* --FGAP-- */

#endif // GEKKOFS_OPEN_FILE_MAP_HPP
