/*
 * (C) 2015 The University of Chicago
 *
 * See COPYRIGHT in top-level directory.
 */

#ifndef GEKKOFS_REGISTRY_MALLEA_SCHEDULER_HPP
#define GEKKOFS_REGISTRY_MALLEA_SCHEDULER_HPP

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <margo.h>

#include <registry/rpc_registration.hpp>

namespace gkfs::registry {

class MalleaScheduler {
public:
    MalleaScheduler(margo_instance_id client_mid,
                    client_rpc_ids client_rpc_ids);
    ~MalleaScheduler();

    MalleaScheduler(const MalleaScheduler&) = delete;
    MalleaScheduler&
    operator=(const MalleaScheduler&) = delete;

    void
    stop();

private:
    margo_instance_id client_mid_;
    client_rpc_ids client_rpc_ids_;
    std::atomic<bool> running_;
    std::mutex wait_mutex_;
    std::condition_variable wait_cv_;
    std::thread expand_thread_;
    std::thread shrink_thread_;
};

} // namespace gkfs::registry

#endif // GEKKOFS_REGISTRY_MALLEA_SCHEDULER_HPP
