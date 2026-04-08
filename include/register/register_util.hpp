/*
  Copyright 2018-2024, Barcelona Supercomputing Center (BSC), Spain
  Copyright 2015-2024, Johannes Gutenberg Universitaet Mainz, Germany
*/

#ifndef GKFS_REGISTER_UTIL_HPP
#define GKFS_REGISTER_UTIL_HPP

#include <string>

namespace gkfs::registers {

void
read_env(std::string& workflow, std::string& hostfile,
         std::string& hostconfigfile, std::string& mergeflows);

bool
init_registry_client();

std::string
read_registry_file();

bool
connect_registry(const std::string& registry_addr);

int
request_registry();

int
register_registry(const std::string& workflow,
                  const std::string& hostconfigfile,
                  const std::string& hostfile);

} // namespace gkfs::registers

#endif // GKFS_REGISTER_UTIL_HPP