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
#include <daemon/handler/rpc_defs.hpp>
#include <daemon/handler/rpc_util.hpp>
#include <daemon/backend/metadata/db.hpp>
#include <daemon/backend/data/chunk_storage.hpp>
#include <daemon/ops/metadentry.hpp>
#include <daemon/handler/transport.hpp>

#include <common/path_util.hpp>
#include <common/rpc/rpc_types.hpp>
#include <common/statistics/stats.hpp>
#include <stage/stage.hpp>
#include <iostream>
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
rpc_srv_stage(hg_handle_t handle) {
    rpc_stage_in_t in{};
    rpc_err_out_t out{};
    auto ret = margo_get_input(handle, &in);
    if(ret != HG_SUCCESS)
        GKFS_DATA->spdlogger()->error(
                "{}() Failed to retrieve input from handle", __func__);
    assert(ret == HG_SUCCESS);
    GKFS_DATA->spdlogger()->debug("{}() path: '{}'", __func__, in.in_path);

    try {
        auto ret = forward_transport(in.in_path, in.out_path, in.opts);
        out.err = ret;
    } catch(const std::exception& e) {
        out.err = -1;
    }

    auto hret = margo_respond(handle, &out);
    if(hret != HG_SUCCESS) {
        GKFS_DATA->spdlogger()->error("{}() Failed to respond", __func__);
    }

    // Destroy handle when finished
    margo_free_input(handle, &in);
    margo_destroy(handle);
    return HG_SUCCESS;
}

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
rpc_srv_stage_metadata(hg_handle_t handle) {
    rpc_stage_metadata_in_t in{};
    rpc_stage_metadata_out_t out{};
    auto ret = margo_get_input(handle, &in);
    if(ret != HG_SUCCESS)
        GKFS_DATA->spdlogger()->error(
                "{}() Failed to retrieve input from handle", __func__);
    assert(ret == HG_SUCCESS);
    GKFS_DATA->spdlogger()->debug("{}() path: '{}'", __func__, in.path);
    std::string val;
    try {
        out.err = 0;
        if(in.flag & STAGE_IN){
            if(GKFS_DATA->mdb()->exists(in.path)){
                /* ignore this ?*/
                auto md = gkfs::metadata::get(in.path);
                if(S_ISREG(md.mode()) && (md.size() != 0))
                    GKFS_DATA->storage()->destroy_chunk_space(in.path);
            } 
            std::string dir = gkfs::path::dirname(in.path);
            if(GKFS_DATA->mdb()->exists(dir)){
                gkfs::metadata::Metadata md(in.mode);
                md.size(in.size);
                gkfs::metadata::update(in.path, md);
            } else {
                out.err = ENOENT;
            }
        } else {
            if(GKFS_DATA->mdb()->exists(in.path)){
                val = gkfs::metadata::get_str(in.path);
                out.db_val = val.c_str();
            } else {
                out.err = ENOENT;
            }
        }
        GKFS_DATA->spdlogger()->debug("{}() Sending output mode '{}'", __func__,
                                      out.db_val);
    } catch(const gkfs::metadata::NotFoundException& e) {
        GKFS_DATA->spdlogger()->debug("{}() Entry not found: '{}'", __func__,
                                      in.path);
        out.err = ENOENT;
    } catch(const std::exception& e) {
        GKFS_DATA->spdlogger()->error(
                "{}() Failed to get metadentry from DB: '{}'", __func__,
                e.what());
        out.err = EBUSY;
    }

    auto hret = margo_respond(handle, &out);
    if(hret != HG_SUCCESS) {
        GKFS_DATA->spdlogger()->error("{}() Failed to respond", __func__);
    }

    // Destroy handle when finished
    margo_free_input(handle, &in);
    margo_destroy(handle);
    return HG_SUCCESS;
}

} // namespace

DEFINE_MARGO_RPC_HANDLER(rpc_srv_stage)

DEFINE_MARGO_RPC_HANDLER(rpc_srv_stage_metadata)
