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
bool forward_get_bloom_filter(size_t size) {
    const size_t MAX_MEMORY = 64 * 1024 * 1024;
    size_t filter_size = size;
    size_t buffer_size = size;
    size_t hosts_size = CTX->hosts().size();
    size_t MAX_TOTAL_MEMORY = MAX_MEMORY;

    char* shared_buf = (char*)malloc(MAX_TOTAL_MEMORY);
    if (!shared_buf) {
        LOG(ERROR, "Failed to allocate memory for buffer");
        return false;
    }
    
    std::vector<hermes::mutable_buffer> bufseq{
        hermes::mutable_buffer{shared_buf, MAX_TOTAL_MEMORY}
    };
    
    hermes::exposed_memory exposed_buffer;
    try {
        exposed_buffer = ld_network_service->expose(
            bufseq, hermes::access_mode::write_only);
    } catch (const std::exception& ex) {
        LOG(ERROR, "{}() Failed to expose shared buffer for RMA. err '{}'",
            __func__, ex.what());
        std::cout << ex.what() << std::endl;
        free(shared_buf);
        return false;
    }
    
    size_t max_hosts_per_group = MAX_TOTAL_MEMORY / buffer_size;
    if (max_hosts_per_group == 0) {
        LOG(ERROR, "Filter size too large。");
        free(shared_buf);
        return false;
    }
    
    std::vector<uint64_t> host_ids(hosts_size);
    std::iota(host_ids.begin(), host_ids.end(), 0);
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(host_ids.begin(), host_ids.end(), g);
    
    std::vector<bloom_filter>& filter_vec = CTX->bloom_filter_vec();
    filter_vec.resize(CTX->hosts().size());
    
    int err = 0;
    size_t total_processed = 0;
    
    while (total_processed < hosts_size) {
        size_t current_group_size = std::min(
            max_hosts_per_group, 
            hosts_size - total_processed
        );
        std::vector<hermes::rpc_handle<gkfs::rpc::Bloom_filter>> handles;
        handles.reserve(current_group_size);
        
        bool rpc_post_failed = false;
        for (size_t i = 0; i < current_group_size; ++i) {
            size_t global_idx = total_processed + i;
            uint64_t host_id = host_ids[global_idx];
            
            try {
                auto endp = CTX->hosts().at(host_id);
                LOG(DEBUG, "Sending bloom filter RPC to host: {}", endp.to_string());
                
                size_t offset = i * buffer_size;
                
                gkfs::rpc::Bloom_filter::input in(offset, exposed_buffer);
                handles.emplace_back(
                    ld_network_service->post<gkfs::rpc::Bloom_filter>(endp, in)
                );
            } catch (const std::exception& ex) {
                LOG(ERROR, "Failed to forward RPC to host {}: {}", 
                    host_id, ex.what());
                std::cout << ex.what() << std::endl;
                rpc_post_failed = true;
                err = -1;
                break;
            }
        }
        
        if (rpc_post_failed) {
            break;
        }
        
        for (size_t i = 0; i < current_group_size; ++i) {
            size_t global_idx = total_processed + i;
            uint64_t host_id = host_ids[global_idx];
            
            try {
                auto out = handles[i].get().at(0);
                if (out.err() != 0) {
                    LOG(ERROR, "Host {} returned error: {}", host_id, out.err());
                    err = out.err();
                    continue;
                }
                
                char* raw_buf = shared_buf + (i * buffer_size);
                filter_vec[host_id].deserialize(raw_buf, filter_size);
                
            } catch (const std::exception& ex) {
                LOG(ERROR, "Error receiving bloom filter from host {}: {}", 
                    host_id, ex.what());
                std::cout << ex.what() << std::endl;
                err = EBUSY;
            }
        }
        
        total_processed += current_group_size;
    }

    free(shared_buf);
    
    return err == 0;
}

} // namespace gkfs::rpc
