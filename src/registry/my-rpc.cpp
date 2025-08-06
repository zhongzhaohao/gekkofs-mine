/*
 * (C) 2015 The University of Chicago
 *
 * See COPYRIGHT in top-level directory.
 */

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

#include <registry/my-rpc.hpp>
#include <registry/utils.hpp>

#include <common/rpc/rpc_types.hpp>
#include <algorithm>


/**
 * @brief Responds to merge request from client 
 * with merge GekkoFS information written into hostfile and hostconfigfile
 * @internal
 * @param handle Mercury RPC handle
 * @return Mercury error code to Mercury
 */
hg_return_t
rpc_srv_registry_request(hg_handle_t handle)
{   
    //std::cout<< "succeed in getting flows hfile hcfile\n" <<std::endl;
    rpc_registry_request_in_t in;
    rpc_registry_request_out_t out;out.err = 0;
    std::vector<std::string> flow_arr = {};

    auto ret = margo_get_input(handle, &in);
    if(ret != HG_SUCCESS)
    assert(ret == HG_SUCCESS);
    auto flows = in.merge_flows;
    auto hfile = in.merge_hfile;
    auto hcfile = in.merge_hcfile;
    auto flow_name = in.flow;
    std::cout<< flows<<" " << hcfile <<" "<< hfile <<" "<< flow_name<<std::endl;
    try {
        std::stringstream ss(flows);
        std::string flow;
        //Separate the flows with; , flow_arr stores all the work flows to be soon merged
        while (std::getline(ss, flow, ';')) {
            flow_arr.push_back(flow);
        }
        bool ok = tree_manager.MergeTree(
            flow_name, hfile, hcfile, flow_arr
        );
        job_flows[flow_name] = {hfile,hcfile,count_file_lines(hfile), fs::last_write_time(hfile)};
        // set read only
        chmod(hfile, S_IRUSR | S_IRGRP | S_IROTH);
        chmod(hcfile, S_IRUSR | S_IRGRP | S_IROTH);

    }   catch(const std::exception& e) {
            std::cout<< "Failed to respond my rpc ult\n" << e.what();
            out.err = -1;
    }

    //std::cout<< "request out err " << out.err <<std::endl;
    auto hret = margo_respond(handle, &out);
    if(hret != HG_SUCCESS) {
        std::cout<< "Failed to respond my rpc ult\n";
    }

    margo_free_input(handle, &in);
    margo_destroy(handle);
    return HG_SUCCESS;
}
DEFINE_MARGO_RPC_HANDLER(rpc_srv_registry_request)

/**
 * @brief Responds to workflow register from client, save system info where workflow is located
 * @internal
 * @param handle Mercury RPC handle
 * @return Mercury error code to Mercury
 */
hg_return_t
rpc_srv_registry_register(hg_handle_t handle)
{
    //std::cout<< "succeed in getting flows hfile hcfile " <<std::endl;
    rpc_registry_register_in_t in;
    rpc_err_out_t out;

    auto ret = margo_get_input(handle, &in);
    if(ret != HG_SUCCESS)
    assert(ret == HG_SUCCESS);
    auto flow = in.work_flow;
    auto hfile = in.hfile;
    auto hcfile = in.hcfile;
    try {
        //Store the hostconfigfile and hostfile of the system where the workflow is located
        //if flow exists: 
        //hfile path different: allow change and del whole old merge tree where flow exists
        //hfile not modified--same flow as before, do nothing
        //hfile modified--allow change and del whole old tree where flow exists
        //changjing: 融合树的子树register,no worry?
        // TODO registry掉线重启
        // TODO mergefile 实际未merge，注册工作流
        // TODO Add flow create time: what if registry getdown
        std::cout<< flow<<" " << hcfile <<" "<< hfile <<std::endl;
        unsigned int lines = count_file_lines(hfile);
        auto last_modified_time = fs::last_write_time(hfile);
        flow_info finfo = {hfile, hcfile, lines, last_modified_time};
        if(job_flows.count(flow)){
            std::string old_hfile = job_flows[flow].hfile;
            if(old_hfile == hfile){
                if(check_file_modified(hfile, job_flows[flow].last_modified_time)){
                    std::cout<< "modified "<< std::endl;
                    // auto root = tree_manager.flownameToRoot[flow];
                    // tree_manager.flownameToRoot.erase(root.flowname);
                    job_flows[flow] = finfo;
                    tree_manager.AddTree(flow, hfile);
                } else {
                    std::cout<< "no modified. "<< std::endl;
                    ; //do nothing 
                }
            } else {
                std::cout<< "new file and we flush it"<< std::endl;
                job_flows[flow] = finfo;
                tree_manager.AddTree(flow, hfile);
            }
        } else {
            std::cout<< "no such flow "<< std::endl;
            job_flows[flow] = finfo;
            tree_manager.AddTree(flow, hfile);
        }

    } catch(const std::exception& e) {
        out.err = -1;
    }
    //std::cout<< "register out err " << out.err <<std::endl;
    auto hret = margo_respond(handle, &out);
    if(hret != HG_SUCCESS) {
        std::cout<< "Failed to respond my rpc ult\n";
    }
    margo_free_input(handle, &in);
    margo_destroy(handle);
    return HG_SUCCESS;
}
DEFINE_MARGO_RPC_HANDLER(rpc_srv_registry_register)
