#include <cerrno>
#include <cstdint>
#include <string>

#include <registry/my-rpc.hpp>

#include <common/common_defs.hpp>
#include <common/rpc/rpc_types.hpp>

int
forward_daemon_update_epoch(margo_instance_id client_mid, hg_id_t rpc_id,
                            const std::string& daemon_addr,
                            gkfs::rpc::daemon_resize_action action,
                            std::uint64_t epoch,
                            const std::string& hostfile,
                            const std::string& unique_id) {
    if(client_mid == MARGO_INSTANCE_NULL || rpc_id == 0) {
        return EINVAL;
    }

    if(daemon_addr.empty() || hostfile.empty() || unique_id.empty()) {
        return EINVAL;
    }

    hg_addr_t daemon_hg_addr = HG_ADDR_NULL;
    hg_handle_t handle = HG_HANDLE_NULL;
    rpc_daemon_update_epoch_in_t in{};
    rpc_err_out_t out{};

    auto ret = margo_addr_lookup(client_mid, daemon_addr.c_str(),
                                 &daemon_hg_addr);
    if(ret != HG_SUCCESS) {
        return EBUSY;
    }

    ret = margo_create(client_mid, daemon_hg_addr, rpc_id, &handle);
    if(ret != HG_SUCCESS) {
        margo_addr_free(client_mid, daemon_hg_addr);
        return EBUSY;
    }

    in.action = static_cast<hg_int32_t>(action);
    in.epoch = epoch;
    in.hostfile = hostfile.c_str();
    in.unique_id = unique_id.c_str();

    ret = margo_forward(handle, &in);
    if(ret != HG_SUCCESS) {
        margo_destroy(handle);
        margo_addr_free(client_mid, daemon_hg_addr);
        return EBUSY;
    }

    ret = margo_get_output(handle, &out);
    if(ret != HG_SUCCESS) {
        margo_destroy(handle);
        margo_addr_free(client_mid, daemon_hg_addr);
        return EBUSY;
    }

    auto err = out.err;
    margo_free_output(handle, &out);
    margo_destroy(handle);
    margo_addr_free(client_mid, daemon_hg_addr);
    return err;
}
