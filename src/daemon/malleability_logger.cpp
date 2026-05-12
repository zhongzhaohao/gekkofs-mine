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

#include <config.hpp>

#include <atomic>
#include <cerrno>
#include <filesystem>
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

namespace fs = std::filesystem;

constexpr auto log_root_dir = "/tmp/mallea_logs/daemon_log";
constexpr std::size_t daemon_flush_threshold = 40000;

std::atomic<bool> logging_enabled_flag{
        gkfs::config::malleability::daemon_logging};
std::atomic<bool> logger_created{false};

class DaemonMalleabilityLogger final
    : public gkfs::malleability::common::MalleabilityLoggerBase {
public:
    DaemonMalleabilityLogger()
        : MalleabilityLoggerBase(log_root_dir,
                                 gkfs::rpc::get_my_hostname(true), ::getpid(),
                                 "daemon_", ".log", daemon_flush_threshold,
                                 false) {
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
        std::error_code ec;
        if(fs::create_directories(path, ec)) {
            return true;
        }
        return !ec && fs::is_directory(path, ec);
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
    static auto* instance = [] {
        auto* created = new DaemonMalleabilityLogger();
        logger_created.store(true, std::memory_order_release);
        return created;
    }();
    return *instance;
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
    if(!logging_enabled()) {
        return {operation, file, "", offset, requested_size, 0};
    }
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
    if(entry.start_time_ns == 0) {
        return;
    }
    if(!logging_enabled()) {
        return;
    }
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
    if(!logging_enabled()) {
        return;
    }
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

void
enable_logging() {
    logging_enabled_flag.store(true, std::memory_order_release);
    if(logger_created.load(std::memory_order_acquire)) {
        logger().enable();
    }
}

void
disable_logging() {
    logging_enabled_flag.store(false, std::memory_order_release);
    if(logger_created.load(std::memory_order_acquire)) {
        logger().disable();
    }
}

bool
logging_enabled() {
    if(!logging_enabled_flag.load(std::memory_order_acquire)) {
        return false;
    }
    if(logger_created.load(std::memory_order_acquire)) {
        return logger().enabled();
    }
    return true;
}

void
shutdown() {
    logging_enabled_flag.store(false, std::memory_order_release);
    if(logger_created.load(std::memory_order_acquire)) {
        logger().shutdown();
    }
}

} // namespace gkfs::daemon::malleability
