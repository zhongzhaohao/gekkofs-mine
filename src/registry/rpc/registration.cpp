#include <registry/rpc_registration.hpp>

#include <registry/my-rpc.hpp>

#include <common/rpc/rpc_types.hpp>

namespace gkfs::registry {

void
register_server_rpcs(margo_instance_id mid) {
    MARGO_REGISTER(mid, gkfs::rpc::tag::registry_request,
                   rpc_registry_request_in_t, rpc_registry_request_out_t,
                   rpc_srv_registry_request);
    MARGO_REGISTER(mid, gkfs::rpc::tag::registry_register,
                   rpc_registry_register_in_t, rpc_err_out_t,
                   rpc_srv_registry_register);
    MARGO_REGISTER(mid, gkfs::rpc::tag::registry_register_mallea,
                   rpc_registry_register_mallea_in_t, rpc_err_out_t,
                   rpc_srv_registry_register_mallea);
    MARGO_REGISTER(mid, gkfs::rpc::tag::registry_query_mallea,
                   rpc_registry_query_mallea_in_t,
                   rpc_registry_query_mallea_out_t,
                   rpc_srv_registry_query_mallea);
    MARGO_REGISTER(mid, gkfs::rpc::tag::registry_unregister_mallea,
                   rpc_registry_unregister_mallea_in_t, rpc_err_out_t,
                   rpc_srv_registry_unregister_mallea);
}

client_rpc_ids
register_client_rpcs(margo_instance_id mid) {
    client_rpc_ids ids{};
    ids.daemon_update_epoch_id =
            MARGO_REGISTER(mid, gkfs::rpc::tag::daemon_update_epoch,
                           rpc_daemon_update_epoch_in_t, rpc_err_out_t, NULL);
    return ids;
}

} // namespace gkfs::registry
