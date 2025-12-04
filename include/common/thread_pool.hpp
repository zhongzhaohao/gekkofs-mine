#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <atomic>
#include <memory>

class ThreadPool {
public:
    ThreadPool() = default;
    ~ThreadPool() { stop(); }

    // 禁止拷贝
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // 初始化线程池
    void init(size_t numThreads) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (workers_.empty()) {
            stop_ = false;
            workers_.reserve(numThreads);
            for (size_t i = 0; i < numThreads; ++i) {
                workers_.emplace_back([this] { workerThread(); });
            }
        }
    }

    // 停止线程池
    void stop() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!stop_) {
            stop_ = true;
            cv_.notify_all();
            lock.unlock();
            
            for (auto& thread : workers_) {
                if (thread.joinable()) {
                    thread.join();
                }
            }
            workers_.clear();
        }
    }

    // 提交任务
    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args) 
        -> std::future<typename std::result_of<F(Args...)>::type> {
        using return_type = typename std::result_of<F(Args...)>::type;

        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        
        std::future<return_type> res = task->get_future();
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stop_)
                throw std::runtime_error("enqueue on stopped ThreadPool");
            
            tasks_.emplace([task]() { (*task)(); });
        }
        cv_.notify_one();
        return res;
    }

private:
    void workerThread() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                
                if (stop_ && tasks_.empty())
                    return;
                
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            task();
        }
    }

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> stop_{true};
};