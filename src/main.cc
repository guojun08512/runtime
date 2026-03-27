#include <iostream>
#include <vector>
#include <chrono>
#include <thread>

#include "runtime/protocol/protocol.h"
#include "enkiTs/TaskScheduler.h"
#include "libipc/ipc.h" // 假设已安装 ipc-cpp

using namespace std;

// --- 生产者任务 (enkiTS) ---
struct MultiProducerTask : enki::ITaskSet {
    MessageSlot* shared_mem;

    void ExecuteRange(enki::TaskSetPartition range, uint32_t threadnum) override {
        // threadnum 是 enkiTS 分配给当前线程的唯一索引 [0, NumThreads-1]
        for (uint32_t i = range.start; i < range.end; ++i) {
            // 计算当前线程对应的起始槽位
            uint32_t base_idx = threadnum * SLOTS_PER_THREAD;
            
            // 找到一个空闲槽位 (简单的线性探测)
            for (uint32_t s = 0; s < SLOTS_PER_THREAD; ++s) {
                MessageSlot& slot = shared_mem[base_idx + s];
                
                uint32_t expected = 0;
                if (slot.status.compare_exchange_strong(expected, 1)) {
                    // --- 开始无锁写入 ---
                    slot.timestamp = i; 
                    slot.data[0] = static_cast<float>(i * 0.1);
                    // --- 写入完成 ---
                    
                    slot.status.store(2, std::memory_order_release);
                    break; 
                }
            }
        }
    }
};

// --- 消费者逻辑 (独立线程) ---
void ConsumerLoop(MessageSlot* shared_mem, bool& keep_running) {
    uint64_t processed_count = 0;
    while (keep_running) {
        bool found_data = false;
        for (uint32_t i = 0; i < TOTAL_SLOTS; ++i) {
            MessageSlot& slot = shared_mem[i];
            
            if (slot.status.load(std::memory_order_acquire) == 2) {
                // 读取数据
                // do_something(slot.data);
                processed_count++;
                
                // 重置槽位为闲置
                slot.status.store(0, std::memory_order_relaxed);
                found_data = true;
            }
        }
        
        if (!found_data) {
            // 高频场景下建议用 yield 而不是 sleep，以保持极低延迟
            std::this_thread::yield(); 
        }
    }
    cout << "Consumer finished. Total processed: " << processed_count << endl;
}

int main() {
    // 1. 初始化 ipc-cpp 共享内存
    // 这里创建一个大小足以容纳所有 Slot 的共享内存区域
    ipc::shm::handle shm_handle{"my_fast_ipc", sizeof(MessageSlot) * TOTAL_SLOTS};
    MessageSlot* raw_ptr = static_cast<MessageSlot*>(shm_handle.get());
    
    // 初始化内存状态
    for(int i=0; i<TOTAL_SLOTS; ++i) raw_ptr[i].status.store(0);

    // 2. 初始化 enkiTS 调度器
    enki::TaskScheduler ts;
    ts.Initialize();

    // 3. 启动消费者线程
    bool keep_running = true;
    thread consumer_thread(ConsumerLoop, raw_ptr, std::ref(keep_running));

    // 4. 派发生产者任务 (模拟 10000 次高频数据产生)
    MultiProducerTask producer;
    producer.shared_mem = raw_ptr;
    producer.m_SetSize = 10000;

    cout << "Dispatching producer tasks..." << endl;
    ts.AddTaskSetToPipe(&producer);
    ts.WaitforTask(&producer);

    // 等待数据处理完
    this_thread::sleep_for(chrono::milliseconds(500));
    keep_running = false;
    consumer_thread.join();

    return 0;
}