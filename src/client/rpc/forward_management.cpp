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

#include <client/rpc/forward_management.hpp>
#include <client/logging.hpp>
#include <client/preload_util.hpp>
#include <client/rpc/rpc_types.hpp>
#include <common/bloom_filter.hpp>
#include <fstream>
#include <random>
#include <algorithm>
namespace gkfs::rpc {

/**
 * Gets fs configuration information from the running daemon and transfers it to
 * the memory of the library
 * @return
 */
bool
forward_get_fs_config() {

    auto endp = CTX->hosts().at(CTX->local_host_id());
    gkfs::rpc::fs_config::output out;

    bool found = false;
    size_t idx = 0;
    while(!found && idx <= CTX->hosts().size()) {
        try {
            LOG(DEBUG, "Retrieving file system configurations from daemon");
            // TODO(amiranda): add a post() with RPC_TIMEOUT to hermes so that
            // we can retry for RPC_TRIES (see old commits with margo)
            // TODO(amiranda): hermes will eventually provide a post(endpoint)
            // returning one result and a broadcast(endpoint_set) returning a
            // result_set. When that happens we can remove the .at(0) :/
            out = ld_network_service->post<gkfs::rpc::fs_config>(endp).get().at(
                    0);
            found = true;
        } catch(const std::exception& ex) {
            LOG(ERROR,
                "Retrieving fs configurations from daemon, possible reattempt at peer: {}",
                idx);
            endp = CTX->hosts().at(idx++);
        }
    }

    if(!found)
        return false;

    CTX->mountdir(out.mountdir());
    LOG(INFO, "Mountdir: '{}'", CTX->mountdir());

    CTX->fs_conf()->rootdir = out.rootdir();
    CTX->fs_conf()->atime_state = out.atime_state();
    CTX->fs_conf()->mtime_state = out.mtime_state();
    CTX->fs_conf()->ctime_state = out.ctime_state();
    CTX->fs_conf()->link_cnt_state = out.link_cnt_state();
    CTX->fs_conf()->blocks_state = out.blocks_state();
    CTX->fs_conf()->uid = out.uid();
    CTX->fs_conf()->gid = out.gid();

    LOG(DEBUG, "Got response with mountdir {}", out.mountdir());

    return true;
}

/**
 * Gets bloom filter from all daemons
 * @return
 */
bool 
forward_get_bloom_filter(size_t size) {

    size_t filter_size = size;
    size_t buffer_size = size + 10;
    size_t hosts_size = CTX->hosts().size();
    
    //prepare buffers for bloom filter
    std::vector<std::unique_ptr<char[]>> bufs;
    bufs.reserve(CTX->hosts().size());

    std::vector<hermes::exposed_memory> exposed_buffers;
    exposed_buffers.reserve(hosts_size);

    
    for(std::size_t i = 0; i < hosts_size; ++i){
        try {
            std::unique_ptr<char[]> buf(new char[buffer_size]);
            bufs.push_back(std::move(buf));
            exposed_buffers.emplace_back(ld_network_service->expose(
                    std::vector<hermes::mutable_buffer>{hermes::mutable_buffer{
                            bufs.back().get(), buffer_size}},
                    hermes::access_mode::write_only));
        } catch(const std::exception& ex) {
            LOG(ERROR, "{}() Failed to expose buffers for RMA. err '{}'",
                __func__, ex.what());
            return false;
        }
    }

    std::vector<uint64_t> host_ids(hosts_size);
    std::iota(host_ids.begin(), host_ids.end(), 0);
    std::random_device rd; // obtain a random number from hardware
    std::mt19937 g(rd());  // seed the random generator
    std::shuffle(host_ids.begin(), host_ids.end(), g); // Shuffle hosts vector
    std::vector<hermes::rpc_handle<gkfs::rpc::Bloom_filter>> handles;
    for (const auto& id : host_ids) {
        try {
            auto endp = CTX->hosts().at(id);
            LOG(DEBUG, "Sending bloom filter RPC to host: {}", endp.to_string());
            
            gkfs::rpc::Bloom_filter::input in(exposed_buffers[id]);
            handles.emplace_back(
                ld_network_service->post<gkfs::rpc::Bloom_filter>(endp, in));

        } catch (const std::exception& ex) {
            LOG(ERROR, "Failed to forward RPC to host {}: {}", 
                endp.to_string(), ex.what());
            return false;
        }
    }

    // get responses
    auto err = 0;
    std::vector<bloom_filter>& filter_vec = CTX->bloom_filter_vec();
    filter_vec.resize(CTX->hosts().size());
    size_t idx = 0;

    for (const auto& h : handles) {
        try {
            auto out = h.get().at(0);
            if (out.err() != 0) {
                LOG(ERROR, "Host {} returned error: {}", idx, out.err());
                err = out.err();
                idx ++;
                continue;
            }
            auto real_id = host_ids[idx];
            void* base_ptr = exposed_buffers[real_id].begin()->data();
            char* raw_buf = reinterpret_cast<char*>(base_ptr);
            //std::cout << "get bloom with size " << buffer_size << std::endl;
            filter_vec[real_id].deserialize(raw_buf, filter_size);

        } catch (const std::exception& ex) {
            LOG(ERROR, "Error receiving bloom filter from host {}: {}", 
                real_id, ex.what());
            err = EBUSY;
        }
        idx ++;
    }
    //std::cout<< "bloom err" <<err << std::endl;
    return err == 0;
}

/**
 * --Multiple GekkoFS--
 * Request Registry to auto merge workflows
 * @param flows workflows to merge with ; as a delimiter
 * @param hcfile target hostconfigfile of Merge GekkoFS for Registry to generate
 * @param hfile target hostfile of Merge GekkoFS for Registry to generate
 */
int
forward_request_registry(std::string flows, std::string hcfile, std::string hfile, std::string workflow) {

    auto endp = CTX->registry();
    gkfs::rpc::registry_request::output out;
    gkfs::rpc::registry_request::input in(flows, hcfile, hfile, workflow);
   
    try {
        LOG(DEBUG, "Retrieving merge files from registry");
        // TODO(amiranda): add a post() with RPC_TIMEOUT to hermes so that we
        // can retry for RPC_TRIES (see old commits with margo)
        // TODO(amiranda): hermes will eventually provide a post(endpoint)
        // returning one result and a broadcast(endpoint_set) returning a
        // result_set. When that happens we can remove the .at(0) :/

        out = ld_network_service->post<gkfs::rpc::registry_request>(endp,in).get().at(0);
        
        LOG(DEBUG, "Got response success: {}", out.err());

        return out.err() ? out.err() : 0;
    } catch(const std::exception& ex) {
        LOG(ERROR, "while getting rpc output");
        return EBUSY;
    }

}

/**
 * --Multiple GekkoFS--
 * Register current workflow to Registry
 * @param work_flow current workflow name
 * @param hcfile current GekkoFS hostconfigfile
 * @param hfile current GekkoFS hostfile
 */
int
forward_register_registry(std::string work_flow, std::string hcfile, std::string hfile) {

    auto endp = CTX->registry();
    gkfs::rpc::registry_register::input in(work_flow, hcfile, hfile);
   
    try {
        LOG(DEBUG, "Retrieving merge files from registry");
        // TODO(amiranda): add a post() with RPC_TIMEOUT to hermes so that we
        // can retry for RPC_TRIES (see old commits with margo)
        // TODO(amiranda): hermes will eventually provide a post(endpoint)
        // returning one result and a broadcast(endpoint_set) returning a
        // result_set. When that happens we can remove the .at(0) :/

        auto out = ld_network_service->post<gkfs::rpc::registry_register>(endp,in).get().at(0);
        LOG(DEBUG, "Got response success: {}", out.err());

        return out.err() ? out.err() : 0;
    } catch(const std::exception& ex) {
        LOG(ERROR, "while getting rpc output");
        return EBUSY;
    }

}

} // namespace gkfs::rpc
