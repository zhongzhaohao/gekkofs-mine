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

#ifndef GEKKOFS_PRELOAD_CTX_HPP
#define GEKKOFS_PRELOAD_CTX_HPP

#include <common/bloom_filter.hpp>
#include <common/file_layout.hpp>
#include <common/thread_pool.hpp>
#include <common/fs_info.hpp>
#include <hermes.hpp>
#include <map>
#include <mercury.h>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include <string>
#include <string_view>
#include <config.hpp>

#include <bitset>

/* Forward declarations */
namespace gkfs {
namespace filemap {
class OpenFileMap;
}
/* --FGAP-- */
namespace filetagmap {
class FileTagMap;
}/* --FGAP-- */
namespace rpc {
class Distributor;
}
namespace log {
struct logger;
}

namespace preload {
/*
 * Client file system config
 */
struct FsConfig {
    // configurable metadata
    bool atime_state;
    bool mtime_state;
    bool ctime_state;
    bool link_cnt_state;
    bool blocks_state;

    uid_t uid;
    gid_t gid;

    std::string rootdir;
};
/* --FGAP-- */
enum class RelativizeStatus { internal, external, fd_unknown, fd_not_a_dir, fgap_trans };

/**
 * Singleton class of the client context with all relevant global data
 */
class PreloadContext {

    static auto constexpr MIN_INTERNAL_FD =
            GKFS_MAX_OPEN_FDS - GKFS_MAX_INTERNAL_FDS;
    static auto constexpr MAX_USER_FDS = MIN_INTERNAL_FD;

private:
    PreloadContext();

    /* --FGAP-- */
    std::shared_ptr<gkfs::filetagmap::FileTagMap> fileTagMap_;

    std::shared_ptr<gkfs::filemap::OpenFileMap> ofm_;
    std::shared_ptr<gkfs::rpc::Distributor> distributor_;
    std::shared_ptr<FsConfig> fs_conf_;

    std::string cwd_;
    std::vector<std::string> mountdir_components_;
    std::string mountdir_;

    /* --Multiple GekkoFS-- */
    hermes::endpoint registry_; // Registry endp
    bool use_workflow_; // Use or not
    std::vector<fs_info> hostsconfig_; // Host(Daemon) config of Each GekkoFS
    std::map<std::string, unsigned int> pathfs_; // Cache of GekkoFS id where path exists
    std::map<std::string, unsigned int> wrapper_pathfs_; // Cache of GekkoFS id where wrapper path exists
    gkfs::file_layout::FileLayoutMap file_layouts_;
    gkfs::file_layout::epoch_t file_layout_global_epoch_ = 0;
    uint64_t local_fs_id_; // Id of GekkoFS having local host(daemon)
    std::vector<bloom_filter> bloom_filter_vec_;  //bloom filter
    ThreadPool thread_pool_; // thread pool
    std::string workflow_;
    std::string unique_id_;
    std::string mergeflows_;
    /* --Multiple GekkoFS-- */

    std::vector<hermes::endpoint> hosts_;
    std::vector<std::string> hosts_name_;
    std::map<std::string, uint64_t> host_uri_to_index_;
    std::map<gkfs::file_layout::epoch_t, std::vector<uint64_t>> epoch_hosts_;
    mutable std::mutex hosts_mutex_;
    uint64_t local_host_id_;
    uint64_t fwd_host_id_;
    std::string rpc_protocol_;
    bool auto_sm_{false};

    bool interception_enabled_;

    std::bitset<GKFS_MAX_INTERNAL_FDS> internal_fds_;
    mutable std::mutex internal_fds_mutex_;
    bool internal_fds_must_relocate_;
    std::bitset<MAX_USER_FDS> protected_fds_;
    std::string hostname;
    int replicas_;

public:
    static PreloadContext*
    getInstance() {
        static PreloadContext instance;
        return &instance;
    }

    PreloadContext(PreloadContext const&) = delete;

    void
    operator=(PreloadContext const&) = delete;

    void
    init_logging();

    void
    mountdir(const std::string& path);

    const std::string&
    mountdir() const;

    const std::vector<std::string>&
    mountdir_components() const;

    void
    cwd(const std::string& path);

    const std::string&
    cwd() const;

    const std::vector<hermes::endpoint>&
    hosts() const;

    void
    hosts(const std::vector<hermes::endpoint>& addrs);

    const std::vector<std::string>&
    hosts_name() const;

    void
    hosts_name(const std::vector<std::string>& hosts_name);

    hermes::endpoint
    host_endpoint(uint64_t host_id) const;

    std::optional<uint64_t>
    host_index_by_uri(const std::string& uri) const;

    void
    register_host_uri(const std::string& uri, uint64_t host_id);

    uint64_t
    append_host_if_absent(const std::string& hostname, const std::string& uri,
                          const hermes::endpoint& endpoint);

    bool
    epoch_hosts_loaded(gkfs::file_layout::epoch_t epoch) const;

    void
    epoch_hosts(gkfs::file_layout::epoch_t epoch,
                const std::vector<uint64_t>& hosts);

    uint64_t
    epoch_host(gkfs::file_layout::epoch_t epoch, uint64_t target) const;

    std::size_t
    epoch_hosts_size(gkfs::file_layout::epoch_t epoch) const;

    /* --Multiple GekkoFS-- */
    
    const hermes::endpoint
    registry() const;

    void
    registry(const hermes::endpoint &registry);

    bool
    use_workflow() const;

    void
    use_workflow(bool use);

    const std::vector<fs_info>&
    hostsconfig() const;

    void
    hostsconfig(const std::vector<fs_info>& hostsconfig);

    std::map<std::string, unsigned int>&
    pathfs() ; 

    std::map<std::string, unsigned int>&
    wrapper_pathfs() ; 

    gkfs::file_layout::FileLayoutMap&
    file_layouts();

    gkfs::file_layout::epoch_t
    file_layout_latest_version_epoch(const std::string& path) const;

    gkfs::file_layout::epoch_t
    file_layout_global_epoch() const;

    void
    update_file_layout_global_epoch(gkfs::file_layout::epoch_t epoch);

    uint64_t
    local_fs_id() const;

    void
    local_fs_id(uint64_t id);

    std::vector<bloom_filter>&
    bloom_filter_vec();

    void
    init_threadpool(size_t thread_count);

    ThreadPool& 
    thread_pool();

    std::string
    workflow() const;

    void
    workflow(std::string workflow);

    std::string
    unique_id() const;

    void
    unique_id(std::string unique_id);


    std::string
    mergeflows() const;

    void
    mergeflows(std::string mergeflows);

    /* --Multiple GekkoFS-- */

    void
    clear_hosts();

    uint64_t
    local_host_id() const;

    void
    local_host_id(uint64_t id);

    uint64_t
    fwd_host_id() const;

    void
    fwd_host_id(uint64_t id);

    const std::string&
    rpc_protocol() const;

    void
    rpc_protocol(const std::string& rpc_protocol);

    bool
    auto_sm() const;

    void
    auto_sm(bool auto_sm);

    RelativizeStatus
    relativize_fd_path(int dirfd, const char* raw_path,
                       std::string& relative_path, int flags = 0,
                       bool resolve_last_link = true) const;

    bool
    relativize_path(const char* raw_path, std::string& relative_path,
                    bool resolve_last_link = true) const;

    const std::shared_ptr<gkfs::filemap::OpenFileMap>&
    file_map() const;

    /* --FGAP-- */
    const std::shared_ptr<gkfs::filetagmap::FileTagMap>& file_tagmap() const;

    void
    distributor(std::shared_ptr<gkfs::rpc::Distributor> distributor);

    std::shared_ptr<gkfs::rpc::Distributor>
    distributor() const;

    const std::shared_ptr<FsConfig>&
    fs_conf() const;

    void
    enable_interception();

    void
    disable_interception();

    bool
    interception_enabled() const;

    int
    register_internal_fd(int fd);

    void
    unregister_internal_fd(int fd);

    bool
    is_internal_fd(int fd) const;

    void
    protect_user_fds();

    void
    unprotect_user_fds();

    std::string
    get_hostname();

    void
    set_replicas(const int repl);

    int
    get_replicas();
};

} // namespace preload
} // namespace gkfs


#endif // GEKKOFS_PRELOAD_CTX_HPP
