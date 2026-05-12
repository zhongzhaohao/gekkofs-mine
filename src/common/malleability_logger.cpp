/*
  Copyright 2018-2024, Barcelona Supercomputing Center (BSC), Spain
  Copyright 2015-2024, Johannes Gutenberg Universitaet Mainz, Germany

  This file is part of GekkoFS.

  SPDX-License-Identifier: GPL-3.0-or-later
*/

#include <common/malleability_logger.hpp>

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <mutex>
#include <new>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <utility>

namespace {

std::string
json_escape(const std::string& value) {
    std::ostringstream escaped;
    for(const auto ch : value) {
        const auto uch = static_cast<unsigned char>(ch);
        switch(ch) {
            case '"':
                escaped << "\\\"";
                break;
            case '\\':
                escaped << "\\\\";
                break;
            case '\b':
                escaped << "\\b";
                break;
            case '\f':
                escaped << "\\f";
                break;
            case '\n':
                escaped << "\\n";
                break;
            case '\r':
                escaped << "\\r";
                break;
            case '\t':
                escaped << "\\t";
                break;
            default:
                if(uch < 0x20) {
                    escaped << "\\u00";
                    const char hex[] = "0123456789abcdef";
                    escaped << hex[(uch >> 4) & 0x0f] << hex[uch & 0x0f];
                } else {
                    escaped << ch;
                }
        }
    }
    return escaped.str();
}

std::string
format_entry(const gkfs::malleability::common::LogEntry& entry,
             const std::string& hostname) {
    const auto actual_size = entry.actual_size > 0 ? entry.actual_size : 0;

    std::ostringstream line;
    line << "{\"start_time_ns\":" << entry.start_time_ns
         << ",\"end_time_ns\":" << entry.end_time_ns << ",\"operation\":\""
         << json_escape(entry.operation) << "\",\"file\":\""
         << json_escape(entry.file) << "\",\"offset\":"
         << static_cast<long long>(entry.offset) << ",\"size\":" << actual_size
         << ",\"requested_size\":" << entry.requested_size
         << ",\"hostname\":\"" << json_escape(hostname)
         << "\",\"process_id\":" << entry.process_id
         << ",\"progress_id\":" << entry.progress_id << ",\"unique_id\":\""
         << json_escape(entry.unique_id) << "\",\"errno\":" << entry.err
         << ",\"status\":\"" << (entry.err == 0 ? "ok" : "error") << "\"";

    if(entry.include_daemon_epoch || entry.daemon_epoch != 0 ||
       entry.kind ==
               gkfs::malleability::common::LogEntryKind::daemon_epoch_update) {
        line << ",\"daemon_epoch\":" << entry.daemon_epoch;
    }

    if(entry.kind == gkfs::malleability::common::LogEntryKind::io &&
       entry.include_processed_chunks) {
        line << ",\"processed_chunks\":" << entry.processed_chunks;
    }

    if(entry.kind == gkfs::malleability::common::LogEntryKind::epoch_change) {
        line << ",\"previous_epoch\":" << entry.previous_epoch
             << ",\"epoch\":" << entry.epoch
             << ",\"previous_host_count\":" << entry.previous_host_count
             << ",\"host_count\":" << entry.host_count;
    }

    if(entry.kind ==
       gkfs::malleability::common::LogEntryKind::daemon_epoch_update) {
        line << ",\"action\":\"" << json_escape(entry.action)
             << "\",\"previous_epoch\":" << entry.previous_epoch
             << ",\"epoch\":" << entry.epoch << ",\"hostfile\":\""
             << json_escape(entry.hostfile) << "\",\"host_count\":"
             << entry.host_count;
    }

    line << "}\n";
    return line.str();
}

} // namespace

namespace gkfs::malleability::common {

struct MalleabilityLoggerBase::LogNode {
    explicit LogNode(LogEntry entry) : entry(std::move(entry)) {}

    LogEntry entry;
    LogNode* next{nullptr};
};

class MalleabilityLoggerBase::Impl {
public:
    std::atomic<LogNode*> pending_head{nullptr};
    std::atomic<std::size_t> pending_count{0};
    std::atomic<bool> enabled{true};
    std::atomic<bool> flush_requested{false};
    std::atomic<bool> shutdown_requested{false};
    std::atomic<bool> stopped{false};
    std::mutex cv_mutex;
    std::mutex flush_mutex;
    std::condition_variable cv;
    std::thread worker;
    bool worker_started{false};
    std::unordered_set<std::string> ready_dirs;
};

MalleabilityLoggerBase::MalleabilityLoggerBase(
        std::string root_dir, std::string hostname, pid_t process_id,
        std::string log_file_prefix, std::string log_file_suffix,
        std::size_t flush_threshold, bool use_unique_id_dir)
    : root_dir_(std::move(root_dir)), hostname_(std::move(hostname)),
      process_id_(process_id), log_file_prefix_(std::move(log_file_prefix)),
      log_file_suffix_(std::move(log_file_suffix)),
      flush_threshold_(flush_threshold), use_unique_id_dir_(use_unique_id_dir),
      impl_(new Impl()) {}

MalleabilityLoggerBase::~MalleabilityLoggerBase() {
    if(impl_ != nullptr) {
        if(!impl_->stopped.load(std::memory_order_acquire)) {
            impl_->shutdown_requested.store(true, std::memory_order_release);
            impl_->cv.notify_one();
            if(impl_->worker.joinable()) {
                impl_->worker.join();
            }
            discard_pending();
            impl_->stopped.store(true, std::memory_order_release);
        }
        delete impl_;
    }
}

std::int64_t
MalleabilityLoggerBase::now_epoch_ns() noexcept {
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   now.time_since_epoch())
            .count();
}

void
MalleabilityLoggerBase::start_worker() noexcept {
    try {
        impl_->worker = std::thread(&MalleabilityLoggerBase::run, this);
        impl_->worker_started = true;
    } catch(...) {
        impl_->worker_started = false;
    }
}

const std::string&
MalleabilityLoggerBase::hostname() const noexcept {
    return hostname_;
}

pid_t
MalleabilityLoggerBase::process_id() const noexcept {
    return process_id_;
}

std::string
MalleabilityLoggerBase::normalize_unique_id(const std::string& unique_id) const {
    return unique_id.empty() ? "default" : unique_id;
}

bool
MalleabilityLoggerBase::ensure_dir(const std::string& path) noexcept {
    (void) path;
    return false;
}

int
MalleabilityLoggerBase::open_append(const std::string& path) noexcept {
    (void) path;
    return -1;
}

bool
MalleabilityLoggerBase::write_all(int fd, const std::string& data) noexcept {
    (void) fd;
    (void) data;
    return false;
}

void
MalleabilityLoggerBase::close_fd(int fd) noexcept {
    (void) fd;
}

void
MalleabilityLoggerBase::enqueue(LogEntry entry) noexcept {
    const auto saved_errno = errno;
    if(!impl_->enabled.load(std::memory_order_acquire) ||
       impl_->stopped.load(std::memory_order_acquire)) {
        errno = saved_errno;
        return;
    }

    auto* node = new(std::nothrow) LogNode(std::move(entry));
    if(node == nullptr) {
        errno = saved_errno;
        return;
    }

    auto* head = impl_->pending_head.load(std::memory_order_acquire);
    do {
        node->next = head;
    } while(!impl_->pending_head.compare_exchange_weak(
            head, node, std::memory_order_release, std::memory_order_acquire));

    const auto queued =
            impl_->pending_count.fetch_add(1, std::memory_order_acq_rel) + 1;
    if(queued >= flush_threshold_) {
        impl_->flush_requested.store(true, std::memory_order_release);
        if(impl_->worker_started) {
            impl_->cv.notify_one();
        } else {
            flush_pending();
        }
    }

    errno = saved_errno;
}

void
MalleabilityLoggerBase::flush() noexcept {
    flush_pending();
}

void
MalleabilityLoggerBase::enable() noexcept {
    if(!impl_->stopped.load(std::memory_order_acquire)) {
        impl_->enabled.store(true, std::memory_order_release);
    }
}

void
MalleabilityLoggerBase::disable() noexcept {
    impl_->enabled.store(false, std::memory_order_release);
    flush_pending();
}

bool
MalleabilityLoggerBase::enabled() const noexcept {
    return impl_->enabled.load(std::memory_order_acquire) &&
           !impl_->stopped.load(std::memory_order_acquire);
}

void
MalleabilityLoggerBase::shutdown() noexcept {
    if(impl_->stopped.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    impl_->enabled.store(false, std::memory_order_release);
    impl_->shutdown_requested.store(true, std::memory_order_release);
    impl_->flush_requested.store(true, std::memory_order_release);
    impl_->cv.notify_one();
    if(impl_->worker.joinable()) {
        impl_->worker.join();
    } else {
        flush_pending();
    }
}

void
MalleabilityLoggerBase::run() noexcept {
    while(!impl_->shutdown_requested.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> lock(impl_->cv_mutex);
        impl_->cv.wait(lock, [this] {
            return impl_->flush_requested.load(std::memory_order_acquire) ||
                   impl_->shutdown_requested.load(std::memory_order_acquire);
        });
        impl_->flush_requested.store(false, std::memory_order_release);
        lock.unlock();
        flush_pending();
    }
    flush_pending();
}

void
MalleabilityLoggerBase::delete_list(LogNode* node) noexcept {
    while(node != nullptr) {
        auto* next = node->next;
        delete node;
        node = next;
    }
}

bool
MalleabilityLoggerBase::ensure_cached_dir(const std::string& path) {
    if(impl_->ready_dirs.find(path) != impl_->ready_dirs.end()) {
        return true;
    }
    if(!ensure_dir(path)) {
        return false;
    }
    impl_->ready_dirs.insert(path);
    return true;
}

void
MalleabilityLoggerBase::append_batch(const std::string& log_path,
                                     const std::string& batch) {
    const auto fd = open_append(log_path);
    if(fd < 0) {
        return;
    }
    write_all(fd, batch);
    close_fd(fd);
}

void
MalleabilityLoggerBase::flush_pending() noexcept {
    const auto saved_errno = errno;
    const std::lock_guard<std::mutex> guard(impl_->flush_mutex);
    LogNode* list = nullptr;
    LogNode* ordered = nullptr;
    try {
        impl_->pending_count.exchange(0, std::memory_order_acq_rel);
        list = impl_->pending_head.exchange(nullptr, std::memory_order_acq_rel);
        if(list == nullptr) {
            errno = saved_errno;
            return;
        }

        while(list != nullptr) {
            auto* next = list->next;
            list->next = ordered;
            ordered = list;
            list = next;
        }

        if(!ensure_cached_dir(root_dir_)) {
            delete_list(ordered);
            errno = saved_errno;
            return;
        }

        std::string current_path;
        std::ostringstream batch;
        bool has_batch = false;

        for(auto* node = ordered; node != nullptr; node = node->next) {
            if(use_unique_id_dir_) {
                const auto unique_dir = root_dir_ + "/" + node->entry.unique_id;
                if(!ensure_cached_dir(unique_dir)) {
                    continue;
                }
            }

            const auto next_path = log_file_path(node->entry);
            if(has_batch && next_path != current_path) {
                append_batch(current_path, batch.str());
                batch.str("");
                batch.clear();
                has_batch = false;
            }

            current_path = next_path;
            has_batch = true;
            batch << format_entry(node->entry, hostname_);
        }

        if(has_batch) {
            append_batch(current_path, batch.str());
        }

        delete_list(ordered);
        ordered = nullptr;
    } catch(...) {
        delete_list(list);
        delete_list(ordered);
    }
    errno = saved_errno;
}

void
MalleabilityLoggerBase::discard_pending() noexcept {
    auto* list = impl_->pending_head.exchange(nullptr, std::memory_order_acq_rel);
    impl_->pending_count.store(0, std::memory_order_release);
    delete_list(list);
}

std::string
MalleabilityLoggerBase::log_file_path(const LogEntry& entry) const {
    const auto log_file =
            log_file_prefix_ + std::to_string(entry.process_id) +
            log_file_suffix_;
    if(use_unique_id_dir_) {
        return root_dir_ + "/" + entry.unique_id + "/" + log_file;
    }
    return root_dir_ + "/" + log_file;
}

} // namespace gkfs::malleability::common
