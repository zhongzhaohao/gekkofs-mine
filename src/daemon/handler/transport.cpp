/*
  Copyright 2018-2024, Barcelona Supercomputing Center (BSC), Spain
  Copyright 2015-2024, Johannes Gutenberg Universitaet Mainz, Germany
*/


#include <daemon/handler/transport.hpp>
#include <client/user_functions.hpp>
#include <client/preload.hpp>
#include <common/metadata.hpp>
#include <stage/stage.hpp>
#include <stage/stage_util.hpp>
#include <hermes.hpp>
#include <CLI/CLI.hpp>
#include <thread>

using namespace std;


static size_t BUFFER_SIZE;
static size_t BLOCK_SIZE = 4 *1024 *1024; // 4MB 
static int N_buffers;
static int threads;
static bool stage_in = false;
static int flag;
static Transport_options opts;
static vector<int> errors{};
static mutex emtx;
static std::atomic<bool> stop_flag{false};
//use mmap or pread?
void readFile(int src_fd, const std::string& src, std::vector<void*>& buffers, 
              std::vector<bool>& buffer_ready,std::mutex& mtx, 
              std::condition_variable& cv_read, std::condition_variable& cv_write,
              bool& read_finished, off_t offset, size_t wsize) {

    int cur_buf = 0;
    off_t cur_off = offset;
    size_t remaining = wsize;
   
    while (remaining > 0 && !stop_flag.load()) { 
         //check buffer ready to read
        {
            std::unique_lock<std::mutex> lock(mtx);
            cv_read.wait(lock, [cur_buf, &buffer_ready] {
                return !buffer_ready[cur_buf] || stop_flag.load(); 
            });
            if (stop_flag) break; 
        }
        // read to buffer
        size_t read_size = (remaining < BUFFER_SIZE) ? remaining : BUFFER_SIZE;
        std::pair<int, off_t> ret = {0, 0};
        if (stage_in) {
            ret.second = pread(src_fd, buffers[cur_buf], read_size, cur_off);
        } else {
            std::set<uint64_t> failed;
            ret = gkfs::rpc::forward_read(src, buffers[cur_buf], cur_off, read_size, 0, failed);
        }
        if (ret.second <= 0 || ret.first) {
            // cancel all read afterwards
            {
                std::lock_guard<std::mutex> lock(mtx);
                read_finished = true;
                cv_write.notify_all();
            }
            break;
        }

        //notify buffer ready to write
        {
            std::lock_guard<std::mutex> lock(mtx);
            buffer_ready[cur_buf] = true;
            cv_write.notify_all();
        }

        cur_buf = (cur_buf + 1) % N_buffers;
        cur_off += ret.second;
        remaining -= ret.second;
    } // end while

    {
        std::lock_guard<std::mutex> lock(mtx);
        read_finished = true;
        cv_write.notify_all();
    }
}


void writeFile(int dest_fd, const std::string& dest, std::vector<void*>& buffers, 
               std::vector<bool>& buffer_ready, std::mutex& mtx, 
               std::condition_variable& cv_read, std::condition_variable& cv_write,
               bool& read_finished, off_t offset, size_t wsize) {

    int cur_buf = 0;
    off_t cur_off = offset;
    size_t remaining = wsize;
    try {
        while (remaining > 0 && !stop_flag.load()) { 
            // check buffer ready to write
            {
                std::unique_lock<std::mutex> lock(mtx);
                cv_write.wait(lock, [cur_buf, &buffer_ready, &read_finished] {
                    return buffer_ready[cur_buf] || read_finished || stop_flag.load();
                });
                if (stop_flag) break; 
            }
            // write buffer
            if (buffer_ready[cur_buf]) {
                std::pair<int, off_t> ret;
                size_t  write_size= (remaining < BUFFER_SIZE) ? remaining : BUFFER_SIZE;
                if (stage_in) {
                    ret = gkfs::rpc::forward_write(dest, buffers[cur_buf], cur_off, write_size, 0);
                } else {
                    if(opts.flag & STAGE_O_DIRECT)
                        write_size = make_align(write_size, opts.o_direct_blk_size);
                    ret.second = pwrite(dest_fd, buffers[cur_buf], write_size, cur_off);
                }
                {
                    std::lock_guard<std::mutex> lock(mtx);
                    buffer_ready[cur_buf] = false;
                    cv_read.notify_all();
                }
                if (ret.second <= 0 || ret.first) {   
                    throw std::system_error(EIO, std::system_category(), "Failed to write.");
                }
                remaining -= write_size;
                cur_off += write_size;
            }

            // check all read have been written
            if (read_finished) {
                bool all_written = true;
                for (bool ready : buffer_ready) {
                    if (ready) {
                        all_written = false;
                        break;
                    }
                }
                if (all_written) {
                    break;
                }
            }
            cur_buf = (cur_buf + 1) % N_buffers;
        }
    } catch (const std::system_error& e) {
        std::cerr << "system error: " << e.what() << std::endl;
        {
            std::unique_lock<std::mutex> lock(emtx);
            stop_flag.store(true);
            errors.push_back(e.code().value());
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << std::endl;
        {
            std::unique_lock<std::mutex> lock(emtx);
            stop_flag.store(true);
            errors.push_back(EBUSY);
        }
    }    
}

// 外层线程执行的函数
void outer_thread(int src_fd, int dest_fd, const std::string& src, 
            const std::string& dest, off_t offset, size_t wsize) {

    try {
        //malloc buffers
        std::vector<void*> buffers{};
        size_t total_size = BUFFER_SIZE * N_buffers;
        ScopedBuffer total_buffer(
            total_size, 
            (flag & STAGE_O_DIRECT) && !stage_in,
            opts.o_direct_blk_size
        );
    
        char* base_ptr = static_cast<char*>(total_buffer.get());
        for (int i = 0; i < N_buffers; ++i) {
            buffers.emplace_back(base_ptr + i * BUFFER_SIZE);
        }

        std::vector<bool> buffer_ready(N_buffers, false);
        std::mutex mtx;
        std::condition_variable cv_read;
        std::condition_variable cv_write;
        bool read_finished = false;

        // use Double-buffer-technique
        std::thread t_read(readFile, src_fd, src, std::ref(buffers), std::ref(buffer_ready),
                            std::ref(mtx), std::ref(cv_read), std::ref(cv_write),
                            std::ref(read_finished), offset, wsize);

        std::thread t_write(writeFile, dest_fd, dest, std::ref(buffers), std::ref(buffer_ready),
                                std::ref(mtx), std::ref(cv_read), std::ref(cv_write),
                                std::ref(read_finished), offset, wsize);

        t_read.join();
        t_write.join();

    } catch (const std::system_error& e) {
        std::cerr << "system error: " << e.what() << std::endl;
        {
            std::unique_lock<std::mutex> lock(emtx);
            errors.push_back(e.code().value());
            stop_flag.store(true);
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << std::endl;
        {
            std::unique_lock<std::mutex> lock(emtx);
            errors.push_back(EBUSY);
            stop_flag.store(true);
        }
    }    
}



int forward_transport(const std::string& src, const std::string& dest, const std::string& opts_str){
    int exit_code = 0;
    int src_fd = -1;
    int dest_fd = -1;

    try {
        // init
        opts = Transport_options::deserialize(opts_str);
        stage_in = opts.flag & STAGE_IN;
        N_buffers = opts.n_buffers;
        BUFFER_SIZE = opts.buffer_size;
        flag = opts.flag;
        BLOCK_SIZE = opts.block_size;
        size_t t_blk_size;

        // re init global vars
        stop_flag.store(false);
        errors.clear();
        /*  If force, do as parameters set.Otherwise, do to insure 
        one thread has at least buffer_size bytes to write except 
        last one.*/
        if(opts.flag & STAGE_FORCE){
            threads = opts.threads;
            t_blk_size = Div(opts.count, threads);
        } else {
            unsigned long min_threads = Div(opts.count, BUFFER_SIZE);
            threads = std::min(opts.threads, min_threads);
            size_t blks = Div(opts.count, BLOCK_SIZE);
            t_blk_size = BLOCK_SIZE * Div(blks, threads);
        }

        std::vector<std::thread> outer_threads;
        auto remain = opts.count;

        if (stage_in) {
            src_fd = open(src.c_str(), O_RDONLY);
            if (src_fd == -1) {
                throw std::system_error(errno, std::system_category(), "Failed open src file: " + src);
            }
        } else {
            auto w_flag = O_WRONLY | O_CREAT;
            if(opts.flag & STAGE_O_DIRECT) w_flag |= O_DIRECT;
            dest_fd = open(dest.c_str(), w_flag, 0666);
            if (dest_fd == -1) {
                throw std::system_error(errno, std::system_category(), "Failed open dest file: " + dest);
            }
        }

        for (int i = 0; i < threads && remain > 0; ++i) {
            auto wsize = std::min(remain, t_blk_size);
            remain -= wsize;
            outer_threads.emplace_back(outer_thread, src_fd, dest_fd, src, dest,
                    opts.offset + t_blk_size * i, wsize);
        }

        for (auto& thread : outer_threads) {
            thread.join();
        }

    } catch (const std::system_error& e) {
        std::cerr << "system error: " << e.what() << std::endl;
        exit_code = e.code().value();
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << std::endl;
        exit_code = EBUSY;
    }

    if (src_fd != -1) close(src_fd);
    if (dest_fd != -1) close(dest_fd);
    if(!exit_code && errors.size()){
        exit_code = errors.front();
    }
    return exit_code;
}
