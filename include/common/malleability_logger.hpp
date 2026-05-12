/*
  Copyright 2018-2024, Barcelona Supercomputing Center (BSC), Spain
  Copyright 2015-2024, Johannes Gutenberg Universitaet Mainz, Germany

  This file is part of GekkoFS.

  SPDX-License-Identifier: GPL-3.0-or-later
*/

#ifndef GEKKOFS_COMMON_MALLEABILITY_LOGGER_HPP
#define GEKKOFS_COMMON_MALLEABILITY_LOGGER_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

extern "C" {
#include <sys/types.h>
}

namespace gkfs::malleability::common {

enum class LogEntryKind {
    io,
    epoch_change,
    daemon_epoch_update,
};

struct LogEntry {
    LogEntryKind kind{LogEntryKind::io};
    std::string operation;
    std::string file;
    std::string unique_id;
    std::string action;
    std::string hostfile;
    off64_t offset{0};
    std::size_t requested_size{0};
    ssize_t actual_size{0};
    std::int64_t start_time_ns{0};
    std::int64_t end_time_ns{0};
    pid_t process_id{0};
    pid_t progress_id{0};
    int err{0};
    std::uint64_t previous_epoch{0};
    std::uint64_t epoch{0};
    std::uint64_t daemon_epoch{0};
    std::uint64_t processed_chunks{0};
    std::size_t previous_host_count{0};
    std::size_t host_count{0};
    bool include_daemon_epoch{false};
    bool include_processed_chunks{false};
};

class MalleabilityLoggerBase {
public:
    MalleabilityLoggerBase(std::string root_dir, std::string hostname,
                           pid_t process_id,
                           std::string log_file_prefix = "",
                           std::string log_file_suffix = ".log",
                           std::size_t flush_threshold = 40000,
                           bool use_unique_id_dir = true);

    virtual
    ~MalleabilityLoggerBase();

    MalleabilityLoggerBase(const MalleabilityLoggerBase&) = delete;
    MalleabilityLoggerBase&
    operator=(const MalleabilityLoggerBase&) = delete;

    void
    enqueue(LogEntry entry) noexcept;

    void
    flush() noexcept;

    void
    enable() noexcept;

    void
    disable() noexcept;

    bool
    enabled() const noexcept;

    void
    shutdown() noexcept;

    static std::int64_t
    now_epoch_ns() noexcept;

protected:
    void
    start_worker() noexcept;

    const std::string&
    hostname() const noexcept;

    pid_t
    process_id() const noexcept;

    std::string
    normalize_unique_id(const std::string& unique_id) const;

private:
    struct LogNode;

    virtual bool
    ensure_dir(const std::string& path) noexcept;

    virtual int
    open_append(const std::string& path) noexcept;

    virtual bool
    write_all(int fd, const std::string& data) noexcept;

    virtual void
    close_fd(int fd) noexcept;

    void
    run() noexcept;

    void
    flush_pending() noexcept;

    void
    discard_pending() noexcept;

    void
    delete_list(LogNode* node) noexcept;

    bool
    ensure_cached_dir(const std::string& path);

    void
    append_batch(const std::string& log_path, const std::string& batch);

    std::string
    log_file_path(const LogEntry& entry) const;

    std::string root_dir_;
    std::string hostname_;
    pid_t process_id_;
    std::string log_file_prefix_;
    std::string log_file_suffix_;
    std::size_t flush_threshold_;
    bool use_unique_id_dir_;

    class Impl;
    Impl* impl_;
};

} // namespace gkfs::malleability::common

#endif // GEKKOFS_COMMON_MALLEABILITY_LOGGER_HPP
