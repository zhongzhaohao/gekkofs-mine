#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <iostream>
#include <vector>
#include <sstream>
#include <fstream>
#include <set>
#include <queue>
#include <algorithm>
#include <cerrno>
#include <mutex>

#include <registry/my-rpc.hpp>
#include <registry/utils.hpp>

#include <common/common_defs.hpp>
#include <common/rpc/rpc_types.hpp>

std::map<std::string, flow_info> job_flows;
std::map<std::string, mallea_info> mallea_jobs;
std::map<std::string, std::string> mallea_borrow_table;
std::mutex mallea_jobs_mutex;
TreeManager tree_manager;

namespace {

bool
mallea_has_borrow_relation_locked(const std::string& unique_id) {
    if(mallea_borrow_table.count(unique_id) != 0) {
        return true;
    }
    return std::any_of(mallea_borrow_table.begin(), mallea_borrow_table.end(),
                       [&unique_id](const auto& entry) {
                           return entry.second == unique_id;
                       });
}

std::string
serialize_mallea_jobs() {
    std::ostringstream oss;
    std::lock_guard<std::mutex> lock(mallea_jobs_mutex);

    for(const auto& [unique_id, info] : mallea_jobs) {
            oss << "unique_id=" << unique_id << ", username=" << info.username
                << ", exec_app_path=" << info.exec_app_path
                << ", paras=" << info.paras << ", hcfile=" << info.hcfile
                << ", hfile=" << info.hfile << ", nodes=" << info.nodes
                << ", ppn=" << info.ppn << ", epoch=" << info.epoch
                << ", resizing="
                << (info.resizing ? "true" : "false")
                << ", pending_unregister="
                << (info.pending_unregister ? "true" : "false") << '\n';
    }
    for(const auto& [borrower, lender] : mallea_borrow_table) {
        oss << "borrower=" << borrower << ", lender=" << lender << '\n';
    }

    return oss.str();
}

[[maybe_unused]] void
complete_mallea_resize_locked(const std::string& unique_id) {
    auto it = mallea_jobs.find(unique_id);
    if(it == mallea_jobs.end()) {
        return;
    }

    if(it->second.pending_unregister &&
       !mallea_has_borrow_relation_locked(unique_id)) {
        mallea_jobs.erase(it);
        return;
    }

    it->second.resizing = false;
}

} // namespace

hg_return_t
rpc_srv_registry_request(hg_handle_t handle) {
    rpc_registry_request_in_t in;
    rpc_registry_request_out_t out;
    out.err = 0;
    std::vector<std::string> flow_arr = {};

    auto ret = margo_get_input(handle, &in);
    if(ret != HG_SUCCESS)
        assert(ret == HG_SUCCESS);
    auto flows = in.merge_flows;
    auto hfile = in.merge_hfile;
    auto hcfile = in.merge_hcfile;
    auto flow_name = in.flow;
    std::cout << flows << " " << hcfile << " " << hfile << " "
              << flow_name << std::endl;
    try {
        std::stringstream ss(flows);
        std::string flow;
        while(std::getline(ss, flow, ';')) {
            flow_arr.push_back(flow);
        }
        bool ok = tree_manager.MergeTree(flow_name, hfile, hcfile, flow_arr);
        (void) ok;
        job_flows[flow_name] = {hfile,
                                hcfile,
                                count_file_lines(hfile),
                                fs::last_write_time(hfile)};
        chmod(hfile, S_IRUSR | S_IRGRP | S_IROTH);
        chmod(hcfile, S_IRUSR | S_IRGRP | S_IROTH);

    } catch(const std::exception& e) {
        std::cout << "Failed to respond my rpc ult\n" << e.what();
        out.err = -1;
    }

    auto hret = margo_respond(handle, &out);
    if(hret != HG_SUCCESS) {
        std::cout << "Failed to respond my rpc ult\n";
    }

    margo_free_input(handle, &in);
    margo_destroy(handle);
    return HG_SUCCESS;
}
DEFINE_MARGO_RPC_HANDLER(rpc_srv_registry_request)

hg_return_t
rpc_srv_registry_register(hg_handle_t handle) {
    rpc_registry_register_in_t in;
    rpc_err_out_t out;
    out.err = 0;

    auto ret = margo_get_input(handle, &in);
    if(ret != HG_SUCCESS)
        assert(ret == HG_SUCCESS);
    auto flow = in.work_flow;
    auto hfile = in.hfile;
    auto hcfile = in.hcfile;
    try {
        std::cout << flow << " " << hcfile << " " << hfile << std::endl;
        unsigned int lines = count_file_lines(hfile);
        auto last_modified_time = fs::last_write_time(hfile);
        flow_info finfo = {hfile, hcfile, lines, last_modified_time};
        if(job_flows.count(flow)) {
            std::string old_hfile = job_flows[flow].hfile;
            if(old_hfile == hfile) {
                if(check_file_modified(hfile, job_flows[flow].last_modified_time)) {
                    std::cout << "modified " << std::endl;
                    job_flows[flow] = finfo;
                    tree_manager.AddTree(flow, hfile);
                } else {
                    std::cout << "no modified. " << std::endl;
                }
            } else {
                std::cout << "new file and we flush it" << std::endl;
                job_flows[flow] = finfo;
                tree_manager.AddTree(flow, hfile);
            }
        } else {
            std::cout << "no such flow " << std::endl;
            job_flows[flow] = finfo;
            tree_manager.AddTree(flow, hfile);
        }

    } catch(const std::exception& e) {
        (void) e;
        out.err = -1;
    }
    auto hret = margo_respond(handle, &out);
    if(hret != HG_SUCCESS) {
        std::cout << "Failed to respond my rpc ult\n";
    }
    margo_free_input(handle, &in);
    margo_destroy(handle);
    return HG_SUCCESS;
}
DEFINE_MARGO_RPC_HANDLER(rpc_srv_registry_register)

hg_return_t
rpc_srv_registry_register_mallea(hg_handle_t handle) {
    rpc_registry_register_mallea_in_t in;
    rpc_err_out_t out;
    out.err = 0;

    auto ret = margo_get_input(handle, &in);
    if(ret != HG_SUCCESS)
        assert(ret == HG_SUCCESS);

    {
        std::lock_guard<std::mutex> lock(mallea_jobs_mutex);
        auto existing = mallea_jobs.find(in.unique_id);
        if(existing != mallea_jobs.end() && existing->second.resizing) {
            out.err = EBUSY;
        } else if(existing != mallea_jobs.end() && !in.force) {
            out.err = EEXIST;
        } else {
            mallea_borrow_table.erase(in.unique_id);
            mallea_jobs[in.unique_id] = {in.username,
                                         in.exec_app_path,
                                         in.paras,
                                         in.hcfile,
                                                 in.hfile,
                                                 in.nodes,
                                                 in.ppn,
                                                 0,
                                                 false,
                                                 false};
        }
    }

    if(out.err == EEXIST) {
        std::cout << "registry_register_mallea duplicate unique_id: "
                  << in.unique_id << std::endl;
    } else if(out.err == EBUSY) {
        std::cout << "registry_register_mallea rejected resizing unique_id: "
                  << in.unique_id << std::endl;
    } else {
        std::cout << "received registry_register_mallea: "
                  << in.unique_id << " " << in.username << " "
                  << in.exec_app_path << " " << in.paras << " " << in.hcfile
                  << " " << in.hfile << " " << in.nodes << " " << in.ppn
                  << " force=" << static_cast<int>(in.force) << std::endl;
    }

    auto hret = margo_respond(handle, &out);
    if(hret != HG_SUCCESS) {
        std::cout << "Failed to respond registry_register_mallea rpc\n";
    }
    margo_free_input(handle, &in);
    margo_destroy(handle);
    return HG_SUCCESS;
}
DEFINE_MARGO_RPC_HANDLER(rpc_srv_registry_register_mallea)

hg_return_t
rpc_srv_registry_query_mallea(hg_handle_t handle) {
    rpc_registry_query_mallea_in_t in{};
    rpc_registry_query_mallea_out_t out{};
    std::string serialized;
    out.err = 0;

    auto ret = margo_get_input(handle, &in);
    if(ret != HG_SUCCESS)
        assert(ret == HG_SUCCESS);

    try {
        serialized = serialize_mallea_jobs();
        out.db_val = serialized.c_str();
    } catch(const std::exception&) {
        out.err = EBUSY;
        out.db_val = "";
    }

    auto hret = margo_respond(handle, &out);
    if(hret != HG_SUCCESS) {
        std::cout << "Failed to respond registry_query_mallea rpc\n";
    }
    margo_free_input(handle, &in);
    margo_destroy(handle);
    return HG_SUCCESS;
}
DEFINE_MARGO_RPC_HANDLER(rpc_srv_registry_query_mallea)

hg_return_t
rpc_srv_registry_unregister_mallea(hg_handle_t handle) {
    rpc_registry_unregister_mallea_in_t in{};
    rpc_err_out_t out{};
    out.err = 0;

    auto ret = margo_get_input(handle, &in);
    if(ret != HG_SUCCESS)
        assert(ret == HG_SUCCESS);

    {
        std::lock_guard<std::mutex> lock(mallea_jobs_mutex);
        auto it = mallea_jobs.find(in.unique_id);
        if(it == mallea_jobs.end()) {
            out.err = ENOENT;
        } else if(it->second.resizing ||
                  mallea_has_borrow_relation_locked(in.unique_id)) {
            it->second.pending_unregister = true;
            std::cout
                    << "registry_unregister_mallea delayed for busy unique_id: "
                    << in.unique_id << std::endl;
        } else {
            mallea_borrow_table.erase(in.unique_id);
            mallea_jobs.erase(it);
            std::cout << "registry_unregister_mallea removed unique_id: "
                      << in.unique_id << std::endl;
        }
    }

    auto hret = margo_respond(handle, &out);
    if(hret != HG_SUCCESS) {
        std::cout << "Failed to respond registry_unregister_mallea rpc\n";
    }
    margo_free_input(handle, &in);
    margo_destroy(handle);
    return HG_SUCCESS;
}
DEFINE_MARGO_RPC_HANDLER(rpc_srv_registry_unregister_mallea)
