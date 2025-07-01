/*
 * (C) 2015 The University of Chicago
 *
 * See COPYRIGHT in top-level directory.
 */

#ifndef __MY_RPC
#define __MY_RPC

#include <margo.h>
#include <map>
#include <common/registry_merge_tree.hpp>
#include <filesystem>
#include <fstream>
namespace fs = std::filesystem;

struct flow_info {
    std::string hfile;
    std::string hcfile;
    unsigned int lines;
    fs::file_time_type last_modified_time;
};

static std::map<std::string, flow_info> job_flows; 
static TreeManager tree_manager;

DECLARE_MARGO_RPC_HANDLER(rpc_srv_registry_request)

DECLARE_MARGO_RPC_HANDLER(rpc_srv_registry_register)

#endif /* __MY_RPC */
