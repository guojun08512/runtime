#include <atomic>
#include <cstdint>

// 确保每个 Slot 对齐到 64 字节（常见 CPU 缓存行大小）
struct alignas(64) MessageSlot {
    std::atomic<uint32_t> status; // 0: 闲置, 1: 写入中, 2: 已就绪
    uint64_t timestamp;
    float data[8];                // 模拟小数据负载
};

// 预留的 Slot 数量应大于 enkiTS 的线程数
#define MAX_THREADS 32
#define SLOTS_PER_THREAD 10
#define TOTAL_SLOTS (MAX_THREADS * SLOTS_PER_THREAD)