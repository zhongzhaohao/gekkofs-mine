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

#include <client/preload_context.hpp>
#include <client/env.hpp>
#include <client/logging.hpp>
#include <client/malleability_logger.hpp>
#include <client/open_file_map.hpp>
#include <client/open_dir.hpp>
#include <client/path.hpp>

#include <common/env_util.hpp>
#include <common/path_util.hpp>
#include <config.hpp>

#include <hermes.hpp>

#include <cassert>
#include <stdexcept>

#ifndef BYPASS_SYSCALL
#include <libsyscall_intercept_hook_point.h>
#else
#include <client/void_syscall_intercept.hpp>
#endif

extern "C" {
#include <syscall.h>
}

namespace gkfs {

namespace preload {

decltype(PreloadContext::MIN_INTERNAL_FD) constexpr PreloadContext::
        MIN_INTERNAL_FD;
decltype(PreloadContext::MAX_USER_FDS) constexpr PreloadContext::MAX_USER_FDS;

PreloadContext::PreloadContext()
    : ofm_(std::make_shared<gkfs::filemap::OpenFileMap>()),
    fileTagMap_(std::make_shared<gkfs::filetagmap::FileTagMap>()),/* --FGAP-- */
      fs_conf_(std::make_shared<FsConfig>()) {

    internal_fds_.set();
    internal_fds_must_relocate_ = true;

    char host[255];
    gethostname(host, 255);
    hostname = host;
    PreloadContext::set_replicas(
            std::stoi(gkfs::env::get_var(gkfs::env::NUM_REPL, "0")));
}

void
PreloadContext::init_logging() {

    const std::string log_opts = gkfs::env::get_var(
            gkfs::env::LOG, gkfs::config::log::client_log_level);

    const std::string log_output = gkfs::env::get_var(
            gkfs::env::LOG_OUTPUT, gkfs::config::log::client_log_path);

    const bool log_per_process =
            gkfs::env::get_var(gkfs::env::LOG_PER_PROCESS).empty() ? false
                                                                   : true;

#ifdef GKFS_DEBUG_BUILD
    // atoi returns 0 if no int conversion can be performed, which works
    // for us since if the user provides a non-numeric value we can just treat
    // it as zero
    const int log_verbosity = std::atoi(
            gkfs::env::get_var(gkfs::env::LOG_DEBUG_VERBOSITY, "0").c_str());

    const std::string log_filter =
            gkfs::env::get_var(gkfs::env::LOG_SYSCALL_FILTER, "");
#endif

    const std::string trunc_val =
            gkfs::env::get_var(gkfs::env::LOG_OUTPUT_TRUNC);

    const bool log_trunc = (!trunc_val.empty() && trunc_val[0] != '0');

    gkfs::log::create_global_logger(log_opts, log_output, log_per_process,
                                    log_trunc
#ifdef GKFS_DEBUG_BUILD
                                    ,
                                    log_filter, log_verbosity
#endif
    );
}

void
PreloadContext::mountdir(const std::string& path) {
    assert(gkfs::path::is_absolute(path));
    assert(!gkfs::path::has_trailing_slash(path));
    mountdir_components_ = gkfs::path::split_path(path);
    mountdir_ = path;
}

const std::string&
PreloadContext::mountdir() const {
    return mountdir_;
}

const std::vector<std::string>&
PreloadContext::mountdir_components() const {
    return mountdir_components_;
}

void
PreloadContext::cwd(const std::string& path) {
    cwd_ = path;
}

const std::string&
PreloadContext::cwd() const {
    return cwd_;
}

const std::vector<hermes::endpoint>&
PreloadContext::hosts() const {
    return hosts_;
}

void
PreloadContext::hosts(const std::vector<hermes::endpoint>& endpoints) {
    std::lock_guard<std::mutex> lock(hosts_mutex_);
    hosts_ = endpoints;
}

const std::vector<std::string>&
PreloadContext::hosts_name() const {
    return hosts_name_;
}

void
PreloadContext::hosts_name(const std::vector<std::string>& hosts_name) {
    std::lock_guard<std::mutex> lock(hosts_mutex_);
    hosts_name_ = hosts_name;
}

hermes::endpoint
PreloadContext::host_endpoint(uint64_t host_id) const {
    std::lock_guard<std::mutex> lock(hosts_mutex_);
    return hosts_.at(host_id);
}

std::optional<uint64_t>
PreloadContext::host_index_by_uri(const std::string& uri) const {
    std::lock_guard<std::mutex> lock(hosts_mutex_);
    const auto it = host_uri_to_index_.find(uri);
    if(it == host_uri_to_index_.end()) {
        return {};
    }
    return it->second;
}

void
PreloadContext::register_host_uri(const std::string& uri, uint64_t host_id) {
    std::lock_guard<std::mutex> lock(hosts_mutex_);
    host_uri_to_index_[uri] = host_id;
}

uint64_t
PreloadContext::append_host_if_absent(const std::string& hostname,
                                      const std::string& uri,
                                      const hermes::endpoint& endpoint) {
    std::lock_guard<std::mutex> lock(hosts_mutex_);
    const auto it = host_uri_to_index_.find(uri);
    if(it != host_uri_to_index_.end()) {
        return it->second;
    }

    const auto host_id = hosts_.size();
    hosts_.push_back(endpoint);
    hosts_name_.push_back(hostname);
    host_uri_to_index_[uri] = host_id;
    return host_id;
}

bool
PreloadContext::epoch_hosts_loaded(gkfs::file_layout::epoch_t epoch) const {
    std::lock_guard<std::mutex> lock(hosts_mutex_);
    return epoch_hosts_.find(epoch) != epoch_hosts_.end();
}

void
PreloadContext::epoch_hosts(gkfs::file_layout::epoch_t epoch,
                            const std::vector<uint64_t>& hosts) {
    std::lock_guard<std::mutex> lock(hosts_mutex_);
    epoch_hosts_[epoch] = hosts;
}

uint64_t
PreloadContext::epoch_host(gkfs::file_layout::epoch_t epoch,
                           uint64_t target) const {
    std::lock_guard<std::mutex> lock(hosts_mutex_);
    const auto it = epoch_hosts_.find(epoch);
    if(it == epoch_hosts_.end() || target >= it->second.size()) {
        throw std::out_of_range("epoch target is not mapped to a host");
    }
    return it->second.at(target);
}

std::size_t
PreloadContext::epoch_hosts_size(gkfs::file_layout::epoch_t epoch) const {
    std::lock_guard<std::mutex> lock(hosts_mutex_);
    const auto it = epoch_hosts_.find(epoch);
    if(it == epoch_hosts_.end()) {
        return 0;
    }
    return it->second.size();
}

/* --Multiple GekkoFS-- */
const hermes::endpoint
PreloadContext::registry() const {
    return registry_;
}

void
PreloadContext::registry(const hermes::endpoint& registry) {
    registry_ = registry;
}

bool
PreloadContext::use_workflow() const {
    return use_workflow_;
}

void
PreloadContext::use_workflow(bool use_workflow) {
    use_workflow_ = use_workflow;
}

const std::vector<fs_info>&
PreloadContext::hostsconfig() const {
    return hostsconfig_;
}

void
PreloadContext::hostsconfig(const std::vector<fs_info>& hconfig) {
    hostsconfig_ = hconfig;
}

std::map<std::string, unsigned int>&
PreloadContext::pathfs() {
    return pathfs_;
}

std::map<std::string, unsigned int>&
PreloadContext::wrapper_pathfs() {
    return wrapper_pathfs_;
}

gkfs::file_layout::FileLayoutMap&
PreloadContext::file_layouts() {
    return file_layouts_;
}

gkfs::file_layout::epoch_t
PreloadContext::file_layout_latest_version_epoch(const std::string& path) const {
    const auto record = file_layouts_.find(path);
    if(!record) {
        return 0;
    }
    return gkfs::file_layout::file_layout_latest_version_epoch(record);
}

gkfs::file_layout::epoch_t
PreloadContext::file_layout_global_epoch() const {
    return file_layout_global_epoch_;
}

void
PreloadContext::update_file_layout_global_epoch(
        gkfs::file_layout::epoch_t epoch) {
    if(epoch > file_layout_global_epoch_) {
        const auto previous_epoch = file_layout_global_epoch_;
        file_layout_global_epoch_ = epoch;
        gkfs::malleability::log_epoch_change(previous_epoch, epoch);
    }
}

uint64_t
PreloadContext::local_fs_id() const {
    return local_fs_id_;
}

void
PreloadContext::local_fs_id(uint64_t id) {
    local_fs_id_ = id;
}

std::vector<bloom_filter>&
PreloadContext::bloom_filter_vec() {
    return bloom_filter_vec_;
}

void
PreloadContext::init_threadpool(size_t thread_count){
    thread_pool_.init(thread_count);
}

ThreadPool& 
PreloadContext::thread_pool() {
    return thread_pool_;
}

std::string
PreloadContext::workflow() const {
    return workflow_;
}

void
PreloadContext::workflow(std::string workflow){
    workflow_ = workflow;
}

std::string
PreloadContext::unique_id() const {
    return unique_id_;
}

void
PreloadContext::unique_id(std::string unique_id) {
    unique_id_ = unique_id;
}

std::string
PreloadContext::mergeflows() const {
    return mergeflows_;
}

void
PreloadContext::mergeflows(std::string mergeflows){
    mergeflows_ = mergeflows;
}

/* --Multiple GekkoFS-- */

void
PreloadContext::clear_hosts() {
    std::lock_guard<std::mutex> lock(hosts_mutex_);
    hosts_.clear();
    hosts_name_.clear();
    host_uri_to_index_.clear();
    epoch_hosts_.clear();
}

uint64_t
PreloadContext::local_host_id() const {
    return local_host_id_;
}

void
PreloadContext::local_host_id(uint64_t id) {
    local_host_id_ = id;
}

uint64_t
PreloadContext::fwd_host_id() const {
    return fwd_host_id_;
}

void
PreloadContext::fwd_host_id(uint64_t id) {
    fwd_host_id_ = id;
}

const std::string&
PreloadContext::rpc_protocol() const {
    return rpc_protocol_;
}

void
PreloadContext::rpc_protocol(const std::string& rpc_protocol) {
    rpc_protocol_ = rpc_protocol;
}

bool
PreloadContext::auto_sm() const {
    return auto_sm_;
}

void
PreloadContext::auto_sm(bool auto_sm) {
    PreloadContext::auto_sm_ = auto_sm;
}

/* --FGAP-- */
std::string get_filename_from_path(const char* path) {
    std::string path_str(path); // convert char* to std::string
    // find the location of the last '/' 
    size_t last_slash_idx = path_str.find_last_of("/\\");
    if (last_slash_idx == std::string::npos) {
        return path_str; // if no '/' in path, return the whole path
    }
    return path_str.substr(last_slash_idx + 1); // return the string after the last '/' 
} /* --FGAP-- */

RelativizeStatus
PreloadContext::relativize_fd_path(int dirfd, const char* raw_path,
                                   std::string& relative_path, int flags,
                                   bool resolve_last_link) const {

    // Relativize path should be called only after the library constructor has
    // been executed
    assert(interception_enabled_);
    // If we run the constructor we also already setup the mountdir
    assert(!mountdir_.empty());

    // We assume raw path is valid
    assert(raw_path != nullptr);

    std::string path;

    if(raw_path != nullptr && raw_path[0] != gkfs::path::separator) {
        // path is relative
        if(dirfd == AT_FDCWD) {
            // path is relative to cwd
            path = gkfs::path::prepend_path(cwd_, raw_path);
        } else {
            if(!ofm_->exist(dirfd)) {
                return RelativizeStatus::fd_unknown;
            } else {
                // check if we have the AT_EMPTY_PATH flag
                // for fstatat.
                if(flags & AT_EMPTY_PATH) {
                    relative_path = ofm_->get(dirfd)->path();
                    return RelativizeStatus::internal;
                }
            }
            // path is relative to fd
            auto dir = ofm_->get_dir(dirfd);
            if(dir == nullptr) {
                return RelativizeStatus::fd_not_a_dir;
            }
            path = mountdir_;
            path.append(dir->path());
            path.push_back(gkfs::path::separator);
            path.append(raw_path);
        }
    } else {
        path = raw_path;
    }

    /* --FGAP-- */
    // 1. get basename
    // 2. check if exists
    //
    std::string basename = get_filename_from_path(raw_path);
    if (fileTagMap_->exist(basename)){
	    std::string fs_tag = fileTagMap_->get_tag(basename);
	    int fs_index = std::stoi(fs_tag);
	    std::string fs_path = fileTagMap_->get_fs_at_index(fs_index);

	    // smt_fgap: relative_path is abs_path
	    relative_path = fs_path + basename;
	    std::string tmp_path = fs_path + basename;
        auto [is_in, resolved] =
            gkfs::path::resolve(tmp_path, resolve_last_link);
	    if (is_in){
	        //std::cout << "[fgap_debug] fgap_trans to " << relative_path << " from " << raw_path 
		//	<< " fs_index: " << fs_index << " fs_path: " << fs_path << std::endl;
	        LOG(INFO, "[fgap_debug] fgap_trans to [{}] from [{}], fs_index:{}, fs_path:{}", \
				relative_path, raw_path, fs_index, fs_path);
		    return RelativizeStatus::internal;
	    } else {
	        relative_path = fs_path + basename;
	        //std::cout << "[fgap_debug] fgap_trans to " << relative_path << " from " << raw_path
                //	<< " fs_index: " << fs_index << " fs_path: " << fs_path << std::endl;
	        LOG(INFO, "[fgap_debug] fgap_trans to [{}] from [{}], fs_index:{}, fs_path:{}", \
				relative_path, raw_path, fs_index, fs_path);
	        //return RelativizeStatus::fgap_trans;
	        // !!!! note that we do not use fgap_trans anymore, instead, we use external directly
	        //       if use fgap_trans, we need to do a lot of work in hook stat/statx/link......
	        return RelativizeStatus::external;
	    }

    } 
    /* --FGAP-- */

    auto [is_in_path, resolved_path] =
            gkfs::path::resolve(path, resolve_last_link);
    relative_path = resolved_path;
    if(is_in_path) {
        return RelativizeStatus::internal;
    }
    return RelativizeStatus::external;
}

bool
PreloadContext::relativize_path(const char* raw_path,
                                std::string& relative_path,
                                bool resolve_last_link) const {
    // Relativize path should be called only after the library constructor has
    // been executed
    assert(interception_enabled_);
    // If we run the constructor we also already setup the mountdir
    assert(!mountdir_.empty());

    // We assume raw path is valid
    assert(raw_path != nullptr);

    std::string path;

    if(raw_path != nullptr && raw_path[0] != gkfs::path::separator) {
        /* Path is not absolute, we need to prepend CWD;
         * First reserve enough space to minimize memory copy
         */
        path = gkfs::path::prepend_path(cwd_, raw_path);
    } else {
        path = raw_path;
    }

    auto [is_in_path, resolved_path] =
        gkfs::path::resolve(path, resolve_last_link);
    relative_path = resolved_path;
    return is_in_path;
}

const std::shared_ptr<gkfs::filemap::OpenFileMap>&
PreloadContext::file_map() const {
    return ofm_;
}

void
PreloadContext::distributor(std::shared_ptr<gkfs::rpc::Distributor> d) {
    distributor_ = d;
}

std::shared_ptr<gkfs::rpc::Distributor>
PreloadContext::distributor() const {
    return distributor_;
}

/* --FGAP-- */
const std::shared_ptr<gkfs::filetagmap::FileTagMap>& PreloadContext::file_tagmap() const {
    return fileTagMap_;
}

const std::shared_ptr<FsConfig>&
PreloadContext::fs_conf() const {
    return fs_conf_;
}

void
PreloadContext::enable_interception() {
    interception_enabled_ = true;
}

void
PreloadContext::disable_interception() {
    interception_enabled_ = false;
}

bool
PreloadContext::interception_enabled() const {
    return interception_enabled_;
}

int
PreloadContext::register_internal_fd(int fd) {

    assert(fd >= 0);
    return fd;
    if(!internal_fds_must_relocate_) {
        LOG(DEBUG, "registering fd {} as internal (no relocation needed)", fd);
        assert(fd >= MIN_INTERNAL_FD);
        internal_fds_.reset(fd - MIN_INTERNAL_FD);
        return fd;
    }

    LOG(DEBUG, "registering fd {} as internal (needs relocation)", fd);

    std::lock_guard<std::mutex> lock(internal_fds_mutex_);
    const int pos = internal_fds_._Find_first();

    if(static_cast<std::size_t>(pos) == internal_fds_.size()) {
        throw std::runtime_error(
                "Internal GekkoFS file descriptors exhausted, increase GKFS_MAX_INTERNAL_FDS in "
                "CMake, rebuild GekkoFS and try again.");
    }
    internal_fds_.reset(pos);


#if defined(GKFS_ENABLE_LOGGING) && defined(GKFS_DEBUG_BUILD)
    long args[gkfs::syscall::MAX_ARGS]{fd, pos + MIN_INTERNAL_FD, O_CLOEXEC};
#endif

    LOG(SYSCALL,
        gkfs::syscall::from_internal_code | gkfs::syscall::to_kernel |
                gkfs::syscall::not_executed,
        SYS_dup3, args);

    const int ifd = ::syscall_no_intercept(SYS_dup3, fd, pos + MIN_INTERNAL_FD,
                                           O_CLOEXEC);

    LOG(SYSCALL,
        gkfs::syscall::from_internal_code | gkfs::syscall::to_kernel |
                gkfs::syscall::executed,
        SYS_dup3, args, ifd);

    assert(::syscall_error_code(ifd) == 0);

#if defined(GKFS_ENABLE_LOGGING) && defined(GKFS_DEBUG_BUILD)
    long args2[gkfs::syscall::MAX_ARGS]{fd};
#endif

    LOG(SYSCALL,
        gkfs::syscall::from_internal_code | gkfs::syscall::to_kernel |
                gkfs::syscall::not_executed,
        SYS_close, args2);

#if defined(GKFS_ENABLE_LOGGING) && defined(GKFS_DEBUG_BUILD)
    int rv = ::syscall_no_intercept(SYS_close, fd);
#else
    ::syscall_no_intercept(SYS_close, fd);
#endif

    LOG(SYSCALL,
        gkfs::syscall::from_internal_code | gkfs::syscall::to_kernel |
                gkfs::syscall::executed,
        SYS_close, args2, rv);

    LOG(DEBUG, "    (fd {} relocated to ifd {})", fd, ifd);

    return ifd;
}

void
PreloadContext::unregister_internal_fd(int fd) {
    return ;
    LOG(DEBUG, "unregistering internal fd {}", fd);

    assert(fd >= MIN_INTERNAL_FD);

    const auto pos = fd - MIN_INTERNAL_FD;

    std::lock_guard<std::mutex> lock(internal_fds_mutex_);
    internal_fds_.set(pos);
}

bool
PreloadContext::is_internal_fd(int fd) const {
    return false;
    if(fd < MIN_INTERNAL_FD) {
        return false;
    }

    const auto pos = fd - MIN_INTERNAL_FD;

    std::lock_guard<std::mutex> lock(internal_fds_mutex_);
    return !internal_fds_.test(pos);
}

void
PreloadContext::protect_user_fds() {

    LOG(DEBUG, "Protecting application fds [{}, {}]", 0, MAX_USER_FDS - 1);

    const int nullfd =
            ::syscall_no_intercept(SYS_openat, 0, "/dev/null", O_RDONLY);
    assert(::syscall_error_code(nullfd) == 0);
    protected_fds_.set(nullfd);

    const auto fd_is_open = [](int fd) -> bool {
        const int ret = ::syscall_no_intercept(SYS_fcntl, fd, F_GETFD);
        const int error = ::syscall_error_code(ret);
        return error == 0 || error != EBADF;
    };

    for(int fd = 0; fd < MAX_USER_FDS; ++fd) {
        if(fd_is_open(fd)) {
            LOG(DEBUG, "  fd {} was already in use, skipping", fd);
            continue;
        }

        const int ret = ::syscall_no_intercept(SYS_dup3, nullfd, fd, O_CLOEXEC);
        assert(::syscall_error_code(ret) == 0);
        protected_fds_.set(fd);
    }

    internal_fds_must_relocate_ = false;
}

void
PreloadContext::unprotect_user_fds() {

    for(std::size_t fd = 0; fd < protected_fds_.size(); ++fd) {
        if(!protected_fds_[fd]) {
            continue;
        }

        const int ret =
                ::syscall_error_code(::syscall_no_intercept(SYS_close, fd));

        if(ret != 0) {
            LOG(ERROR, "Failed to unprotect fd")
        }
    }

    internal_fds_must_relocate_ = true;
}


std::string
PreloadContext::get_hostname() {
    return hostname;
}

void
PreloadContext::set_replicas(const int repl) {
    replicas_ = repl;
}

int
PreloadContext::get_replicas() {
    return replicas_;
}

} // namespace preload
} // namespace gkfs
