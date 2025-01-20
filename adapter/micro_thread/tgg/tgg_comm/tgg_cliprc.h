#pragma once
/// 创建线程，每个core_id创建一个线程


#include <iostream>
#include <vector>
#include <thread>

class ThreadArray {
public:
    // 构造函数，传入要创建的线程数量
    ThreadArray(const std::vector<int>& lcoreIdx) : lcoreIdx(lcoreIdx) {}

    // 启动所有线程的函数，需要传入线程函数以及对应的参数（示例中线程函数接受一个整数参数）
    template<typename Func, typename... Args>
    void startThreads(Func&& func, Args&&... args) {
        for (int i = 0; i < lcoreIdx.size(); ++i) {
            threads.push_back(std::thread(func, args..., lcoreIdx[i]));
        }
    }

    // 等待所有线程执行完毕的函数
    void joinAllThreads() {
        for (auto& th : threads) {
            if (th.joinable()) {
                th.join();
            }
        }
    }

    ~ThreadArray() {
        // 在析构函数中确保所有线程都已正确结束，避免资源泄漏
        joinAllThreads();
    }

private:
    const std::vector<int>& lcoreIdx;
    std::vector<std::thread> threads;
};


void tgg_process_read(int lcore_idx);


// // 线程函数，这里简单打印一个线程编号
// void threadFunction(int threadId) {
//     std::cout << "线程 " << threadId << " 正在运行" << std::endl;
// }

// int main() {
//     ThreadArray threadArr(5);  // 创建包含5个线程的线程数组对象
//     threadArr.startThreads(threadFunction);  // 启动所有线程
//     return 0;
// }