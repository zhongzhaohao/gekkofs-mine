#include <register/register_util.hpp>

#include <client/env.hpp>
#include <client/preload.hpp>
#include <client/preload_util.hpp>
#include <client/rpc/rpc_types.hpp>
#include <common/env_util.hpp>

#include <cerrno>
#include <fstream>
#include <memory>
#include <random>
#include <thread>

#include <hermes.hpp>

namespace gkfs::registers {

namespace {

void
extract_protocol(const std::string& uri) {
    if(uri.rfind("://") == std::string::npos) {
        throw std::runtime_error(
                fmt::format("Invalid format for URI: '{}'", uri));
    }

    std::string protocol{};

    if(uri.find(gkfs::rpc::protocol::ofi_sockets) != std::string::npos) {
        protocol = gkfs::rpc::protocol::ofi_sockets;
    } else if(uri.find(gkfs::rpc::protocol::ofi_psm2) != std::string::npos) {
        protocol = gkfs::rpc::protocol::ofi_psm2;
    } else if(uri.find(gkfs::rpc::protocol::ofi_verbs) != std::string::npos) {
        protocol = gkfs::rpc::protocol::ofi_verbs;
    } else if(uri.find(gkfs::rpc::protocol::na_ucx) != std::string::npos) {
        protocol = gkfs::rpc::protocol::na_ucx;
    }

    if(uri.find(gkfs::rpc::protocol::na_sm) != std::string::npos) {
        if(protocol.empty()) {
            protocol = gkfs::rpc::protocol::na_sm;
        } else {
            CTX->auto_sm(true);
        }
    }

    if(protocol.empty()) {
        throw std::runtime_error(
                fmt::format("Unsupported RPC protocol found in URI '{}'", uri));
    }

    CTX->rpc_protocol(protocol);
}

hermes::endpoint
lookup_endpoint(const std::string& uri, std::size_t max_retries = 3) {
    std::random_device rd;
    std::size_t attempts = 0;

    do {
        try {
            return ld_network_service->lookup(uri);
        } catch(const std::exception&) {
            std::mt19937 g(rd());
            std::uniform_int_distribution<> distr(50, 50 * (attempts + 2));
            std::this_thread::sleep_for(std::chrono::milliseconds(distr(g)));
            continue;
        }
    } while(++attempts < max_retries);

    throw std::runtime_error("registry endpoint lookup failed");
}

} // namespace

void
read_env(std::string& workflow, std::string& hostfile,
         std::string& hostconfigfile, std::string& mergeflows) {
    hostfile = gkfs::env::get_var(gkfs::env::HOSTS_FILE,
                                  gkfs::config::hostfile_path);
    hostconfigfile = gkfs::env::get_var(gkfs::env::HOSTS_CONFIG_FILE,
                                        gkfs::config::hostfile_config_path);
    workflow = gkfs::env::get_var(gkfs::env::WORK_FLOW, "default_job");
    mergeflows = gkfs::env::get_var(gkfs::env::MERGE_FLOWS, "");
}

std::string
read_registry_file() {
    std::string registryfile;
    registryfile = gkfs::env::get_var(gkfs::env::REGISTRY_FILE,
                                      gkfs::config::registryfile_path);

    std::ifstream lf(registryfile);
    std::string addr;
    getline(lf, addr);
    if(addr.empty()) {
        throw std::runtime_error(
                fmt::format("Registryfile empty: '{}'", registryfile));
    }

    extract_protocol(addr);
    return addr;
}

bool
init_registry_client() {
    try {
        hermes::engine_options opts{};

        if(CTX->auto_sm()) {
            opts |= hermes::use_auto_sm;
        }
        if(gkfs::rpc::protocol::ofi_psm2 == CTX->rpc_protocol()) {
            opts |= hermes::force_no_block_progress;
        }

        opts |= hermes::process_may_fork;
        ld_network_service = std::make_unique<hermes::async_engine>(
                hermes::get_transport_type(CTX->rpc_protocol()), opts);
        ld_network_service->run();
    } catch(const std::exception&) {
        return false;
    }

    return true;
}

bool
connect_registry(const std::string& registry_addr) {
    try {
        auto endp = lookup_endpoint(registry_addr);
        CTX->registry(endp);
    } catch(const std::exception&) {
        return false;
    }

    return true;
}

int
request_registry() {
    std::string mergeflows;
    std::string hostfile;
    std::string hostconfigfile;
    std::string workflow;

    read_env(workflow, hostfile, hostconfigfile, mergeflows);

    auto endp = CTX->registry();
    gkfs::rpc::registry_request::input in(mergeflows, hostconfigfile, hostfile,
                                          workflow);

    try {
        auto out =
                ld_network_service->post<gkfs::rpc::registry_request>(endp, in)
                        .get()
                        .at(0);
        return out.err() ? out.err() : 0;
    } catch(const std::exception&) {
        return EBUSY;
    }
}

int
register_registry(const std::string& workflow,
                  const std::string& hostconfigfile,
                  const std::string& hostfile) {
    auto endp = CTX->registry();
    gkfs::rpc::registry_register::input in(workflow, hostconfigfile, hostfile);

    try {
        auto out =
                ld_network_service->post<gkfs::rpc::registry_register>(endp, in)
                        .get()
                        .at(0);
        return out.err() ? out.err() : 0;
    } catch(const std::exception&) {
        return EBUSY;
    }
}

} // namespace gkfs::registers