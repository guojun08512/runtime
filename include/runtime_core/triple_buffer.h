#pragma once

#include "libipc/shm.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

namespace runtime_core {

class TripleBuffer {
public:
    enum class Role {
        Writer,
        Reader,
    };

    TripleBuffer(std::string name,
                 uint32_t slot_size,
                 Role role)
        : name_(std::move(name)),
          slot_size_(AlignUp(slot_size, kCacheLineSize)),
          role_(role) {
        const std::size_t required = DataOffset() + 3 * slot_size_;
        handle_ = std::make_unique<ipc::shm::handle>(
            name_.c_str(), required, ipc::shm::create | ipc::shm::open);
        if (!handle_ || !handle_->valid() || handle_->size() < required) {
            handle_.reset();
            return;
        }

        base_ = static_cast<uint8_t *>(handle_->get());
        control_ = reinterpret_cast<Control *>(base_);
        if (control_->magic.load(std::memory_order_acquire) != kMagic ||
            control_->abi_version.load(std::memory_order_acquire) != kAbi ||
            control_->slot_size.load(std::memory_order_acquire) != slot_size_) {
            std::memset(base_, 0, required);
            control_->abi_version.store(kAbi, std::memory_order_relaxed);
            control_->slot_size.store(slot_size_, std::memory_order_relaxed);
            control_->middle_index.store(2, std::memory_order_relaxed);
            control_->fresh.store(0, std::memory_order_relaxed);
            control_->magic.store(kMagic, std::memory_order_release);
        }

        writer_index_ = 0;
        reader_index_ = 1;
    }

    TripleBuffer(const TripleBuffer &) = delete;
    TripleBuffer &operator=(const TripleBuffer &) = delete;

    bool valid() const { return handle_ != nullptr; }
    const std::string &name() const { return name_; }
    uint32_t slot_size() const { return slot_size_; }

    void *AcquireWrite() {
        if (!valid() || role_ != Role::Writer) {
            return nullptr;
        }
        return Slot(writer_index_);
    }

    bool Publish() {
        if (!valid() || role_ != Role::Writer) {
            return false;
        }

        const uint64_t sequence =
            control_->publish_sequence.fetch_add(1,
                                                 std::memory_order_relaxed) +
            1;
        control_->slot_sequence[writer_index_].store(
            sequence, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);

        const uint32_t published = writer_index_;
        writer_index_ =
            control_->middle_index.exchange(published,
                                             std::memory_order_acq_rel);
        control_->fresh.store(1, std::memory_order_release);
        return true;
    }

    const void *AcquireRead(uint64_t *sequence) {
        if (!valid() || role_ != Role::Reader) {
            return nullptr;
        }

        if (control_->fresh.exchange(0, std::memory_order_acquire) != 0) {
            reader_index_ =
                control_->middle_index.exchange(reader_index_,
                                                std::memory_order_acq_rel);
            reader_has_data_ = true;
        }
        if (!reader_has_data_) {
            return nullptr;
        }

        if (sequence != nullptr) {
            *sequence = control_->slot_sequence[reader_index_].load(
                std::memory_order_relaxed);
        }
        return Slot(reader_index_);
    }

private:
    struct alignas(64) Control {
        std::atomic<uint64_t> magic{0};
        std::atomic<uint32_t> abi_version{0};
        std::atomic<uint32_t> slot_size{0};
        std::atomic<uint32_t> middle_index{2};
        std::atomic<uint32_t> fresh{0};
        std::atomic<uint64_t> publish_sequence{0};
        std::atomic<uint64_t> slot_sequence[3]{};
    };

    static constexpr uint64_t kMagic = 0x5254334255463131ULL;
    static constexpr uint32_t kAbi = 1;
    static constexpr std::size_t kCacheLineSize = 64;

    static constexpr std::size_t AlignUp(std::size_t value,
                                         std::size_t alignment) {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    static constexpr std::size_t DataOffset() {
        return AlignUp(sizeof(Control), kCacheLineSize);
    }

    uint8_t *Slot(uint32_t index) {
        return base_ + DataOffset() + index * slot_size_;
    }

    const uint8_t *Slot(uint32_t index) const {
        return base_ + DataOffset() + index * slot_size_;
    }

    std::string name_;
    uint32_t slot_size_ = 0;
    Role role_ = Role::Writer;
    std::unique_ptr<ipc::shm::handle> handle_;
    uint8_t *base_ = nullptr;
    Control *control_ = nullptr;
    uint32_t writer_index_ = 0;
    uint32_t reader_index_ = 1;
    bool reader_has_data_ = false;
};

class TaskBuffers {
public:
    TaskBuffers(const std::string &task_id,
                uint32_t input_size,
                uint32_t output_size)
        : input_(MakeName(task_id, "in"),
                 input_size,
                 TripleBuffer::Role::Reader),
          output_(MakeName(task_id, "out"),
                  output_size,
                  TripleBuffer::Role::Writer) {}

    bool valid() const { return input_.valid() && output_.valid(); }
    const std::string &input_name() const { return input_.name(); }
    const std::string &output_name() const { return output_.name(); }

    static std::string InputName(const std::string &task_id) {
        return MakeName(task_id, "in");
    }

    static std::string OutputName(const std::string &task_id) {
        return MakeName(task_id, "out");
    }

    const void *AcquireInput(uint32_t *size, uint64_t *sequence) {
        if (size != nullptr) {
            *size = input_.slot_size();
        }
        return input_.AcquireRead(sequence);
    }

    void *AcquireOutput(uint32_t *size) {
        if (size != nullptr) {
            *size = output_.slot_size();
        }
        return output_.AcquireWrite();
    }

    bool PublishOutput() { return output_.Publish(); }

private:
    static std::string MakeName(const std::string &task_id,
                                const char *direction) {
        std::string safe;
        safe.reserve(task_id.size());
        for (char character : task_id) {
            const bool valid =
                (character >= 'a' && character <= 'z') ||
                (character >= 'A' && character <= 'Z') ||
                (character >= '0' && character <= '9') ||
                character == '_' || character == '-' || character == '.';
            safe.push_back(valid ? character : '_');
            if (safe.size() >= 64) {
                break;
            }
        }
        if (safe.empty()) {
            safe = "task";
        }

        uint32_t hash = 2166136261u;
        for (char character : task_id) {
            hash ^= static_cast<uint8_t>(character);
            hash *= 16777619u;
        }

        static constexpr char kHex[] = "0123456789abcdef";
        char suffix[9] = {};
        for (int i = 7; i >= 0; --i) {
            suffix[i] = kHex[hash & 0x0f];
            hash >>= 4;
        }
        return "runtime.tb." + safe + "." + direction + "." + suffix;
    }

    TripleBuffer input_;
    TripleBuffer output_;
};

static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);

} // namespace runtime_core
