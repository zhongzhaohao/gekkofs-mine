#ifndef GEKKOFS_COMMON_FILE_LAYOUT_HPP
#define GEKKOFS_COMMON_FILE_LAYOUT_HPP

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace gkfs::file_layout {

using chunk_id_t = std::uint64_t;
using epoch_t = std::uint64_t;

struct FileLayoutEntry {
    chunk_id_t start_chunk;
    epoch_t epoch;
};

struct FileLayout {
    std::vector<FileLayoutEntry> entries;

    [[nodiscard]] bool
    empty() const {
        return entries.empty();
    }

    [[nodiscard]] epoch_t
    get_epoch(chunk_id_t chunk_id) const {
        if(entries.empty()) {
            return 0;
        }

        auto it = std::upper_bound(
                entries.begin(), entries.end(), chunk_id,
                [](chunk_id_t id, const FileLayoutEntry& entry) {
                    return id < entry.start_chunk;
                });

        if(it == entries.begin()) {
            return 0;
        }

        --it;
        return it->epoch;
    }

    [[nodiscard]] std::string
    serialize() const {
        std::ostringstream oss;
        for(std::size_t i = 0; i < entries.size(); ++i) {
            if(i != 0) {
                oss << ';';
            }
            oss << entries[i].start_chunk << ':' << entries[i].epoch;
        }
        return oss.str();
    }

    static FileLayout
    deserialize(std::string_view serialized) {
        FileLayout layout;

        if(serialized.empty()) {
            return layout;
        }

        std::string data{serialized};
        std::stringstream ss(data);
        std::string token;
        chunk_id_t previous_start = 0;
        bool first = true;

        while(std::getline(ss, token, ';')) {
            if(token.empty()) {
                throw std::invalid_argument("empty file layout entry");
            }

            const auto sep = token.find(':');
            if(sep == std::string::npos) {
                throw std::invalid_argument("invalid file layout entry");
            }

            const auto start = static_cast<chunk_id_t>(
                    std::stoull(token.substr(0, sep)));
            const auto epoch = static_cast<epoch_t>(
                    std::stoull(token.substr(sep + 1)));

            if(first) {
                if(start != 0) {
                    throw std::invalid_argument(
                            "file layout must start at chunk 0");
                }
                first = false;
            } else if(start <= previous_start) {
                throw std::invalid_argument(
                        "file layout entries must be strictly increasing");
            }

            layout.entries.push_back({start, epoch});
            previous_start = start;
        }

        return layout;
    }

    void
    append(chunk_id_t start_chunk, epoch_t epoch) {
        if(entries.empty()) {
            if(start_chunk != 0) {
                throw std::invalid_argument(
                        "file layout must start at chunk 0");
            }
            entries.push_back({start_chunk, epoch});
            return;
        }

        if(start_chunk < entries.back().start_chunk) {
            throw std::invalid_argument(
                    "file layout append must be monotonically increasing");
        }

        if(start_chunk == entries.back().start_chunk) {
            entries.back().epoch = epoch;
            return;
        }

        if(entries.back().epoch == epoch) {
            return;
        }

        entries.push_back({start_chunk, epoch});
    }

    bool
    truncate_from(chunk_id_t first_removed_chunk) {
        if(entries.empty()) {
            return false;
        }

        auto first_removed = std::lower_bound(
                entries.begin(), entries.end(), first_removed_chunk,
                [](const FileLayoutEntry& entry, chunk_id_t chunk) {
                    return entry.start_chunk < chunk;
                });

        if(first_removed == entries.begin()) {
            ++first_removed;
        }

        if(first_removed == entries.end()) {
            return false;
        }

        entries.erase(first_removed, entries.end());
        return true;
    }
};

struct FileLayoutSnapshot {
    FileLayout layout;
    epoch_t latest_version_epoch = 0;
    std::string serialized_layout;

    [[nodiscard]] bool
    empty() const {
        return layout.empty();
    }
};

using FileLayoutSnapshotPtr = std::shared_ptr<const FileLayoutSnapshot>;

struct FileLayoutRecord {
    FileLayoutSnapshotPtr snapshot;
    mutable std::mutex mutex;
};

using FileLayoutRecordPtr = std::shared_ptr<FileLayoutRecord>;
using FileLayoutMap = std::map<std::string, FileLayoutRecordPtr>;

inline FileLayoutSnapshotPtr
make_file_layout_snapshot(epoch_t latest_version_epoch, epoch_t init_epoch) {
    auto snapshot = std::make_shared<FileLayoutSnapshot>();
    snapshot->latest_version_epoch = latest_version_epoch;
    snapshot->layout.append(0, init_epoch);
    snapshot->serialized_layout = snapshot->layout.serialize();
    return snapshot;
}

inline FileLayoutSnapshotPtr
make_file_layout_snapshot(epoch_t latest_version_epoch,
                          std::string_view serialized_layout) {
    auto snapshot = std::make_shared<FileLayoutSnapshot>();
    snapshot->latest_version_epoch = latest_version_epoch;
    snapshot->serialized_layout = std::string(serialized_layout);
    snapshot->layout = FileLayout::deserialize(serialized_layout);
    return snapshot;
}

inline FileLayoutRecordPtr
make_file_layout_record(epoch_t latest_version_epoch, epoch_t init_epoch) {
    auto record = std::make_shared<FileLayoutRecord>();
    auto snapshot = make_file_layout_snapshot(latest_version_epoch, init_epoch);
    std::atomic_store_explicit(&record->snapshot, snapshot,
                               std::memory_order_release);
    return record;
}

inline FileLayoutRecordPtr
make_file_layout_record(epoch_t latest_version_epoch,
                        std::string_view serialized_layout) {
    auto record = std::make_shared<FileLayoutRecord>();
    auto snapshot =
            make_file_layout_snapshot(latest_version_epoch, serialized_layout);
    std::atomic_store_explicit(&record->snapshot, snapshot,
                               std::memory_order_release);
    return record;
}

inline FileLayoutSnapshotPtr
load_file_layout_snapshot(const FileLayoutRecordPtr& record) {
    if(!record) {
        return {};
    }
    return std::atomic_load_explicit(&record->snapshot,
                                     std::memory_order_acquire);
}

inline epoch_t
file_layout_latest_version_epoch(const FileLayoutRecordPtr& record) {
    auto snapshot = load_file_layout_snapshot(record);
    if(!snapshot) {
        return 0;
    }
    return snapshot->latest_version_epoch;
}

inline void
publish_file_layout_snapshot_if_newer(FileLayoutRecordPtr& record,
                                      epoch_t latest_version_epoch,
                                      std::string_view serialized_layout) {
    if(!record) {
        record = std::make_shared<FileLayoutRecord>();
    }

    auto next =
            make_file_layout_snapshot(latest_version_epoch, serialized_layout);
    auto current = load_file_layout_snapshot(record);
    while(!current ||
          latest_version_epoch > current->latest_version_epoch) {
        if(std::atomic_compare_exchange_weak_explicit(
                   &record->snapshot, &current, next,
                   std::memory_order_release, std::memory_order_acquire)) {
            return;
        }
    }
}

} // namespace gkfs::file_layout

#endif // GEKKOFS_COMMON_FILE_LAYOUT_HPP
