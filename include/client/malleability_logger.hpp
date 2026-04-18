/*
  Copyright 2018-2024, Barcelona Supercomputing Center (BSC), Spain
  Copyright 2015-2024, Johannes Gutenberg Universitaet Mainz, Germany

  This file is part of GekkoFS' POSIX interface.

  SPDX-License-Identifier: LGPL-3.0-or-later
*/

#ifndef GEKKOFS_MALLEABILITY_LOGGER_HPP
#define GEKKOFS_MALLEABILITY_LOGGER_HPP

#include <cstdint>
#include <cstddef>
#include <string>

extern "C" {
#include <sys/types.h>
}

namespace gkfs::malleability {

struct PendingIoLog {
    std::string operation;
    std::string file;
    off64_t offset;
    std::size_t requested_size;
    std::int64_t start_time_ns;
};

PendingIoLog
start_io(const char* operation, const std::string& file, off64_t offset,
         std::size_t requested_size);

void
finish_io(PendingIoLog entry, off64_t final_offset, ssize_t actual_size,
          int err);

void
flush();

void
shutdown();

void
log_epoch_change(std::uint64_t previous_epoch, std::uint64_t current_epoch);

} // namespace gkfs::malleability

#endif // GEKKOFS_MALLEABILITY_LOGGER_HPP
