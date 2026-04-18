/*
  Copyright 2018-2024, Barcelona Supercomputing Center (BSC), Spain
  Copyright 2015-2024, Johannes Gutenberg Universitaet Mainz, Germany

  This file is part of GekkoFS.

  SPDX-License-Identifier: GPL-3.0-or-later
*/

#ifndef GEKKOFS_DAEMON_MALLEABILITY_LOGGER_HPP
#define GEKKOFS_DAEMON_MALLEABILITY_LOGGER_HPP

#include <cstdint>
#include <cstddef>
#include <string>

extern "C" {
#include <sys/types.h>
}

namespace gkfs::daemon::malleability {

struct PendingIoLog {
    std::string operation;
    std::string file;
    std::string unique_id;
    off64_t offset;
    std::size_t requested_size;
    std::int64_t start_time_ns;
};

PendingIoLog
start_io(const char* operation, const std::string& file, off64_t offset,
         std::size_t requested_size, const std::string& unique_id);

void
finish_io(PendingIoLog entry, off64_t final_offset, ssize_t actual_size,
          int err, std::uint64_t processed_chunks);

void
log_epoch_update(const std::string& action, std::uint64_t previous_epoch,
                 std::uint64_t current_epoch, const std::string& hostfile,
                 const std::string& unique_id, int err);

} // namespace gkfs::daemon::malleability

#endif // GEKKOFS_DAEMON_MALLEABILITY_LOGGER_HPP
