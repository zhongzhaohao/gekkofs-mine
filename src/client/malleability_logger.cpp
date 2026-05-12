/*
  Copyright 2018-2024, Barcelona Supercomputing Center (BSC), Spain
  Copyright 2015-2024, Johannes Gutenberg Universitaet Mainz, Germany

  This file is part of GekkoFS' POSIX interface.

  SPDX-License-Identifier: LGPL-3.0-or-later
*/

#include <client/malleability_logger.hpp>
#include <client/env.hpp>
#include <client/preload.hpp>

#include <common/env_util.hpp>
#include <common/hostfile.hpp>
#include <common/malleability_logger.hpp>

#include <config.hpp>

#include <atomic>
#include <cerrno>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

#ifndef BYPASS_SYSCALL
#include <libsyscall_intercept_hook_point.h>
#else
#include <client/void_syscall_intercept.hpp>
#endif

extern "C" {
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
}

namespace {

constexpr auto log_root_dir = "/tmp/malleability_log";

std::atomic<bool> logging_enabled_flag{
        gkfs::config::malleability::client_logging};
std::atomic<bool> logger_created{false};

class ClientMalleabilityLogger final
    : public gkfs::malleability::common::MalleabilityLoggerBase {
public:
    ClientMalleabilityLogger()
        : MalleabilityLoggerBase(log_root_dir, CTX->get_hostname(), ::getpid()) {
        start_worker();
    }

    ~ClientMalleabilityLogger() override {
        shutdown();
    }

    pid_t
    log_process_id() const noexcept {
        return process_id();
    }

private:
    bool
    ensure_dir(const std::string& path) noexcept override {
        const auto mkdir_ret =
                ::syscall_no_intercept(SYS_mkdir, path.c_str(), 0755);
        const auto mkdir_err = ::syscall_error_code(mkdir_ret);
        return mkdir_err == 0 || mkdir_err == EEXIST;
    }

    int
    open_append(const std::string& path) noexcept override {
        const auto fd_ret = ::syscall_no_intercept(
                SYS_openat, AT_FDCWD, path.c_str(),
                O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        const auto fd_err = ::syscall_error_code(fd_ret);
        if(fd_err != 0) {
            return -1;
        }
        return static_cast<int>(fd_ret);
    }

    bool
    write_all(int fd, const std::string& data) noexcept override {
        const char* current = data.data();
        auto remaining = data.size();

        while(remaining > 0) {
            const auto write_ret =
                    ::syscall_no_intercept(SYS_write, fd, current, remaining);
            const auto write_err = ::syscall_error_code(write_ret);
            if(write_err != 0 || write_ret == 0) {
                return false;
            }
            current += write_ret;
            remaining -= static_cast<std::size_t>(write_ret);
        }
        return true;
    }

    void
    close_fd(int fd) noexcept override {
        ::syscall_no_intercept(SYS_close, fd);
    }
};

ClientMalleabilityLogger*
logger() noexcept {
    static auto* instance = [] {
        auto* created = new ClientMalleabilityLogger();
        logger_created.store(true, std::memory_order_release);
        return created;
    }();
    return instance;
}

std::string
log_unique_id() {
    auto unique_id = CTX->unique_id();
    return unique_id.empty() ? "default" : unique_id;
}

std::size_t
count_hostfile_entries(std::uint64_t epoch) {
    const auto base_hostfile = gkfs::env::get_var(
            gkfs::env::HOSTS_FILE, gkfs::config::hostfile_path);
    const auto hostfile = gkfs::utils::get_epoch_hostfile_name(
            base_hostfile, CTX->unique_id(), epoch);

    std::ifstream input(hostfile);
    if(!input) {
        return 0;
    }

    std::size_t count = 0;
    std::string line;
    std::string hostname;
    std::string uri;
    while(std::getline(input, line)) {
        std::istringstream iss(line);
        if(iss >> hostname >> uri) {
            ++count;
        }
    }
    return count;
}

} // namespace

namespace gkfs::malleability {

PendingIoLog
start_io(const char* operation, const std::string& file, off64_t offset,
         std::size_t requested_size) {
    if(!logging_enabled()) {
        return {operation, file, offset, requested_size, 0};
    }
    return {operation, file, offset, requested_size,
            gkfs::malleability::common::MalleabilityLoggerBase::now_epoch_ns()};
}

void
finish_io(PendingIoLog entry, off64_t final_offset, ssize_t actual_size,
          int err) {
    const auto saved_errno = errno;
    if(entry.start_time_ns == 0) {
        errno = saved_errno;
        return;
    }
    if(!logging_enabled()) {
        errno = saved_errno;
        return;
    }
    try {
        const auto process_id = logger()->log_process_id();
        logger()->enqueue(
                {gkfs::malleability::common::LogEntryKind::io,
                 std::move(entry.operation),
                 std::move(entry.file),
                 log_unique_id(),
                 "",
                 "",
                 final_offset,
                 entry.requested_size,
                 actual_size,
                 entry.start_time_ns,
                 gkfs::malleability::common::MalleabilityLoggerBase::
                         now_epoch_ns(),
                 process_id,
                 process_id,
                 err});
    } catch(...) {
    }
    errno = saved_errno;
}

void
flush() {
    const auto saved_errno = errno;
    if(logger_created.load(std::memory_order_acquire)) {
        logger()->flush();
    }
    errno = saved_errno;
}

void
enable_logging() {
    logging_enabled_flag.store(true, std::memory_order_release);
    if(logger_created.load(std::memory_order_acquire)) {
        logger()->enable();
    }
}

void
disable_logging() {
    logging_enabled_flag.store(false, std::memory_order_release);
    if(logger_created.load(std::memory_order_acquire)) {
        logger()->disable();
    }
}

bool
logging_enabled() {
    if(!logging_enabled_flag.load(std::memory_order_acquire)) {
        return false;
    }
    if(logger_created.load(std::memory_order_acquire)) {
        return logger()->enabled();
    }
    return true;
}

void
shutdown() {
    const auto saved_errno = errno;
    logging_enabled_flag.store(false, std::memory_order_release);
    if(logger_created.load(std::memory_order_acquire)) {
        logger()->shutdown();
    }
    errno = saved_errno;
}

void
log_epoch_change(std::uint64_t previous_epoch, std::uint64_t current_epoch) {
    if(!logging_enabled()) {
        return;
    }
    if(current_epoch <= previous_epoch) {
        return;
    }

    const auto previous_host_count = count_hostfile_entries(previous_epoch);
    const auto current_host_count = count_hostfile_entries(current_epoch);
    if(previous_host_count == current_host_count) {
        return;
    }

    const auto now =
            gkfs::malleability::common::MalleabilityLoggerBase::now_epoch_ns();
    const auto operation =
            current_host_count > previous_host_count ? "expand" : "shrink";
    const auto process_id = logger()->log_process_id();

    logger()->enqueue(
            {gkfs::malleability::common::LogEntryKind::epoch_change,
             operation,
             "",
             log_unique_id(),
             "",
             "",
             0,
             0,
             0,
             now,
             now,
             process_id,
             process_id,
             0,
             previous_epoch,
             current_epoch,
             0,
             0,
             previous_host_count,
             current_host_count});
}

} // namespace gkfs::malleability
