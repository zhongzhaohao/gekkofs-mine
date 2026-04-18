/*
 * (C) 2015 The University of Chicago
 *
 * See COPYRIGHT in top-level directory.
 */

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include <CLI/CLI.hpp>
#include <fmt/format.h>
#include <mercury.h>
#include <margo.h>

#include <common/common_defs.hpp>
#include <common/env_util.hpp>
#include <common/rpc/rpc_types.hpp>

#include <registry/env.hpp>
#include <registry/mallea_scheduler.hpp>
#include <registry/rpc_registration.hpp>

using namespace std;

namespace {

struct registry_options {
    string rpc_protocol;
    string listen;
};

string
resolve_rpc_protocol(const CLI::App& desc, const registry_options& opts) {
    auto rpc_protocol = string(gkfs::rpc::protocol::ofi_sockets);

    if(desc.count("--protocol")) {
        rpc_protocol = opts.rpc_protocol;
    }

    if(rpc_protocol != gkfs::rpc::protocol::ofi_verbs &&
       rpc_protocol != gkfs::rpc::protocol::ofi_sockets &&
       rpc_protocol != gkfs::rpc::protocol::ofi_psm2 &&
       rpc_protocol != gkfs::rpc::protocol::na_ucx) {
        throw runtime_error(fmt::format("Given RPC protocol '{}' not supported. ",
                                        rpc_protocol));
    }

    return rpc_protocol;
}

string
make_server_bind_address(const CLI::App& desc, const registry_options& opts,
                         const string& rpc_protocol) {
    string bind;
    if(desc.count("--listen")) {
        bind = "://" + opts.listen;
    }

    return rpc_protocol + bind;
}

margo_instance_id
init_margo_instance(const string& address, int mode, const string& error_msg) {
    char starter_json[] = "{\"output_dir\":\"/tmp\"}";
    struct margo_init_info args = {nullptr};
    args.json_config = starter_json;

    auto mid = margo_init_ext(address.c_str(), mode, &args);
    if(mid == MARGO_INSTANCE_NULL) {
        throw runtime_error(error_msg);
    }

    return mid;
}

void
write_registry_address(const string& registry_file,
                       const char* addr_self_string) {
    ofstream lf(registry_file, ios::out);
    if(!lf) {
        throw runtime_error(fmt::format("Failed to open hosts file '{}': {}",
                                        registry_file, strerror(errno)));
    }

    lf << addr_self_string << endl;
    if(!lf) {
        throw runtime_error(
                fmt::format("Failed to write on hosts file '{}': {}",
                            registry_file, strerror(errno)));
    }
    lf.close();
}

} // namespace

int
main(int argc, char** argv) {
    CLI::App desc{"Allowed options"};
    registry_options opts{};

    desc.add_option("--protocol,-P", opts.rpc_protocol, "RPC protocol to use.");
    desc.add_option("--listen,-l", opts.listen, "Address to listen");

    try {
        desc.parse(argc, argv);
    } catch(const CLI::ParseError& e) {
        return desc.exit(e);
    }

    auto server_mid = MARGO_INSTANCE_NULL;
    auto client_mid = MARGO_INSTANCE_NULL;

    try {
        const auto registry_file = gkfs::env::get_var(
                gkfs::env::REGISTRY_FILE, gkfs::config::registryfile_path);
        const auto rpc_protocol = resolve_rpc_protocol(desc, opts);
        const auto bind =
                make_server_bind_address(desc, opts, rpc_protocol);

        server_mid = init_margo_instance(bind, MARGO_SERVER_MODE,
                                         "Error: margo_init_ext() server");

        hg_addr_t addr_self;
        auto hret = margo_addr_self(server_mid, &addr_self);
        if(hret != HG_SUCCESS) {
            throw runtime_error("Error: margo_addr_self()");
        }

        char addr_self_string[128];
        hg_size_t addr_self_string_sz = 128;
        hret = margo_addr_to_string(server_mid, addr_self_string,
                                    &addr_self_string_sz, addr_self);
        if(hret != HG_SUCCESS) {
            margo_addr_free(server_mid, addr_self);
            throw runtime_error("Error: margo_addr_to_string()");
        }
        margo_addr_free(server_mid, addr_self);

        write_registry_address(registry_file, addr_self_string);
        fprintf(stderr, "# bind \"%s\"\n", bind.c_str());
        fprintf(stderr, "# accepting RPCs on address \"%s\"\n",
                addr_self_string);

        gkfs::registry::register_server_rpcs(server_mid);

        client_mid = init_margo_instance(rpc_protocol, MARGO_CLIENT_MODE,
                                         "Error: margo_init_ext() client");
        const auto client_rpc_ids =
                gkfs::registry::register_client_rpcs(client_mid);

        {
            gkfs::registry::MalleaScheduler mallea_scheduler(
                    client_mid, client_rpc_ids);

            margo_wait_for_finalize(server_mid);
            server_mid = MARGO_INSTANCE_NULL;
            mallea_scheduler.stop();
        }

        margo_finalize(client_mid);
        client_mid = MARGO_INSTANCE_NULL;
    } catch(...) {
        if(client_mid != MARGO_INSTANCE_NULL) {
            margo_finalize(client_mid);
        }
        if(server_mid != MARGO_INSTANCE_NULL) {
            margo_finalize(server_mid);
        }
        throw;
    }

    return 0;
}
