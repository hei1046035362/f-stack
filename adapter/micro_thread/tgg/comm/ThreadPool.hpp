#ifndef __THREAD_POOL_H__
#define __THREAD_POOL_H__
/// 线程池    暂时用不上，如果后期业务较多，可以考虑把和后台交互的数据解包、组包以及计算型逻辑放到线程池中处理


#include <iostream>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>

class ThreadPool {
public:
    // 构造函数，初始化线程池
    ThreadPool(size_t numThreads) : stop(false) {
        for (size_t i = 0; i < numThreads; ++i) {
            threads.push_back(std::thread(&ThreadPool::workerThread, this));
        }
    }

    // 析构函数，停止线程池并等待所有线程完成任务
    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            stop = true;
        }
        condition.notify_all();
        for (auto& th : threads) {
            if (th.joinable()) {
                th.join();
            }
        }
    }

    // 向线程池提交任务
    template <typename F, typename... Args>
    void submit(F&& f, Args&&... args) {
        // 创建一个函数对象，绑定参数
        auto task = std::bind(std::forward<F>(f), std::forward<Args>(args)...);

        {
            std::unique_lock<std::mutex> lock(queueMutex);
            tasks.push(task);
        }
        condition.notify_one();
    }

private:
    // 线程池中的线程向量
    std::vector<std::thread> threads;

    // 任务队列，存储待执行的任务
    std::queue<std::function<void()>> tasks;

    // 互斥锁，用于保护任务队列
    std::mutex queueMutex;

    // 条件变量，用于线程间的同步
    std::condition_variable condition;

    // 标志位，用于指示线程池是否停止
    bool stop;

    // 工作线程函数，从任务队列中获取任务并执行
    void workerThread() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                condition.wait(lock, [this] { return stop ||!tasks.empty(); });
                if (stop && tasks.empty()) {
                    return;
                }
                task = tasks.front();
                tasks.pop();
            }
            task();
        }
    }
};



#endif // __THREAD_POOL_H__