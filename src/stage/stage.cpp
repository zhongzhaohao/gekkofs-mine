/*
  Copyright 2018-2024, Barcelona Supercomputing Center (BSC), Spain
  Copyright 2015-2024, Johannes Gutenberg Universitaet Mainz, Germany
*/

#include <client/user_functions.hpp>
#include <client/preload.hpp>
#include <common/metadata.hpp>
#include <stage/stage.hpp>
#include <stage/stage_util.hpp>
#include <ctime>
#include <fstream>
#include <iostream>
#include <hermes.hpp>
#include <CLI/CLI.hpp>
#include <daemon/handler/transport.hpp>
#include <system_error>
#include <filesystem>
namespace fs = std::filesystem;
using namespace std;

static unsigned long nodes = 1;
static int flag = 0;
static std::vector<size_t> hosts;

void
parse_input(const Stage_options& opts, const CLI::App& desc, Transport_options& topt) {
    nodes = CTX->hosts().size();
    if(desc.count("--buffer_size")){
        topt.buffer_size = parseStorageSize(opts.buffer_size);
    }
    if(desc.count("--block_size")){
        topt.block_size = parseStorageSize(opts.block_size);
    }
    if(desc.count("--threads")){
        topt.threads = opts.threads;
    }
    if(desc.count("--nodes") && opts.nodes > 0){
        nodes = std::min(opts.nodes, nodes);
    }
    if(desc.count("--n_buffers")){
        topt.n_buffers = opts.n_buffers;
    }
    if(desc.count("--recursive")){
        flag |= STAGE_RECURSIVE;
    }   
    if(desc.count("--force")){
        flag |= STAGE_FORCE;
    } 
    if(desc.count("--nodelists")){
        FastNodeChecker checker;
        checker.parse(opts.nodelists);
        for(unsigned long id = 0; id < CTX->hosts().size(); id ++){
            auto hostname = CTX->hosts_name().at(id);
            if(checker.contains(hostname))
                hosts.push_back(id);
        }
        if(hosts.size() > 0)
            nodes = hosts.size();
    } else {
        hosts.resize(nodes);
        std::iota(hosts.begin(), hosts.end(), 0);
    }

    //make this at last
    if(desc.count("--o_direct_size") && !(flag & STAGE_IN)){
        flag |= STAGE_O_DIRECT;
        flag &= ~STAGE_FORCE;
        topt.o_direct_blk_size = opts.o_direct_size;
        size_t blk_size = opts.o_direct_size;
        topt.buffer_size = make_align(topt.buffer_size, blk_size);
        topt.block_size = make_align(topt.block_size, blk_size);
    }
}

std::string get_abs_path(const std::string& path) {
    try {
        return fs::absolute(path).string();
    } catch (const fs::filesystem_error& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        exit(1);
    }
}

void parrallel_stage(const size_t host_id, const std::string src, const std::string dest, 
                    const std::string str_opts){
                        
    auto ret = gkfs::rpc::forward_stage(host_id, src, dest, str_opts);
    if(ret){
        std::error_code ec(ret, std::system_category());
        std::cerr << "Data transfer to daemon at " << CTX->hosts_name()[host_id] << " failed.\n";
        std::cerr << "Error: " << ec.message() << std::endl;
        exit(1);
    }
}

int main(int argc, char* argv[]){
    CLI::App desc{"Allowed options"};
    Stage_options opts{};
    
    desc.add_option("source", opts.src, "Source file ")
       ->required()->expected(1);
    desc.add_option("destination", opts.dest, "Destination file")
       ->required()->expected(1);
    desc.add_option("--o_direct_size", opts.o_direct_size,
                    "O_DIRECT block size. O_DIRECT is not used as default."
                    "Only used for stage out, writing data to underlining system.");
    desc.add_option("--buffer_size,-b", opts.buffer_size,
                    "Bytes stage in/out of underlining file system at one time when doing staging.");
    desc.add_option("--block_size,-a", opts.block_size,
                    "Divide file into blocks and equally assigned to each thread.");
    desc.add_option("--threads,-t", opts.threads,
                    "Number of threads per node."
                    "If file size divided by buffer size and num nodes is too small, this is ignored.");
    desc.add_option("--nodes,-n", opts.nodes,
                "Specify number of nodes to do staging tasks, default all."
                "If file size divided by buffer size  is too small, this is ignored.");
    desc.add_option("--nodelists", opts.nodelists,
                "Specify nodes to do staging tasks. Higher priority than --nodes.");
    desc.add_option("--n_buffers", opts.n_buffers,
                "Double buffer technique, default 2."
                "Maybe useful to set it larger when staging out.");
    desc.add_flag("--recursive,-r", "NOT COMPLETATED. Used for directory staging.");
    desc.add_flag("--force,-f", "Num of threads and nodes will be forcefully set"
                " regardless of the file size.");
    desc.add_flag("--mmap,-m", "NOT COMPLETATED.If set, the file in the underlying system "
                "will be manipulated by mmap instead of pread/pwrite.");

    try {
        desc.parse(argc, argv);
    } catch(const CLI::ParseError& e) {
        return desc.exit(e);
    }


    gkfs_init();
    std::string mountDir = CTX->mountdir();
    opts.src = get_abs_path(opts.src);
    opts.dest = get_abs_path(opts.dest);
    auto src = opts.src;
    auto dest = opts.dest;    
    bool out = src.find(mountDir) == 0;
    bool in = dest.find(mountDir) == 0;
    if ((in && out) || (!in &&!out)) {
        std::cerr << "Only allow staging into " 
            << mountDir << " or out of it" << std::endl;
        exit(1);
    }
    if(in) flag |= STAGE_IN;

    //parse paras to init transport options
    Transport_options trans_opt_init;
    parse_input(opts, desc, trans_opt_init);

    size_t fsize = 0;
    struct stat st;
    std::string attr;

    //handle metadata
    if(in){
        if (stat(src.c_str(), &st) == -1) {
            std::cerr << "Source file " << src << " doesn't exist." << std::endl;
            exit(1);
        }
        dest = dest.substr(mountDir.size());
        auto err = gkfs::rpc::forward_stage_metadata(dest, st.st_mode, st.st_size, flag, attr);
        if(err){
            if(err == ENOENT)
                std::cerr << opts.dest << ": parent directory doesn't exist."<< std::endl;
            else
                std::cerr << opts.dest << ": metadata failed to stage in gekkofs." << std::endl;
            exit(1);
        } 
        fsize = st.st_size;
    } else {
        src = src.substr(mountDir.size());
        auto err = gkfs::rpc::forward_stage_metadata(src, st.st_mode, st.st_size, flag, attr);
        if(err){
            if(err == ENOENT)
                std::cerr << opts.src << " doesn't exist." << std::endl;
            exit(1);
        }
        gkfs::metadata::Metadata md(attr);
        if(S_ISDIR(md.mode())){
            std::cerr << opts.src << " is a directory." << std::endl;
            exit(1);
        }
        fsize = md.size();
    }

    //init transport options of all nodes
    trans_opt_init.flag = flag;
    std::vector<Transport_options> trans_opts(nodes, trans_opt_init);
    size_t n_size, offset = 0;
    size_t mem_used = 0;
    unsigned long marks = 0;
    size_t remaining = fsize;

    //calculate file size each daemon processes.
    if(flag & STAGE_FORCE){
        n_size = Div(fsize, nodes) ; 
        mem_used = nodes * trans_opt_init.n_buffers * trans_opt_init.threads * trans_opt_init.buffer_size;
    } else {
        auto blks = Div(fsize, trans_opt_init.buffer_size);
        //guarantee every node has at least one buffer_size data to stage;
        nodes = std::min(blks, nodes); 
        auto n_blks = blks / nodes;
        marks = blks % nodes;
        n_size = n_blks * trans_opt_init.buffer_size;
        mem_used = ( nodes * std::min(n_blks, trans_opt_init.threads) + 
                    (nodes - marks) * std::min(n_blks + 1, trans_opt_init.threads) )
                    * trans_opt_init.n_buffers *  trans_opt_init.buffer_size ;
    }            
    std::cout<< "Used "<< nodes << " nodes"<<" malloc " << convertBytes(mem_used) << " as total."<<std::endl;

    //forward real stage to daemons
    //TODO forward_stage wait too long.
    std::vector<std::thread> stage_threads;
    std::vector<size_t> host_ids{};
    for(long unsigned int idx = 0; idx < nodes && remaining > 0; idx++)   {
        host_ids.push_back(hosts[idx]);
        n_size += idx > (nodes - marks - 1)? trans_opt_init.buffer_size : 0;
        auto count = std::min(n_size, remaining);
        trans_opts[idx].count = count;
        trans_opts[idx].offset = offset; 
        remaining -=  count;
        offset += count;
    }

    for(long unsigned int idx = 0; idx < host_ids.size(); idx++)   {
        stage_threads.emplace_back(parrallel_stage, host_ids[idx], src, 
                                dest, trans_opts[idx].serialize());
    }

    for (auto& thread : stage_threads) {
        thread.join();
    }

    if(!(flag & STAGE_IN)){
        truncate64(dest.c_str(),fsize);
    }
    gkfs_end();
    return 0;
}
