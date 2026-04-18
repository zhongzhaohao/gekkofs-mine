/*
 * (C) 2015 The University of Chicago
 *
 * See COPYRIGHT in top-level directory.
 */

#ifndef __MY_RPC
#define __MY_RPC

#include <margo.h>
#include <common/common_defs.hpp>
#include <cstdint>
#include <mutex>
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

struct mallea_info {
    std::string username;
    std::string exec_app_path;
    std::string paras;
    std::string hcfile;
    std::string hfile;
    uint32_t nodes;
    uint32_t ppn;
    std::uint64_t epoch = 0;
    bool resizing = false;
    bool pending_unregister = false;
};

extern std::map<std::string, flow_info> job_flows;
extern std::map<std::string, mallea_info> mallea_jobs;
extern std::map<std::string, std::string> mallea_borrow_table;
extern std::mutex mallea_jobs_mutex;
extern TreeManager tree_manager;

int
forward_daemon_update_epoch(margo_instance_id client_mid,
                            hg_id_t rpc_id,
                            const std::string& daemon_addr,
                            gkfs::rpc::daemon_resize_action action,
                            std::uint64_t epoch,
                            const std::string& hostfile,
                            const std::string& unique_id);

DECLARE_MARGO_RPC_HANDLER(rpc_srv_registry_request)

DECLARE_MARGO_RPC_HANDLER(rpc_srv_registry_register)

DECLARE_MARGO_RPC_HANDLER(rpc_srv_registry_register_mallea)

DECLARE_MARGO_RPC_HANDLER(rpc_srv_registry_query_mallea)

DECLARE_MARGO_RPC_HANDLER(rpc_srv_registry_unregister_mallea)

#endif /* __MY_RPC */
