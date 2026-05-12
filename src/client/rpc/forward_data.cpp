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

#include <client/preload_util.hpp>
#include <client/rpc/forward_data.hpp>
#include <client/rpc/rpc_types.hpp>
#include <client/logging.hpp>

#include <common/rpc/distributor.hpp>
#include <common/arithmetic/arithmetic.hpp>
#include <common/rpc/rpc_util.hpp>

#include <unordered_set>
#include <iostream>
#include <tuple>

using namespace std;

namespace gkfs::rpc {
namespace cfg = gkfs::config::rpc;
using gkfs::utils::arithmetic::last_smaller_equal;

namespace {

struct EpochTarget {
    gkfs::file_layout::epoch_t epoch{};
    uint64_t target{};
};

struct EpochChunkRange {
    gkfs::file_layout::epoch_t epoch{};
    uint64_t chunk_start{};
    uint64_t chunk_end{};
    uint64_t host_size{};
};

bool
operator<(const EpochTarget& lhs, const EpochTarget& rhs) {
    return std::tie(lhs.epoch, lhs.target) < std::tie(rhs.epoch, rhs.target);
}

bool
operator==(const EpochTarget& lhs, const EpochTarget& rhs) {
    return lhs.epoch == rhs.epoch && lhs.target == rhs.target;
}

gkfs::file_layout::FileLayoutSnapshotPtr
load_layout_snapshot_for_path(const std::string& path) {
    auto& layouts = CTX->file_layouts();
    auto record = layouts.find(path);
    if(!record && CTX->use_workflow()) {
        for(const auto& host_config : CTX->hostsconfig()) {
            const auto prefix = "/" + host_config.flowname;
            if(path == prefix) {
                record = layouts.find("/");
                break;
            }
            if(path.rfind(prefix + "/", 0) == 0) {
                record = layouts.find(path.substr(prefix.size()));
                break;
            }
        }
    }

    if(!record) {
        return {};
    }

    return gkfs::file_layout::load_file_layout_snapshot(record);
}

gkfs::file_layout::epoch_t
snapshot_latest_version_epoch(
        const gkfs::file_layout::FileLayoutSnapshotPtr& snapshot) {
    if(!snapshot) {
        return 0;
    }
    return snapshot->latest_version_epoch;
}

std::vector<EpochChunkRange>
split_chunk_range_by_epoch(
        const gkfs::file_layout::FileLayoutSnapshotPtr& snapshot,
        uint64_t chunk_start, uint64_t chunk_end,
        gkfs::file_layout::epoch_t fallback_epoch) {
    std::vector<EpochChunkRange> ranges;
    if(chunk_start > chunk_end) {
        return ranges;
    }

    if(!snapshot || snapshot->empty()) {
        gkfs::utils::ensure_epoch_hosts(fallback_epoch);
        ranges.push_back({fallback_epoch,
                          chunk_start,
                          chunk_end,
                          CTX->epoch_hosts_size(fallback_epoch)});
        return ranges;
    }

    const auto& entries = snapshot->layout.entries;
    auto it = std::upper_bound(
            entries.begin(), entries.end(), chunk_start,
            [](uint64_t id, const gkfs::file_layout::FileLayoutEntry& entry) {
                return id < entry.start_chunk;
            });

    auto entry_idx = static_cast<std::size_t>(std::distance(entries.begin(), it));
    if(entry_idx != 0) {
        --entry_idx;
    }

    auto segment_start = chunk_start;
    while(segment_start <= chunk_end) {
        const auto epoch = entries[entry_idx].epoch;
        uint64_t next_segment_start = chunk_end + 1;
        if(entry_idx + 1 < entries.size()) {
            next_segment_start = entries[entry_idx + 1].start_chunk;
        }

        const auto segment_end =
                std::min(chunk_end, next_segment_start - 1);
        gkfs::utils::ensure_epoch_hosts(epoch);
        ranges.push_back(
                {epoch, segment_start, segment_end, CTX->epoch_hosts_size(epoch)});

        if(segment_end == chunk_end) {
            break;
        }

        segment_start = segment_end + 1;
        ++entry_idx;
    }

    return ranges;
}

EpochTarget
locate_epoch_target(const std::string& path, uint64_t chunk_id, int copy,
                    gkfs::file_layout::epoch_t epoch, uint64_t host_size) {
    const auto& distributor = *CTX->distributor();
    const auto target = distributor.locate_data(
            path, static_cast<gkfs::rpc::chunkid_t>(chunk_id), copy,
            static_cast<int>(host_size));
    return {epoch, target};
}

uint64_t
global_host_id(const EpochTarget& target) {
    gkfs::utils::ensure_epoch_hosts(target.epoch);
    return CTX->epoch_host(target.epoch, target.target);
}

} // namespace

/*
 * This file includes all data RPC calls.
 * NOTE: No errno is defined here!
 */

/**
 * Send an RPC request to write from a buffer.
 * There is a bitset of 1024 chunks to tell the server
 * which chunks to process. Exceeding this value will work without
 * replication. Another way is to leverage mercury segments.
 * TODO: Decide how to manage a write to a replica that doesn't exist
 * @param path
 * @param buf
 * @param append_flag
 * @param write_size
 * @param num_copies number of replicas
 * @return pair<error code, written size>
 */
pair<int, ssize_t>
forward_write(const string& path, const void* buf, const off64_t offset,
              const size_t write_size, const int8_t num_copies,
              std::uint64_t latest_version_epoch) {
    // import pow2-optimized arithmetic functions
    using namespace gkfs::utils::arithmetic;

    assert(write_size > 0);
    const auto file_layout_snapshot = load_layout_snapshot_for_path(path);
    const auto file_latest_version_epoch =
            latest_version_epoch
                    ? latest_version_epoch
                    : snapshot_latest_version_epoch(file_layout_snapshot);

    // Calculate chunkid boundaries and numbers so that daemons know in
    // which interval to look for chunks
    auto chnk_start = block_index(offset, gkfs::config::rpc::chunksize);
    auto chnk_end = block_index((offset + write_size) - 1,
                                gkfs::config::rpc::chunksize);

    auto chnk_total = (chnk_end - chnk_start) + 1;
    const auto epoch_ranges = split_chunk_range_by_epoch(
            file_layout_snapshot, chnk_start, chnk_end,
            file_latest_version_epoch);
    /* --PFL implementation-- */
    //component number of first chunk
    auto cpn = last_smaller_equal(cfg::PFLchunkID, chnk_start);
    //help to calculate total chunk size of target
    std::map<EpochTarget, uint64_t> target_total_size{};
    /* --PFL implementation-- */
    // Collect all chunk ids within count that have the same destination so
    // that those are send in one rpc bulk transfer
    std::map<EpochTarget, std::vector<uint64_t>> target_chnks{};
    
    // contains the target ids, used to access the target_chnks map.
    // First idx is chunk with potential offset
    std::vector<EpochTarget> targets{};

    // targets for the first and last chunk as they need special treatment
    // We need a set to manage replicas.
    std::set<EpochTarget> chnk_start_target{};
    std::set<EpochTarget> chnk_end_target{};

    std::map<EpochTarget, std::vector<uint8_t>> write_ops_vect;

    // If num_copies is 0, we do the normal write operation. Otherwise
    // we process all the replicas.
    for(const auto& range : epoch_ranges) {
        for(uint64_t chnk_id = range.chunk_start; chnk_id <= range.chunk_end;
            ++chnk_id) {
            // /* --PFL implementation-- */
            //track component number of current chunk
            if(cpn + 1 < cfg::PFLcomponents &&
               chnk_id >= cfg::PFLchunkID[cpn + 1]) {
                cpn++;
            }
            for(auto copy = num_copies ? 1 : 0; copy < num_copies + 1; copy++) {
                auto target = locate_epoch_target(path, chnk_id, copy,
                                                  range.epoch, range.host_size);

                if(write_ops_vect.find(target) == write_ops_vect.end())
                    write_ops_vect[target] =
                            std::vector<uint8_t>(((chnk_total + 7) / 8));
                gkfs::rpc::set_bitset(write_ops_vect[target],
                                      chnk_id - chnk_start);

                if(target_chnks.count(target) == 0) {
                    target_chnks.insert(std::make_pair(
                            target, std::vector<uint64_t>{chnk_id}));
                    targets.push_back(target);
                    // /* --PFL implementation-- */
                    target_total_size.insert(
                            std::make_pair(target, cfg::PFLsize[cpn]));
                } else {
                    target_chnks[target].push_back(chnk_id);
                    // /* --PFL implementation-- */
                    target_total_size[target] += cfg::PFLsize[cpn];
                }

                // set first and last chnk targets
                if(chnk_id == chnk_start) {
                    chnk_start_target.insert(target);
                }

                if(chnk_id == chnk_end) {
                    chnk_end_target.insert(target);
                }
            }
        }
    }

    // some helper variables for async RPC
    std::vector<hermes::mutable_buffer> bufseq{
            hermes::mutable_buffer{const_cast<void*>(buf), write_size},
    };

    // expose user buffers so that they can serve as RDMA data sources
    // (these are automatically "unexposed" when the destructor is called)
    hermes::exposed_memory local_buffers;

    try {
        local_buffers = ld_network_service->expose(
                bufseq, hermes::access_mode::read_only);

    } catch(const std::exception& ex) {
        LOG(ERROR, "Failed to expose buffers for RMA");
        return make_pair(EBUSY, 0);
    }

    std::vector<hermes::rpc_handle<gkfs::rpc::write_data>> handles;

    // Issue non-blocking RPC requests and wait for the result later
    //
    // TODO(amiranda): This could be simplified by adding a vector of inputs
    // to async_engine::broadcast(). This would allow us to avoid manually
    // looping over handles as we do below
    for(const auto& target : targets) {

        // total chunk_size for target
        auto total_chunk_size =
                target_chnks[target].size() * gkfs::config::rpc::chunksize;
        // /* --PFL implementation-- */
        if(cfg::use_PFL) 
            total_chunk_size = target_total_size[target];

        // receiver of first chunk must subtract the offset from first chunk
        if(chnk_start_target.end() != chnk_start_target.find(target)) {
            total_chunk_size -=
                    block_overrun(offset, gkfs::config::rpc::chunksize);
        }

        // receiver of last chunk must subtract
        if(chnk_end_target.end() != chnk_end_target.find(target) ) { //&&!is_aligned(offset + write_size, gkfs::config::rpc::chunksize)
            total_chunk_size -= block_underrun(offset + write_size,
                                               gkfs::config::rpc::chunksize);
        }

        const auto target_host_id = global_host_id(target);
        auto endp = CTX->host_endpoint(target_host_id);
        const auto epoch_host_size = CTX->epoch_hosts_size(target.epoch);
        try {
            LOG(DEBUG, "Sending RPC ...");

            gkfs::rpc::write_data::input in(
                    path,
                    // first offset in targets is the chunk with
                    // a potential offset
                    block_overrun(offset, gkfs::config::rpc::chunksize),
                    /* --Multiple GekkoFS--*/
                    target.target,
                    epoch_host_size,
                    /* --Multiple GekkoFS--*/
                    // number of chunks handled by that destination
                    gkfs::rpc::compress_bitset(write_ops_vect[target]),
                    target_chnks[target].size(),
                    // chunk start id of this write
                    chnk_start,
                    // chunk end id of this write
                    chnk_end,
                    // total size to write
                    total_chunk_size, local_buffers, file_latest_version_epoch,
                    CTX->unique_id());

            // TODO(amiranda): add a post() with RPC_TIMEOUT to hermes so that
            // we can retry for RPC_TRIES (see old commits with margo)
            // TODO(amiranda): hermes will eventually provide a post(endpoint)
            // returning one result and a broadcast(endpoint_set) returning a
            // result_set. When that happens we can remove the .at(0) :/
            handles.emplace_back(
                    ld_network_service->post<gkfs::rpc::write_data>(endp, in));

            LOG(DEBUG,
                "host: {} epoch: {} epoch_target: {}, path: \"{}\", chunk_start: {}, chunk_end: {}, chunks: {}, size: {}, offset: {}",
                target_host_id, target.epoch, target.target, path, chnk_start,
                chnk_end, in.chunk_n(), total_chunk_size, in.offset());
        } catch(const std::exception& ex) {
            LOG(ERROR,
                "Unable to send non-blocking rpc for "
                "path \"{}\" [epoch: {}, target: {}, peer: {}]",
                path, target.epoch, target.target, target_host_id);
            if(num_copies == 0)
                return make_pair(EBUSY, 0);
        }
    }

    auto err = 0;
    ssize_t out_size = 0;
    std::size_t idx = 0;
#ifdef REPLICA_CHECK
    std::vector<uint8_t> fill(chnk_total);
    auto write_ops = write_ops_vect.begin();
#endif
    for(const auto& h : handles) {
        try {
            // XXX We might need a timeout here to not wait forever for an
            // output that never comes?
            auto out = h.get().at(0);

            if(out.err() != 0) {
                LOG(ERROR, "Daemon reported error: {}", out.err());
                err = out.err();
            } else {
                out_size += static_cast<size_t>(out.io_size());
#ifdef REPLICA_CHECK
                if(num_copies) {
                    if(fill.size() == 0) {
                        fill = write_ops->second;
                    } else {
                        for(size_t i = 0; i < fill.size(); i++) {
                            fill[i] |= write_ops->second[i];
                        }
                    }
                }
                write_ops++;
#endif
            }
        } catch(const std::exception& ex) {
            LOG(ERROR,
                "Failed to get rpc output for path \"{}\" [epoch: {}, target: {}]",
                path, targets[idx].epoch, targets[idx].target);
            std::cout<< "Forward_write Error:" << ex.what() << std::endl;
            err = EIO;
        }
        idx++;
    }
    // As servers can fail (and we cannot know if the total data is written), we
    // send the updated size but check that at least one copy of all chunks are
    // processed.
    if(num_copies) {
        // A bit-wise or should show that all the chunks are written (255)
        out_size = write_size;
#ifdef REPLICA_CHECK
        for(size_t i = 0; i < fill.size() - 1; i++) {
            if(fill[i] != 255) {
                err = EIO;
                break;
            }
        }
        // Process the leftover bytes
        for(uint64_t chnk_id = (chnk_start + (fill.size() - 1) * 8);
            chnk_id <= chnk_end; chnk_id++) {
            if(!(fill[(chnk_id - chnk_start) / 8] &
                 (1 << ((chnk_id - chnk_start) % 8)))) {
                err = EIO;
                break;
            }
        }
#endif
    }
    /*
     * Typically file systems return the size even if only a part of it was
     * written. In our case, we do not keep track which daemon fully wrote its
     * workload. Thus, we always return size 0 on error.
     */
    if(err)
        return make_pair(err, 0);
    else
        return make_pair(0, out_size);
}

/**
 * Send an RPC request to read to a buffer.
 * @param path
 * @param buf
 * @param offset
 * @param read_size
 * @param num_copies number of copies available (0 is no replication)
 * @param failed nodes failed that should not be used
 * @return pair<error code, read size>
 */
pair<int, ssize_t>
forward_read(const string& path, void* buf, const off64_t offset,
             const size_t read_size, const int8_t num_copies,
             std::set<uint64_t>& failed, std::uint64_t latest_version_epoch) {
    // import pow2-optimized arithmetic functions
    using namespace gkfs::utils::arithmetic;
    const auto file_layout_snapshot = load_layout_snapshot_for_path(path);
    const auto file_latest_version_epoch =
            latest_version_epoch
                    ? latest_version_epoch
                    : snapshot_latest_version_epoch(file_layout_snapshot);

    // Calculate chunkid boundaries and numbers so that daemons know in which
    // interval to look for chunks
    auto chnk_start = block_index(offset, gkfs::config::rpc::chunksize);
    auto chnk_end =
            block_index((offset + read_size - 1), gkfs::config::rpc::chunksize);
    auto chnk_total = (chnk_end - chnk_start) + 1;
    const auto epoch_ranges = split_chunk_range_by_epoch(
            file_layout_snapshot, chnk_start, chnk_end,
            file_latest_version_epoch);
    /* --PFL implementation-- */
    //component number of first chunk
    auto cpn = last_smaller_equal(cfg::PFLchunkID, chnk_start);
    //help to calculate total chunk size of target
    std::map<EpochTarget, uint64_t> target_total_size{};
    /* --PFL implementation-- */
    // Collect all chunk ids within count that have the same destination so
    // that those are send in one rpc bulk transfer
    std::map<EpochTarget, std::vector<uint64_t>> target_chnks{};

    // contains the recipient ids, used to access the target_chnks map.
    // First idx is chunk with potential offset
    std::vector<EpochTarget> targets{};
    // targets for the first and last chunk as they need special treatment
    EpochTarget chnk_start_target{};
    EpochTarget chnk_end_target{};
    std::map<EpochTarget, std::vector<uint8_t>> read_bitset_vect;

    for(const auto& range : epoch_ranges) {
        for(uint64_t chnk_id = range.chunk_start; chnk_id <= range.chunk_end;
            ++chnk_id) {
            // /* --PFL implementation-- */
            //track component number of current chunk
            if(cpn + 1 < cfg::PFLcomponents &&
               chnk_id >= cfg::PFLchunkID[cpn + 1]) {
                cpn++;
            }
            auto target = locate_epoch_target(path, chnk_id, 0, range.epoch,
                                              range.host_size);
            if(num_copies > 0) {
                // If we have some failures we select another copy (randomly).
                while(failed.find(global_host_id(target)) != failed.end()) {
                    LOG(DEBUG, "Selecting another node, target: {} down",
                        target.target);
                    target = locate_epoch_target(path, chnk_id,
                                                 rand() % num_copies,
                                                 range.epoch, range.host_size);
                }
            }

            if(read_bitset_vect.find(target) == read_bitset_vect.end())
                read_bitset_vect[target] =
                        std::vector<uint8_t>(((chnk_total + 7) / 8));
            read_bitset_vect[target][(chnk_id - chnk_start) / 8] |=
                    1 << ((chnk_id - chnk_start) % 8); // set

            if(target_chnks.count(target) == 0) {
                target_chnks.insert(std::make_pair(
                        target, std::vector<uint64_t>{chnk_id}));
                targets.push_back(target);
                // /* --PFL implementation-- */
                target_total_size.insert(
                            std::make_pair(target, cfg::PFLsize[cpn]));
            } else {
                target_chnks[target].push_back(chnk_id);
                // /* --PFL implementation-- */
                target_total_size[target] += cfg::PFLsize[cpn];
            }

            // set first and last chnk targets
            if(chnk_id == chnk_start) {
                chnk_start_target = target;
            }

            if(chnk_id == chnk_end) {
                chnk_end_target = target;
            }
        }
    }

    // some helper variables for async RPCs
    std::vector<hermes::mutable_buffer> bufseq{
            hermes::mutable_buffer{buf, read_size},
    };

    // expose user buffers so that they can serve as RDMA data targets
    // (these are automatically "unexposed" when the destructor is called)
    hermes::exposed_memory local_buffers;

    try {
        local_buffers = ld_network_service->expose(
                bufseq, hermes::access_mode::write_only);

    } catch(const std::exception& ex) {
        LOG(ERROR, "Failed to expose buffers for RMA");
        return make_pair(EBUSY, 0);
    }

    std::vector<hermes::rpc_handle<gkfs::rpc::read_data>> handles;

    // Issue non-blocking RPC requests and wait for the result later
    //
    // TODO(amiranda): This could be simplified by adding a vector of inputs
    // to async_engine::broadcast(). This would allow us to avoid manually
    // looping over handles as we do below

    for(const auto& target : targets) {

        // total chunk_size for target
        auto total_chunk_size =
                target_chnks[target].size() * gkfs::config::rpc::chunksize;
        // /* --PFL implementation-- */
        if(cfg::use_PFL) 
            total_chunk_size = target_total_size[target];        
        // receiver of first chunk must subtract the offset from first chunk
        if(target == chnk_start_target) {
            total_chunk_size -=
                    block_overrun(offset, gkfs::config::rpc::chunksize);
        }

        // receiver of last chunk must subtract
        if(target == chnk_end_target ) { //&& !is_aligned(offset + read_size, gkfs::config::rpc::chunksize)
            total_chunk_size -= block_underrun(offset + read_size,
                                               gkfs::config::rpc::chunksize);
        }

        const auto target_host_id = global_host_id(target);
        auto endp = CTX->host_endpoint(target_host_id);
        const auto epoch_host_size = CTX->epoch_hosts_size(target.epoch);
        try {

            LOG(DEBUG, "Sending RPC ...");

            gkfs::rpc::read_data::input in(
                    path,
                    // first offset in targets is the chunk with
                    // a potential offset
                    block_overrun(offset, gkfs::config::rpc::chunksize), 
                    /* --Multiple GekkoFS--*/
                    target.target,
                    epoch_host_size,
                    /* --Multiple GekkoFS--*/
                    gkfs::rpc::compress_bitset(read_bitset_vect[target]),
                    // number of chunks handled by that destination
                    target_chnks[target].size(),
                    // chunk start id of this write
                    chnk_start,
                    // chunk end id of this write
                    chnk_end,
                    // total size to write
                    total_chunk_size, local_buffers, file_latest_version_epoch,
                    CTX->unique_id());

            // TODO(amiranda): add a post() with RPC_TIMEOUT to hermes so
            // that we can retry for RPC_TRIES (see old commits with margo)
            // TODO(amiranda): hermes will eventually provide a
            // post(endpoint) returning one result and a
            // broadcast(endpoint_set) returning a result_set. When that
            // happens we can remove the .at(0) :/
            handles.emplace_back(
                    ld_network_service->post<gkfs::rpc::read_data>(endp, in));

            LOG(DEBUG,
                "host: {} epoch: {} epoch_target: {}, path: {}, chunk_start: {}, chunk_end: {}, chunks: {}, size: {}, offset: {}",
                target_host_id, target.epoch, target.target, path, chnk_start,
                chnk_end, in.chunk_n(), total_chunk_size, in.offset());

            LOG(TRACE_READS,
                "read {} host: {}, epoch: {}, epoch_target: {}, path: {}, chunk_start: {}, chunk_end: {}",
                CTX->get_hostname(), target_host_id, target.epoch,
                target.target, path, chnk_start, chnk_end);


        } catch(const std::exception& ex) {
            LOG(ERROR,
                "Unable to send non-blocking rpc for path \"{}\" "
                "[epoch: {}, target: {}, peer: {}]",
                path, target.epoch, target.target, target_host_id);
            return make_pair(EBUSY, 0);
        }
    }

    // Wait for RPC responses and then get response and add it to out_size
    // which is the read size. All potential outputs are served to free
    // resources regardless of errors, although an errorcode is set.
    auto err = 0;
    ssize_t out_size = 0;
    std::size_t idx = 0;

    for(const auto& h : handles) {
        try {
            // XXX We might need a timeout here to not wait forever for an
            // output that never comes?
            auto out = h.get().at(0);

            if(out.err() != 0) {
                LOG(ERROR, "Daemon reported error: {}", out.err());
                err = out.err();
            }

            out_size += static_cast<size_t>(out.io_size());

        } catch(const std::exception& ex) {
            LOG(ERROR,
                "Failed to get rpc output for path \"{}\" [epoch: {}, target: {}]",
                path, targets[idx].epoch, targets[idx].target);
            std::cout<< "Forward_read Error:" << ex.what() << std::endl;
            err = EIO;
            // We should get targets[idx] and remove from the list of peers
            failed.insert(global_host_id(targets[idx]));
            // Then repeat the read with another peer (We repear the full
            // read, this can be optimised but it is a cornercase)
        }
        idx++;
    }


    /*
     * Typically file systems return the size even if only a part of it was
     * read. In our case, we do not keep track which daemon fully read its
     * workload. Thus, we always return size 0 on error.
     */
    if(err)
        return make_pair(err, 0);
    else
        return make_pair(0, out_size);
}

/**
 * Send an RPC request to truncate a file to given new size
 * @param path
 * @param current_size
 * @param new_size
 * @param num_copies Number of replicas
 * @return error code
 */
int
forward_truncate(const std::string& path, size_t current_size, size_t new_size,
                 const int8_t num_copies) {
    // import pow2-optimized arithmetic functions
    using namespace gkfs::utils::arithmetic;

    assert(current_size > new_size);

    // Find out which data servers need to delete data chunks in order to
    // contact only them
    const unsigned int chunk_start =
            block_index(new_size, gkfs::config::rpc::chunksize);
    const unsigned int chunk_end = block_index(current_size - new_size - 1,
                                               gkfs::config::rpc::chunksize);

    const auto file_layout_snapshot = load_layout_snapshot_for_path(path);
    const auto file_latest_version_epoch =
            snapshot_latest_version_epoch(file_layout_snapshot);
    const auto epoch_ranges = split_chunk_range_by_epoch(
            file_layout_snapshot, chunk_start, chunk_end,
            file_latest_version_epoch);
    std::unordered_set<uint64_t> hosts;
    for(const auto& range : epoch_ranges) {
        for(uint64_t chunk_id = range.chunk_start; chunk_id <= range.chunk_end;
            ++chunk_id) {
            for(auto copy = 0; copy < (num_copies + 1); ++copy) {
                const auto target = locate_epoch_target(
                        path, chunk_id, copy, range.epoch, range.host_size);
                hosts.insert(global_host_id(target));
            }
        }
    }

    std::vector<hermes::rpc_handle<gkfs::rpc::trunc_data>> handles;

    auto err = 0;

    for(const auto& host : hosts) {
        auto endp = CTX->host_endpoint(host);

        try {
            LOG(DEBUG, "Sending RPC ...");

            gkfs::rpc::trunc_data::input in(path, new_size,
                                            CTX->unique_id());

            // TODO(amiranda): add a post() with RPC_TIMEOUT to hermes so
            // that we can retry for RPC_TRIES (see old commits with margo)
            // TODO(amiranda): hermes will eventually provide a
            // post(endpoint) returning one result and a
            // broadcast(endpoint_set) returning a result_set. When that
            // happens we can remove the .at(0) :/
            handles.emplace_back(
                    ld_network_service->post<gkfs::rpc::trunc_data>(endp, in));

        } catch(const std::exception& ex) {
            // TODO(amiranda): we should cancel all previously posted
            // requests here, unfortunately, Hermes does not support it yet
            // :/
            LOG(ERROR, "Failed to send request to host: {}", host);
            err = EIO;
            break; // We need to gather all responses so we can't return
                   // here
        }
    }
    // Wait for RPC responses and then get response
    for(const auto& h : handles) {
        try {
            // XXX We might need a timeout here to not wait forever for an
            // output that never comes?
            auto out = h.get().at(0);

            if(out.err()) {
                LOG(ERROR, "received error response: {}", out.err());
                err = EIO;
            }
        } catch(const std::exception& ex) {
            LOG(ERROR, "while getting rpc output");
            err = EIO;
        }
    }
    return err ? err : 0;
}

/**
 * Send an RPC request to chunk stat all hosts
 * @return pair<error code, rpc::ChunkStat>
 */
pair<int, ChunkStat>
forward_get_chunk_stat() {

    std::vector<hermes::rpc_handle<gkfs::rpc::chunk_stat>> handles;

    auto err = 0;

    for(const auto& endp : CTX->hosts()) {
        try {
            LOG(DEBUG, "Sending RPC to host: {}", endp.to_string());

            gkfs::rpc::chunk_stat::input in(0);

            // TODO(amiranda): add a post() with RPC_TIMEOUT to hermes so
            // that we can retry for RPC_TRIES (see old commits with margo)
            // TODO(amiranda): hermes will eventually provide a
            // post(endpoint) returning one result and a
            // broadcast(endpoint_set) returning a result_set. When that
            // happens we can remove the .at(0) :/
            handles.emplace_back(
                    ld_network_service->post<gkfs::rpc::chunk_stat>(endp, in));

        } catch(const std::exception& ex) {
            // TODO(amiranda): we should cancel all previously posted
            // requests here, unfortunately, Hermes does not support it yet
            // :/
            LOG(ERROR, "Failed to send request to host: {}", endp.to_string());
            err = EBUSY;
            break; // We need to gather all responses so we can't return
                   // here
        }
    }

    unsigned long chunk_size = gkfs::config::rpc::chunksize;
    unsigned long chunk_total = 0;
    unsigned long chunk_free = 0;

    // wait for RPC responses
    for(std::size_t i = 0; i < handles.size(); ++i) {

        gkfs::rpc::chunk_stat::output out{};

        try {
            // XXX We might need a timeout here to not wait forever for an
            // output that never comes?
            out = handles[i].get().at(0);

            if(out.err()) {
                err = out.err();
                LOG(ERROR,
                    "Host '{}' reported err code '{}' during stat chunk.",
                    CTX->hosts().at(i).to_string(), err);
                // we don't break here to ensure all responses are processed
                continue;
            }
            assert(out.chunk_size() == chunk_size);
            chunk_total += out.chunk_total();
            chunk_free += out.chunk_free();
        } catch(const std::exception& ex) {
            LOG(ERROR, "Failed to get RPC output from host: {}", i);
            // Avoid setting err if a server fails.
            // err = EBUSY;
        }
    }

    if(err)
        return make_pair(err, ChunkStat{});
    else
        return make_pair(0, ChunkStat{chunk_size, chunk_total, chunk_free});
}

} // namespace gkfs::rpc
