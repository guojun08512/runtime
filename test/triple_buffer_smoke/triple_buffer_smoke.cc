#include "runtime_core/logging.h"
#include "runtime_core/triple_buffer.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>

#include <unistd.h>

namespace {

struct Payload {
    uint64_t sequence;
    uint64_t complement;
    uint32_t marker;
};

} // namespace

int main(int argc, char **argv) {
    runtime_logging::Init(argv[0]);

    const std::string name =
        "runtime.tb.test." + std::to_string(getpid());
    ipc::shm::handle::clear_storage(name.c_str());
    runtime_core::TripleBuffer writer(
        name, sizeof(Payload), runtime_core::TripleBuffer::Role::Writer);
    runtime_core::TripleBuffer reader(
        name, sizeof(Payload), runtime_core::TripleBuffer::Role::Reader);
    if (!writer.valid() || !reader.valid()) {
        LOG(ERROR) << "failed to create triple-buffer shared memory";
        return 1;
    }

    constexpr uint64_t kIterations = 100000;
    std::atomic<bool> finished{false};
    std::atomic<uint64_t> read_count{0};
    std::atomic<bool> inconsistent{false};

    std::thread write_thread([&]() {
        for (uint64_t i = 1; i <= kIterations; ++i) {
            auto *payload =
                static_cast<Payload *>(writer.AcquireWrite());
            if (payload == nullptr) {
                inconsistent.store(true, std::memory_order_relaxed);
                break;
            }
            payload->sequence = i;
            payload->complement = ~i;
            payload->marker = 0xA5A5A5A5u;
            if (!writer.Publish()) {
                inconsistent.store(true, std::memory_order_relaxed);
                break;
            }
        }
        finished.store(true, std::memory_order_release);
    });

    std::thread read_thread([&]() {
        while (!finished.load(std::memory_order_acquire) ||
               read_count.load(std::memory_order_relaxed) == 0) {
            uint64_t version = 0;
            const auto *payload = static_cast<const Payload *>(
                reader.AcquireRead(&version));
            if (payload == nullptr) {
                continue;
            }
            if (payload->complement != ~payload->sequence ||
                payload->marker != 0xA5A5A5A5u ||
                payload->sequence != version) {
                inconsistent.store(true, std::memory_order_relaxed);
                break;
            }
            read_count.fetch_add(1, std::memory_order_relaxed);
        }
    });

    write_thread.join();
    read_thread.join();

    if (inconsistent.load() || read_count.load() == 0) {
        LOG(ERROR) << "triple-buffer consistency test failed";
        return 1;
    }

    LOG(INFO) << "triple-buffer test passed, reads="
              << read_count.load();
    return 0;
}
