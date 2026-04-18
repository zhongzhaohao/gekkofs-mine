/*
  Copyright 2018-2024, Barcelona Supercomputing Center (BSC), Spain
  Copyright 2015-2024, Johannes Gutenberg Universitaet Mainz, Germany

  This file is part of GekkoFS.

  SPDX-License-Identifier: GPL-3.0-or-later
*/

#include <daemon/malleability_logger.hpp>
#include <daemon/daemon.hpp>

#include <common/malleability_logger.hpp>
#include <common/rpc/rpc_util.hpp>

#include <cerrno>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

extern "C" {
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
}

namespace {

constexpr auto log_root_dir = "/tmp/mallea_logs/daemon_log/";

class DaemonMalleabilityLogger final
    : public gkfs::malleability::common::MalleabilityLoggerBase {
public:
    DaemonMalleabilityLogger()
        : MalleabilityLoggerBase(log_root_dir,
                                 gkfs::rpc::get_my_hostname(true), ::getpid(),
                                 "daemon_", ".log", 40000, false) {
        start_worker();
    }

    ~DaemonMalleabilityLogger() override {
        shutdown();
    }

    pid_t
    log_process_id() const noexcept {
        return process_id();
    }

    std::string
    log_unique_id(const std::string& unique_id) const {
        return normalize_unique_id(unique_id);
    }

private:
    bool
    ensure_dir(const std::string& path) noexcept override {
        if(::mkdir(path.c_str(), 0755) == 0) {
            return true;
        }
        return errno == EEXIST;
    }

    int
    open_append(const std::string& path) noexcept override {
        return ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
                      0644);
    }

    bool
    write_all(int fd, const std::string& data) noexcept override {
        const char* current = data.data();
        auto remaining = data.size();

        while(remaining > 0) {
            const auto written = ::write(fd, current, remaining);
            if(written <= 0) {
                return false;
            }
            current += written;
            remaining -= static_cast<std::size_t>(written);
        }
        return true;
    }

    void
    close_fd(int fd) noexcept override {
        ::close(fd);
    }
};

DaemonMalleabilityLogger&
logger() {
    static DaemonMalleabilityLogger instance;
    return instance;
}

std::size_t
count_hostfile_entries(const std::string& hostfile) {
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

namespace gkfs::daemon::malleability {

PendingIoLog
start_io(const char* operation, const std::string& file, off64_t offset,
         std::size_t requested_size, const std::string& unique_id) {
    return {operation,
            file,
            logger().log_unique_id(unique_id),
            offset,
            requested_size,
            gkfs::malleability::common::MalleabilityLoggerBase::now_epoch_ns()};
}

void
finish_io(PendingIoLog entry, off64_t final_offset, ssize_t actual_size,
          int err, std::uint64_t processed_chunks) {
    try {
        const auto process_id = logger().log_process_id();
        logger().enqueue(
                {gkfs::malleability::common::LogEntryKind::io,
                 std::move(entry.operation),
                 std::move(entry.file),
                 std::move(entry.unique_id),
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
                 err,
                 0,
                 0,
                 GKFS_DATA->epoch(),
                 processed_chunks,
                 0,
                 0,
                 true,
                 true});
    } catch(...) {
    }
}

void
log_epoch_update(const std::string& action, std::uint64_t previous_epoch,
                 std::uint64_t current_epoch, const std::string& hostfile,
                 const std::string& unique_id, int err) {
    try {
        const auto now =
                gkfs::malleability::common::MalleabilityLoggerBase::
                        now_epoch_ns();
        const auto process_id = logger().log_process_id();
        logger().enqueue(
                {gkfs::malleability::common::LogEntryKind::daemon_epoch_update,
                 "daemon_epoch_update",
                 "",
                 logger().log_unique_id(unique_id),
                 action,
                 hostfile,
                 0,
                 0,
                 0,
                 now,
                 now,
                 process_id,
                 process_id,
                 err,
                 previous_epoch,
                 current_epoch,
                 current_epoch,
                 0,
                 0,
                 count_hostfile_entries(hostfile),
                 true,
                 false});
    } catch(...) {
    }
}

} // namespace gkfs::daemon::malleability
