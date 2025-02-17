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

#ifndef GEKKOFS_USER_FUNCTIONS_HPP
#define GEKKOFS_USER_FUNCTIONS_HPP
#include <string>
#include <cstdint>
#include <vector>
#include <utility>
#include <set>
extern "C" {
#include <sys/types.h>
#include <sys/stat.h>
}

struct linux_dirent64;
using namespace std;
namespace gkfs {

namespace rpc {

int
forward_stage(size_t host_id, const std::string& in_path,const std::string& out_path,
              const std::string& opts);

int
forward_stage_metadata(const std::string& path,const mode_t mode, const size_t size,
                    const int flag, std::string &attr) ;

pair<int, ssize_t>
forward_write(const string& path, const void* buf, const off64_t offset,
              const size_t write_size, const int8_t num_copies);

pair<int, ssize_t>
forward_read(const string& path, void* buf, const off64_t offset,
             const size_t read_size, const int8_t num_copies,
             std::set<int8_t>& failed);
}// namespace rpc

} // namespace gkfs
extern "C" int
gkfs_init();

extern "C" int
gkfs_end();

#endif // GEKKOFS_USER_FUNCTIONS_HPP
