/*
  Copyright 2018-2024, Barcelona Supercomputing Center (BSC), Spain
  Copyright 2015-2024, Johannes Gutenberg Universitaet Mainz, Germany

  This software was partially supported by the
  EC H2020 funded project NEXTGenIO (Project ID: 671951, www.nextgenio.eu).

  This software was partially supported by the
  ADA-FS project under the SPPEXA project funded by the DFG.

  This file is part of GekkoFS.

  GekkoFS is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  GekkoFS is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with GekkoFS.  If not, see <https://www.gnu.org/licenses/>.

  SPDX-License-Identifier: GPL-3.0-or-later
*/
/**
 * @brief Provides all Margo RPC handler definitions called by Mercury on client
 * request for all file system management operations.
 * @internal
 * The end of the file defines the associates the Margo RPC handler functions
 * and associates them with their corresponding GekkoFS handler functions.
 * @endinternal
 */
#include <daemon/daemon.hpp>
#include <daemon/handler/rpc_defs.hpp>
#include <daemon/handler/rpc_util.hpp>
#include <daemon/malleability_logger.hpp>

#include <common/common_defs.hpp>
#include <common/rpc/rpc_types.hpp>
#include <cstdint>
#include <common/rpc/rpc_util.hpp>
#include <iostream>
#include <fstream>
extern "C" {
#include <unistd.h>

}

using namespace std;

namespace {

/**
 * @brief Responds with general file system meta information requested on client
 * startup.
 * @internal
 * Most notably this is where the client gets the information on which path
 * GekkoFS is accessible.
 * @endinteral
 * @param handle Mercury RPC handle
 * @return Mercury error code to Mercury
 */
hg_return_t
rpc_srv_get_fs_config(hg_handle_t handle) {
    rpc_config_out_t out{};

    GKFS_DATA->spdlogger()->debug("{}() Got config RPC", __func__);

    // get fs config
    out.mountdir = GKFS_DATA->mountdir().c_str();
    out.rootdir = GKFS_DATA->rootdir().c_str();
    out.atime_state = static_cast<hg_bool_t>(GKFS_DATA->atime_state());
    out.mtime_state = static_cast<hg_bool_t>(GKFS_DATA->mtime_state());
    out.ctime_state = static_cast<hg_bool_t>(GKFS_DATA->ctime_state());
    out.link_cnt_state = static_cast<hg_bool_t>(GKFS_DATA->link_cnt_state());
    out.blocks_state = static_cast<hg_bool_t>(GKFS_DATA->blocks_state());
    out.uid = getuid();
    out.gid = getgid();
    GKFS_DATA->spdlogger()->debug("{}() Sending output configs back to library",
                                  __func__);
    auto hret = margo_respond(handle, &out);
    if(hret != HG_SUCCESS) {
        GKFS_DATA->spdlogger()->error(
                "{}() Failed to respond to client to serve file system configurations",
                __func__);
    }

    // Destroy handle when finished
    margo_destroy(handle);
    return HG_SUCCESS;
}

hg_return_t 
rpc_srv_get_bloom_filter(hg_handle_t handle) {
    rpc_bloom_filter_in_t in{};
    rpc_bloom_filter_out_t out{};
    hg_bulk_t bulk_handle = nullptr;
    out.err = 0;

    // 获取输入参数
    auto ret = margo_get_input(handle, &in);
    if (ret != HG_SUCCESS) {
        GKFS_DATA->spdlogger()->error(
            "{}() Could not get RPC input: {}", __func__, ret);
        out.err = EIO;
        return gkfs::rpc::cleanup_respond(&handle, &in, &out, &bulk_handle);
    }

    if constexpr(!gkfs::config::use_bloom) {
        out.err = ENOTSUP;
        return gkfs::rpc::cleanup_respond(&handle, &in, &out, &bulk_handle);
    }

    const auto& bloom_filter_str = GKFS_DATA->Bloom_filter().serialize();
    size_t filter_size = bloom_filter_str.size();
    void* filter_data = const_cast<char*>(bloom_filter_str.c_str());
    auto hgi = margo_get_info(handle);
    auto mid = margo_hg_info_get_instance(hgi);

    try {
        ret = margo_bulk_create(mid, 1, &filter_data, &filter_size, 
                               HG_BULK_READ_ONLY, &bulk_handle);
        if (ret != HG_SUCCESS) {
            GKFS_DATA->spdlogger()->error(
                    "{}() Failed to create bulk handle: {}", __func__, ret);
                out.err = EIO;
                return gkfs::rpc::cleanup_respond(&handle, &in, &out, &bulk_handle);
        }
        ret = margo_bulk_transfer(mid, HG_BULK_PUSH, hgi->addr, 
                                 in.bulk_handle, in.offset, bulk_handle, 0, filter_size);
        if (ret != HG_SUCCESS) {
            GKFS_DATA->spdlogger()->error(
                    "{}() Failed to transfer bulk: {}", __func__, ret);
                out.err = EIO;
                return gkfs::rpc::cleanup_respond(&handle, &in, &out, &bulk_handle);
        }

        GKFS_DATA->spdlogger()->debug(
            "{}() Sent bloom filter (size: {}) via bulk transfer",
            __func__, filter_size);

    } catch (const std::exception& ex) {
        GKFS_DATA->spdlogger()->error(
            "{}() Error during bulk transfer: {}", __func__, ex.what());
        out.err = EIO;
    }

    auto handler_ret = 
            gkfs::rpc::cleanup_respond(&handle, &in, &out, &bulk_handle);

    return handler_ret;
} 

hg_return_t
rpc_srv_daemon_update_epoch(hg_handle_t handle) {
    rpc_daemon_update_epoch_in_t in{};
    rpc_err_out_t out{};

    auto ret = margo_get_input(handle, &in);
    if(ret != HG_SUCCESS) {
        GKFS_DATA->spdlogger()->error(
                "{}() Failed to retrieve daemon update epoch input", __func__);
        out.err = EIO;
        auto hret = margo_respond(handle, &out);
        if(hret != HG_SUCCESS) {
            GKFS_DATA->spdlogger()->error(
                    "{}() Failed to respond to invalid daemon update epoch RPC",
                    __func__);
        }
        margo_destroy(handle);
        return HG_SUCCESS;
    }

    const auto action_raw = static_cast<std::int32_t>(in.action);
    const auto epoch = static_cast<std::uint64_t>(in.epoch);
    const std::string hostfile = in.hostfile != nullptr ? in.hostfile : "";
    const std::string unique_id =
            in.unique_id != nullptr ? in.unique_id : "";

    if(!gkfs::rpc::is_valid_daemon_resize_action(action_raw) ||
       hostfile.empty() || unique_id.empty()) {
        GKFS_DATA->spdlogger()->error(
                "{}() Invalid daemon control request: action='{}' epoch='{}' hostfile='{}' unique_id='{}'",
                __func__, action_raw, epoch, hostfile, unique_id);
        out.err = EINVAL;
    } else {
        const auto action =
                static_cast<gkfs::rpc::daemon_resize_action>(action_raw);
        const auto previous_epoch = GKFS_DATA->epoch();
        GKFS_DATA->resize_action(action);
        GKFS_DATA->resize_hostfile(hostfile);
        GKFS_DATA->resize_unique_id(unique_id);
        if(action == gkfs::rpc::daemon_resize_action::expand ||
           action == gkfs::rpc::daemon_resize_action::shrink) {
            GKFS_DATA->epoch(epoch);
            gkfs::daemon::malleability::log_epoch_update(
                    gkfs::rpc::to_string(action), previous_epoch,
                    GKFS_DATA->epoch(), hostfile, unique_id, 0);
        }
        GKFS_DATA->spdlogger()->info(
                "{}() Applied daemon resize update action='{}' epoch='{}' current_epoch='{}' hostfile='{}' unique_id='{}'",
                __func__, gkfs::rpc::to_string(action), epoch,
                GKFS_DATA->epoch(), hostfile, unique_id);
        out.err = 0;
    }

    auto hret = margo_respond(handle, &out);
    if(hret != HG_SUCCESS) {
        GKFS_DATA->spdlogger()->error(
                "{}() Failed to respond to daemon update epoch RPC", __func__);
    }

    margo_free_input(handle, &in);
    margo_destroy(handle);
    return HG_SUCCESS;
}

} // namespace

DEFINE_MARGO_RPC_HANDLER(rpc_srv_get_fs_config)
DEFINE_MARGO_RPC_HANDLER(rpc_srv_get_bloom_filter)
DEFINE_MARGO_RPC_HANDLER(rpc_srv_daemon_update_epoch)
