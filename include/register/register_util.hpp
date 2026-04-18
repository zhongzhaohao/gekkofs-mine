/*
  Copyright 2018-2024, Barcelona Supercomputing Center (BSC), Spain
  Copyright 2015-2024, Johannes Gutenberg Universitaet Mainz, Germany
*/

#ifndef GKFS_REGISTER_UTIL_HPP
#define GKFS_REGISTER_UTIL_HPP

#include <cstdint>
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

int
register_registry_mallea(const std::string& unique_id,
                         const std::string& username,
                         const std::string& exec_app_path,
                         const std::string& paras,
                         const std::string& hostconfigfile,
                         const std::string& hostfile,
                         uint32_t nodes,
                         uint32_t ppn,
                         bool force);

int
query_registry_mallea(const std::string& output_path);

int
unregister_registry_mallea(const std::string& unique_id);

} // namespace gkfs::registers

#endif // GKFS_REGISTER_UTIL_HPP
