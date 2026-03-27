#pragma once
#include <atomic>
#include <cstdint>

enum class SlotState : uint32_t {
    IDLE = 0,        // 生产者可以写入
    PRODUCING = 1,   // 生产者锁定中
    READY = 2,       // 写入完成，等待分发
    PROCESSING = 3,  // 消费者已分发，enkiTS 线程处理中
    DONE = 4         // 业务处理完成，等待回收
};

struct alignas(64) MessageSlot {
    std::atomic<uint32_t> status{0}; 
    uint64_t timestamp;
    float data[16]; // 增加数据区
    size_t totalAllocations;
};

#define SLOTS_PER_THREAD 1
#define TOTAL_SLOTS (1 * SLOTS_PER_THREAD) // 假设最大32线程