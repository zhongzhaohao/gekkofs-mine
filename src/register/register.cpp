/*
  Copyright 2018-2024, Barcelona Supercomputing Center (BSC), Spain
  Copyright 2015-2024, Johannes Gutenberg Universitaet Mainz, Germany
*/

#include <client/preload.hpp>
#include <client/logging.hpp>
#include <client/preload_util.hpp>
#include <register/register_util.hpp>
#include <CLI/CLI.hpp>

#include <cstring>
#include <iostream>
#include <system_error>

using namespace std;

int
main(int argc, char* argv[]) {
    string workflow;
    string hostfile;
    string hostconfigfile;
    string mergeflows;
    string registry_addr;
    bool do_request = false;
    bool do_register = false;

    gkfs::registers::read_env(workflow, hostfile, hostconfigfile, mergeflows);

    CLI::App app{"Register a GekkoFS client instance with the registry"};
    app.add_flag("--request", do_request,
                 "Request the registry to generate merged host files");
    app.add_flag("--register", do_register,
                 "Register the current client instance in the registry");
    app.add_option("-w,--workflow", workflow,
                   "Workflow name registered in the registry");
    app.add_option("--hostfile", hostfile,
                   "Path to the client hostfile");
    app.add_option("--hostconfigfile", hostconfigfile,
                   "Path to the client host configuration file");

    try {
        app.parse(argc, argv);
    } catch(const CLI::ParseError& e) {
        return app.exit(e);
    }

    if(!do_request && !do_register) {
        do_request = true;
        do_register = true;
    }

    if(workflow.empty() || hostfile.empty() || hostconfigfile.empty()) {
        cerr << "workflow, hostfile, and hostconfigfile must be provided"
             << endl;
        return 1;
    }

    try {
        registry_addr = gkfs::registers::read_registry_file();
    } catch(const std::exception& ex) {
        cerr << "failed to read registry address: " << ex.what() << endl;
        return 1;
    }

    if(!gkfs::registers::init_registry_client()) {
        return 1;
    }

    if(!gkfs::registers::connect_registry(registry_addr)) {
        ld_network_service.reset();
        return 1;
    }

    if(do_request) {
        const auto err = gkfs::registers::request_registry();
        if(err != 0) {
            std::error_code ec(err, std::system_category());
            cerr << "registry request failed: "
                 << (ec ? ec.message() : std::strerror(err)) << endl;
            ld_network_service.reset();
            return 1;
        }
    }

    if(do_register) {
        const auto err = gkfs::registers::register_registry(workflow, hostconfigfile, hostfile);
        if(err != 0) {
            std::error_code ec(err, std::system_category());
            cerr << "registry registration failed: "
                 << (ec ? ec.message() : std::strerror(err)) << endl;
            ld_network_service.reset();
            return 1;
        }
    }

    ld_network_service.reset();
    return 0;
}