#include <registry/mallea_scheduler.hpp>

#include <registry/my-rpc.hpp>

#include <common/hostfile.hpp>
#include <common/rpc/rpc_types.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <random>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

bool
mallea_has_borrow_relation_locked(const std::string& unique_id) {
    if(mallea_borrow_table.count(unique_id) != 0) {
        return true;
    }
    return std::any_of(mallea_borrow_table.begin(), mallea_borrow_table.end(),
                       [&unique_id](const auto& entry) {
                           return entry.second == unique_id;
                       });
}

std::vector<std::string>
read_daemon_addresses(const std::string& hostfile) {
    std::ifstream host_stream(hostfile);
    std::vector<std::string> addrs;
    std::string line;

    while(std::getline(host_stream, line)) {
        if(line.empty()) {
            continue;
        }

        const auto pos = line.find_first_of(" \t");
        if(pos == std::string::npos) {
            addrs.push_back(line);
            continue;
        }

        const auto addr_begin = line.find_first_not_of(" \t", pos);
        if(addr_begin != std::string::npos) {
            addrs.push_back(line.substr(addr_begin));
        }
    }

    return addrs;
}

int
append_hostfile(std::ofstream& out, const std::string& hostfile,
                std::unordered_set<std::string>* seen = nullptr) {
    std::ifstream in(hostfile);
    if(!in) {
        std::cout << "mallea scheduler failed to open hostfile: " << hostfile
                  << std::endl;
        return EINVAL;
    }

    std::string line;
    while(std::getline(in, line)) {
        if(!line.empty()) {
            if(seen != nullptr && !seen->insert(line).second) {
                continue;
            }
            out << line << '\n';
        }
    }

    if(!out) {
        std::cout << "mallea scheduler failed to append hostfile: " << hostfile
                  << std::endl;
        return EIO;
    }

    return 0;
}

int
write_borrower_hostfile(const std::string& borrower_hostfile,
                        const std::string& borrower_base_hostfile,
                        const std::string& borrower_current_hostfile,
                        const std::string& lender_hostfile) {
    std::ofstream out(borrower_hostfile, std::ios::out | std::ios::trunc);
    if(!out) {
        std::cout << "mallea scheduler failed to create borrower hostfile: "
                  << borrower_hostfile << std::endl;
        return EIO;
    }

    std::unordered_set<std::string> seen;
    auto err = append_hostfile(out, borrower_base_hostfile, &seen);
    if(err != 0) {
        return err;
    }

    if(borrower_current_hostfile != borrower_base_hostfile) {
        err = append_hostfile(out, borrower_current_hostfile, &seen);
        if(err != 0) {
            return err;
        }
    }

    err = append_hostfile(out, lender_hostfile, &seen);
    if(err != 0) {
        return err;
    }

    out.close();
    if(!out) {
        std::cout << "mallea scheduler failed to close borrower hostfile: "
                  << borrower_hostfile << std::endl;
        return EIO;
    }

    return 0;
}

int
write_shrink_hostfile(const std::string& shrink_hostfile,
                      const std::string& borrower_base_hostfile) {
    std::ofstream out(shrink_hostfile, std::ios::out | std::ios::trunc);
    if(!out) {
        std::cout << "mallea scheduler failed to create shrink hostfile: "
                  << shrink_hostfile << std::endl;
        return EIO;
    }

    auto err = append_hostfile(out, borrower_base_hostfile);
    if(err != 0) {
        return err;
    }

    out.close();
    if(!out) {
        std::cout << "mallea scheduler failed to close shrink hostfile: "
                  << shrink_hostfile << std::endl;
        return EIO;
    }

    return 0;
}

std::string
current_hostfile(const std::string& unique_id, const mallea_info& info) {
    return gkfs::utils::get_epoch_hostfile_name(info.hfile, unique_id,
                                                info.epoch);
}

int
notify_daemons(margo_instance_id client_mid,
               const gkfs::registry::client_rpc_ids& client_rpc_ids,
               const std::string& daemon_hostfile,
               gkfs::rpc::daemon_resize_action action, std::uint64_t epoch,
               const std::string& peer_hostfile,
               const std::string& peer_unique_id) {
    const auto daemon_addrs = read_daemon_addresses(daemon_hostfile);
    if(daemon_addrs.empty()) {
        std::cout << "mallea scheduler failed to read daemon hostfile: "
                  << daemon_hostfile << std::endl;
        return EINVAL;
    }

    auto first_err = 0;
    for(const auto& daemon_addr : daemon_addrs) {
        const auto err = forward_daemon_update_epoch(
                client_mid, client_rpc_ids.daemon_update_epoch_id, daemon_addr,
                action, epoch, peer_hostfile, peer_unique_id);
        if(err != 0 && first_err == 0) {
            first_err = err;
        }
        if(err != 0) {
            std::cout << "daemon_update_epoch failed: daemon=" << daemon_addr
                      << ", action=" << gkfs::rpc::to_string(action)
                      << ", err=" << err << std::endl;
        }
    }

    return first_err;
}

void
complete_mallea_resize_locked(const std::string& unique_id) {
    auto it = mallea_jobs.find(unique_id);
    if(it == mallea_jobs.end()) {
        return;
    }

    if(it->second.pending_unregister &&
       !mallea_has_borrow_relation_locked(unique_id)) {
        mallea_jobs.erase(it);
        return;
    }

    it->second.resizing = false;
}

void
run_mallea_expand_scheduler(margo_instance_id client_mid,
                            gkfs::registry::client_rpc_ids client_rpc_ids,
                            std::atomic<bool>& running,
                            std::mutex& wait_mutex,
                            std::condition_variable& wait_cv) {
    std::mt19937 rng(std::random_device{}());
    std::unique_lock<std::mutex> wait_lock(wait_mutex);

    while(running.load()) {
        if(wait_cv.wait_for(wait_lock, std::chrono::seconds(30),
                            [&running] { return !running.load(); })) {
            break;
        }

        std::vector<std::string> candidates;
        {
            std::lock_guard<std::mutex> lock(mallea_jobs_mutex);
            for(const auto& [unique_id, info] : mallea_jobs) {
                if(!info.resizing && !info.pending_unregister &&
                   mallea_borrow_table.count(unique_id) == 0) {
                    candidates.push_back(unique_id);
                }
            }
        }

        if(candidates.size() < 2) {
            continue;
        }

        std::shuffle(candidates.begin(), candidates.end(), rng);
        const auto& first = candidates[0];
        const auto& second = candidates[1];

        std::bernoulli_distribution choose_direction(0.5);
        const bool first_borrows = choose_direction(rng);
        const auto& borrower = first_borrows ? first : second;
        const auto& lender = first_borrows ? second : first;
        mallea_info borrower_info;
        mallea_info lender_info;
        std::uint64_t next_epoch = 0;

        {
            std::lock_guard<std::mutex> lock(mallea_jobs_mutex);
            auto borrower_it = mallea_jobs.find(borrower);
            auto lender_it = mallea_jobs.find(lender);
            if(borrower_it == mallea_jobs.end() ||
               lender_it == mallea_jobs.end() || borrower_it->second.resizing ||
               lender_it->second.resizing ||
               borrower_it->second.pending_unregister ||
               lender_it->second.pending_unregister ||
               mallea_borrow_table.count(borrower) != 0 ||
               mallea_borrow_table.count(lender) != 0) {
                continue;
            }
            borrower_it->second.resizing = true;
            lender_it->second.resizing = true;
            borrower_info = borrower_it->second;
            lender_info = lender_it->second;
            next_epoch = borrower_it->second.epoch + 1;
        }

        const auto borrower_current_hostfile =
                current_hostfile(borrower, borrower_info);
        const auto lender_current_hostfile =
                current_hostfile(lender, lender_info);
        const auto borrower_hostfile = gkfs::utils::get_epoch_hostfile_name(
                borrower_info.hfile, borrower, next_epoch);
        const auto hostfile_err = write_borrower_hostfile(
                borrower_hostfile, borrower_info.hfile,
                borrower_current_hostfile, lender_current_hostfile);
        if(hostfile_err != 0) {
            std::lock_guard<std::mutex> lock(mallea_jobs_mutex);
            complete_mallea_resize_locked(borrower);
            complete_mallea_resize_locked(lender);
            continue;
        }

        std::cout << "mallea scheduler selected jobs: "
                  << "job_a=" << first << ", job_b=" << second
                  << ", borrower=" << borrower << ", lender=" << lender
                  << ", epoch=" << next_epoch
                  << ", hostfile=" << borrower_hostfile << std::endl;

        const auto borrower_err = notify_daemons(
                client_mid, client_rpc_ids, borrower_current_hostfile,
                gkfs::rpc::daemon_resize_action::expand, next_epoch,
                borrower_hostfile, borrower);
        const auto lender_err = notify_daemons(
                client_mid, client_rpc_ids, lender_current_hostfile,
                gkfs::rpc::daemon_resize_action::borrowed, lender_info.epoch,
                borrower_hostfile, borrower);
        if(borrower_err != 0 || lender_err != 0) {
            std::lock_guard<std::mutex> lock(mallea_jobs_mutex);
            complete_mallea_resize_locked(borrower);
            complete_mallea_resize_locked(lender);
        } else {
            std::lock_guard<std::mutex> lock(mallea_jobs_mutex);
            auto borrower_it = mallea_jobs.find(borrower);
            if(borrower_it != mallea_jobs.end() &&
               !borrower_it->second.pending_unregister) {
                borrower_it->second.epoch = next_epoch;
                mallea_borrow_table[borrower] = lender;
            }
            complete_mallea_resize_locked(borrower);
            complete_mallea_resize_locked(lender);
        }
    }
}

void
run_mallea_shrink_scheduler(margo_instance_id client_mid,
                            gkfs::registry::client_rpc_ids client_rpc_ids,
                            std::atomic<bool>& running,
                            std::mutex& wait_mutex,
                            std::condition_variable& wait_cv) {
    std::mt19937 rng(std::random_device{}());
    std::unique_lock<std::mutex> wait_lock(wait_mutex);

    while(running.load()) {
        if(wait_cv.wait_for(wait_lock, std::chrono::seconds(60),
                            [&running] { return !running.load(); })) {
            break;
        }

        std::vector<std::pair<std::string, std::string>> borrow_pairs;
        {
            std::lock_guard<std::mutex> lock(mallea_jobs_mutex);
            for(const auto& [borrower, lender] : mallea_borrow_table) {
                borrow_pairs.emplace_back(borrower, lender);
            }
        }

        if(borrow_pairs.empty()) {
            continue;
        }

        std::shuffle(borrow_pairs.begin(), borrow_pairs.end(), rng);
        std::string borrower;
        std::string lender;
        mallea_info borrower_info;
        mallea_info lender_info;
        std::uint64_t next_epoch = 0;
        bool selected = false;

        {
            std::lock_guard<std::mutex> lock(mallea_jobs_mutex);
            for(const auto& [candidate_borrower, candidate_lender] :
                borrow_pairs) {
                auto borrower_it = mallea_jobs.find(candidate_borrower);
                auto lender_it = mallea_jobs.find(candidate_lender);
                if(borrower_it == mallea_jobs.end()) {
                    mallea_borrow_table.erase(candidate_borrower);
                    continue;
                }
                if(lender_it == mallea_jobs.end() ||
                   borrower_it->second.resizing || lender_it->second.resizing) {
                    continue;
                }

                borrower = candidate_borrower;
                lender = candidate_lender;
                borrower_it->second.resizing = true;
                lender_it->second.resizing = true;
                borrower_info = borrower_it->second;
                lender_info = lender_it->second;
                next_epoch = borrower_it->second.epoch + 1;
                selected = true;
                break;
            }
        }

        if(!selected) {
            continue;
        }

        const auto borrower_current_hostfile =
                current_hostfile(borrower, borrower_info);
        const auto lender_current_hostfile =
                current_hostfile(lender, lender_info);
        const auto shrink_hostfile = gkfs::utils::get_epoch_hostfile_name(
                borrower_info.hfile, borrower, next_epoch);
        const auto hostfile_err =
                write_shrink_hostfile(shrink_hostfile, borrower_info.hfile);
        if(hostfile_err != 0) {
            std::lock_guard<std::mutex> lock(mallea_jobs_mutex);
            complete_mallea_resize_locked(borrower);
            complete_mallea_resize_locked(lender);
            continue;
        }

        std::cout << "mallea shrink scheduler selected pair: "
                  << "borrower=" << borrower << ", lender=" << lender
                  << ", epoch=" << next_epoch
                  << ", hostfile=" << shrink_hostfile << std::endl;

        const auto borrower_err = notify_daemons(
                client_mid, client_rpc_ids, borrower_current_hostfile,
                gkfs::rpc::daemon_resize_action::shrink, next_epoch,
                shrink_hostfile, borrower);
        const auto lender_err = notify_daemons(
                client_mid, client_rpc_ids, lender_current_hostfile,
                gkfs::rpc::daemon_resize_action::shrink, lender_info.epoch,
                shrink_hostfile, borrower);
        if(borrower_err != 0 || lender_err != 0) {
            std::lock_guard<std::mutex> lock(mallea_jobs_mutex);
            complete_mallea_resize_locked(borrower);
            complete_mallea_resize_locked(lender);
        } else {
            std::lock_guard<std::mutex> lock(mallea_jobs_mutex);
            auto borrower_it = mallea_jobs.find(borrower);
            if(borrower_it != mallea_jobs.end() &&
               !borrower_it->second.pending_unregister) {
                borrower_it->second.epoch = next_epoch;
            }
            mallea_borrow_table.erase(borrower);
            complete_mallea_resize_locked(borrower);
            complete_mallea_resize_locked(lender);
        }
    }
}

} // namespace

namespace gkfs::registry {

MalleaScheduler::MalleaScheduler(margo_instance_id client_mid,
                                 client_rpc_ids client_rpc_ids)
    : client_mid_(client_mid), client_rpc_ids_(client_rpc_ids),
      running_(true) {
    expand_thread_ = std::thread(run_mallea_expand_scheduler, client_mid_,
                                 client_rpc_ids_, std::ref(running_),
                                 std::ref(wait_mutex_), std::ref(wait_cv_));
    try {
        shrink_thread_ = std::thread(run_mallea_shrink_scheduler, client_mid_,
                                     client_rpc_ids_, std::ref(running_),
                                     std::ref(wait_mutex_),
                                     std::ref(wait_cv_));
    } catch(...) {
        stop();
        throw;
    }
}

MalleaScheduler::~MalleaScheduler() {
    stop();
}

void
MalleaScheduler::stop() {
    if(!running_.exchange(false)) {
        return;
    }

    wait_cv_.notify_all();
    if(expand_thread_.joinable()) {
        expand_thread_.join();
    }
    if(shrink_thread_.joinable()) {
        shrink_thread_.join();
    }
}

} // namespace gkfs::registry
