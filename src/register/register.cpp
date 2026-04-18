/*
  Copyright 2018-2024, Barcelona Supercomputing Center (BSC), Spain
  Copyright 2015-2024, Johannes Gutenberg Universitaet Mainz, Germany
*/

#include <client/preload.hpp>
#include <client/logging.hpp>
#include <client/preload_util.hpp>
#include <register/register_util.hpp>
#include <CLI/CLI.hpp>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <system_error>

using namespace std;

namespace {

bool
confirm_overwrite(const std::string& unique_id) {
    std::cout << "unique_id '" << unique_id
              << "' already exists in registry. Overwrite? [y/N]: "
              << std::flush;

    std::string answer;
    if(!std::getline(std::cin, answer)) {
        return false;
    }

    return answer == "y" || answer == "Y" || answer == "yes" ||
           answer == "YES";
}

} // namespace

int
main(int argc, char* argv[]) {
    string workflow;
    string hostfile;
    string hostconfigfile;
    string mergeflows;
    string registry_addr;
    string unique_id;
    string username;
    string exec_app_path;
    string paras;
    uint32_t nodes = 0;
    uint32_t ppn = 0;
    string output_path;
    bool do_request = false;
    bool do_register = false;
    bool do_register_mallea = false;
    bool do_query_mallea = false;
    bool do_unregister_mallea = false;
    bool force_mallea = false;

    gkfs::registers::read_env(workflow, hostfile, hostconfigfile, mergeflows);
    if(const char* user_env = std::getenv("USER"); user_env != nullptr) {
        username = user_env;
    }

    CLI::App app{"Register a GekkoFS client instance with the registry"};
    app.add_flag("--request", do_request,
                 "Request the registry to generate merged host files");
    app.add_flag("--register", do_register,
                 "Register the current client instance in the registry");
    app.add_flag("--register-mallea", do_register_mallea,
                 "Send the register info to the registry for malleability support");
    app.add_flag("--query-mallea", do_query_mallea,
                 "Query all registered malleability jobs and write them to --output");
    app.add_flag("--unregister-mallea", do_unregister_mallea,
                 "Remove a registered malleability job by --unique-id");
    app.add_flag("--force", force_mallea,
                 "Force overwrite when registering malleability info");
    app.add_option("-w,--workflow", workflow,
                   "Workflow name registered in the registry");
    app.add_option("--unique-id", unique_id,
                   "Unique id sent by registry_register_mallea; a Slurm job id is fine as long as it is unique");
    app.add_option("--hostfile", hostfile,
                   "Path to the client hostfile");
    app.add_option("--hostconfigfile", hostconfigfile,
                   "Path to the client host configuration file");
    app.add_option("--username", username,
                   "Username sent by registry_register_mallea");
    app.add_option("--exec-app-path", exec_app_path,
                   "Executable path sent by registry_register_mallea");
    app.add_option("--paras", paras,
                   "Execution parameters sent by registry_register_mallea");
    app.add_option("--nodes", nodes,
                   "Node count sent by registry_register_mallea");
    app.add_option("--ppn", ppn,
                   "Processes per node sent by registry_register_mallea");
    app.add_option("-o,--output", output_path,
                   "Output file used by --query-mallea");

    try {
        app.parse(argc, argv);
    } catch(const CLI::ParseError& e) {
        return app.exit(e);
    }

    if(!do_request && !do_register && !do_register_mallea && !do_query_mallea &&
       !do_unregister_mallea) {
        do_request = true;
        do_register = true;
    }

    if((do_request || do_register) &&
       (workflow.empty() || hostfile.empty() || hostconfigfile.empty())) {
        cerr << "workflow, hostfile, and hostconfigfile must be provided"
             << endl;
        return 1;
    }

    if(do_register_mallea &&
       (unique_id.empty() || hostfile.empty() || hostconfigfile.empty() ||
        nodes == 0 || ppn == 0)) {
        cerr << "unique-id, hostfile, hostconfigfile, nodes, and ppn must be provided for --register-mallea"
             << endl;
        return 1;
    }

    if(do_query_mallea && output_path.empty()) {
        cerr << "--output must be provided for --query-mallea" << endl;
        return 1;
    }

    if(do_unregister_mallea && unique_id.empty()) {
        cerr << "--unique-id must be provided for --unregister-mallea" << endl;
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

    if(do_register_mallea) {
        auto err = gkfs::registers::register_registry_mallea(
                unique_id, username, exec_app_path, paras, hostconfigfile,
                hostfile, nodes, ppn, force_mallea);
        if(err == EEXIST && !force_mallea) {
            if(confirm_overwrite(unique_id)) {
                err = gkfs::registers::register_registry_mallea(
                        unique_id, username, exec_app_path, paras,
                        hostconfigfile, hostfile, nodes, ppn, true);
            } else {
                cerr << "registry mallea registration canceled" << endl;
                ld_network_service.reset();
                return 1;
            }
        }
        if(err != 0) {
            std::error_code ec(err, std::system_category());
            cerr << "registry mallea registration failed: "
                 << (ec ? ec.message() : std::strerror(err)) << endl;
            ld_network_service.reset();
            return 1;
        }
    }

    if(do_query_mallea) {
        const auto err = gkfs::registers::query_registry_mallea(output_path);
        if(err != 0) {
            std::error_code ec(err, std::system_category());
            cerr << "registry mallea query failed: "
                 << (ec ? ec.message() : std::strerror(err)) << endl;
            ld_network_service.reset();
            return 1;
        }
    }

    if(do_unregister_mallea) {
        const auto err =
                gkfs::registers::unregister_registry_mallea(unique_id);
        if(err != 0) {
            std::error_code ec(err, std::system_category());
            cerr << "registry mallea unregister failed: "
                 << (ec ? ec.message() : std::strerror(err)) << endl;
            ld_network_service.reset();
            return 1;
        }
    }

    ld_network_service.reset();
    return 0;
}
