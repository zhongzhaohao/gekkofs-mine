/*
 * (C) 2015 The University of Chicago
 *
 * See COPYRIGHT in top-level directory.
 */

#ifndef GEKKOFS_REGISTRY_RPC_REGISTRATION_HPP
#define GEKKOFS_REGISTRY_RPC_REGISTRATION_HPP

#include <margo.h>

namespace gkfs::registry {

struct client_rpc_ids {
    hg_id_t daemon_update_epoch_id;
};

void
register_server_rpcs(margo_instance_id mid);

client_rpc_ids
register_client_rpcs(margo_instance_id mid);

} // namespace gkfs::registry

#endif // GEKKOFS_REGISTRY_RPC_REGISTRATION_HPP
