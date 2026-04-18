#ifndef GEKKOFS_COMMON_HOSTFILE_HPP
#define GEKKOFS_COMMON_HOSTFILE_HPP

#include <cstdint>
#include <string>

namespace gkfs::utils {

inline std::string
get_epoch_hostfile_name(const std::string& base_hostfile,
                        const std::string& unique_id, std::uint64_t epoch) {
    if(epoch == 0) {
        return base_hostfile;
    }

    return base_hostfile + "_" + unique_id + "_" + std::to_string(epoch);
}

} // namespace gkfs::utils

#endif // GEKKOFS_COMMON_HOSTFILE_HPP
