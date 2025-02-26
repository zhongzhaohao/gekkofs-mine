#include <client/rpc/forward_stage.hpp>
#include <client/logging.hpp>
#include <client/preload_util.hpp>
#include <client/rpc/rpc_types.hpp>
#include <common/rpc/distributor.hpp>

#include <daemon/handler/transport.hpp>
namespace gkfs::rpc {

/**
 * Gets fs configuration information from the running daemon and transfers it to
 * the memory of the library
 * @return
 */
int
forward_stage(size_t host_id, const std::string& in_path, const std::string& out_path,
              const std::string& opts) {

    auto endp = CTX->hosts().at(host_id);
    gkfs::rpc::stage::output out;
    gkfs::rpc::stage::input in(in_path, out_path, opts);
    
    try {
        LOG(DEBUG, "Forwarding staging infomations.");

        out = ld_network_service->post<gkfs::rpc::stage>(endp,in).get().at(0);
        
        LOG(DEBUG, "Got response success: {}", out.err());

        return out.err() ? out.err() : 0; 
    } catch(const std::exception& ex) {
        LOG(ERROR, "while getting rpc output");
        return EBUSY;
    }

}

/**
 * TODO num_copies
 * @return
 */
int
forward_stage_metadata(const std::string& path,const mode_t mode, const size_t size,
                    const int flag, std::string &attr) {

    auto endp = CTX->hosts().at(
                CTX->distributor()->locate_file_metadata(path, 0));
    gkfs::rpc::stage_metadata::output out;
    gkfs::rpc::stage_metadata::input in(path, mode, size, flag);

    try {
        LOG(DEBUG, "Staging is processing file metadata.");

        out = ld_network_service->post<gkfs::rpc::stage_metadata>(endp,in).get(1200).at(0);
        
        LOG(DEBUG, "Got response success: {}", out.err());
        if(out.err())
            return out.err();

        attr = out.db_val();
    } catch(const std::exception& ex) {
        LOG(ERROR, "while getting rpc output");
        return EBUSY;
    }
    return 0;
}


} // namespace gkfs::rpc
