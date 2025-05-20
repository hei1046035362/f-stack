#pragma once
/// 创建线程，每个core_id创建一个线程


#include <iostream>
#include <vector>
#include <thread>

class ThreadArray {
public:
    // 构造函数，传入要创建的线程数量
    ThreadArray(const std::vector<int>& lcoreIdx, const std::vector<int>& ccoreIdx) : lcoreIdx(lcoreIdx),
    ccoreIdx(ccoreIdx) {}

    // 启动所有线程的函数，需要传入线程函数以及对应的参数（示例中线程函数接受一个整数参数）
    template<typename Func, typename... Args>
    void startThreads(Func&& func, Args&&... args) {
        for (unsigned int i = 0; i < lcoreIdx.size(); ++i) {
            std::thread thd(func, args..., lcoreIdx[i]);
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);       // 清空核心集合
            CPU_SET(ccoreIdx[i], &cpuset);     // 添加核心
            // 设置线程亲和性
            if (pthread_setaffinity_np(thd.native_handle(), sizeof(cpu_set_t), &cpuset) != 0) {
                std::cerr << "bound ccore[" << i << ":"  << ccoreIdx[i] << "] failed" << std::endl;
            }
            printf("Start Thread[%d]:Bound cpu[%d]\n", i, ccoreIdx[i]);
            threads.push_back(std::move(thd));
        }
        printf("All Working threads[%ld] started.\n", lcoreIdx.size());
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
    const std::vector<int>& ccoreIdx;
    std::vector<std::thread> threads;
};


void tgg_process_read(int lcore_idx);

// 透传处理线程
int init_bwtrans();
void uninit_bwtrans();

// // 线程函数，这里简单打印一个线程编号
// void threadFunction(int threadId) {
//     std::cout << "线程 " << threadId << " 正在运行" << std::endl;
// }

// int main() {
//     ThreadArray threadArr(5);  // 创建包含5个线程的线程数组对象
//     threadArr.startThreads(threadFunction);  // 启动所有线程
//     return 0;
// }