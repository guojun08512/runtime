#include "runtime_core/task_callback.h"

#include <algorithm>
#include <cstring>

namespace {

void WriteU32LE(uint8_t *output, uint32_t offset, uint32_t value) {
    output[offset] = static_cast<uint8_t>(value);
    output[offset + 1] = static_cast<uint8_t>(value >> 8);
    output[offset + 2] = static_cast<uint8_t>(value >> 16);
    output[offset + 3] = static_cast<uint8_t>(value >> 24);
}

uint8_t InputByte(const RuntimeTaskContext *context,
                  uint32_t index,
                  uint64_t sequence) {
    if (context->input_data != nullptr && index < context->input_size) {
        return static_cast<const uint8_t *>(context->input_data)[index];
    }
    return static_cast<uint8_t>((sequence + index * 17) & 0xff);
}

int DefaultIecCallback(const RuntimeTaskContext *context) {
    if (context->event_id != RT_HOOK_COMM_CYCLE) {
        return 0;
    }
    if (context->output_data == nullptr || context->output_size < 16) {
        return 1;
    }
    auto *output = static_cast<uint8_t *>(context->output_data);
    std::memset(output, 0, context->output_size);
    const uint32_t count =
        std::min<uint32_t>(context->input_data == nullptr
                               ? 16
                               : context->input_size,
                           256);
    uint32_t sum = 0;
    uint32_t xor_sum = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t value = InputByte(context, i, context->sequence);
        sum += value;
        xor_sum ^= value;
    }
    WriteU32LE(output, 0, 0x49454331u);
    WriteU32LE(output, 4, static_cast<uint32_t>(context->sequence));
    WriteU32LE(output, 8, sum);
    WriteU32LE(output, 12, xor_sum);
    return 0;
}

int DefaultIoCallback(const RuntimeTaskContext *context) {
    if (context->event_id != RT_HOOK_COMM_CYCLE) {
        return 0;
    }
    if (context->output_data == nullptr || context->output_size < 16) {
        return 1;
    }
    auto *output = static_cast<uint8_t *>(context->output_data);
    std::memset(output, 0, context->output_size);
    const uint32_t payload_size =
        std::min<uint32_t>(context->input_data == nullptr
                               ? 256
                               : context->input_size,
                           context->output_size - 16);
    uint32_t checksum = 0;
    for (uint32_t i = 0; i < payload_size; ++i) {
        const uint8_t value =
            static_cast<uint8_t>(InputByte(context, i, context->sequence) ^
                                 0x5A);
        output[16 + i] = value;
        checksum = (checksum << 5) - checksum + value;
    }
    WriteU32LE(output, 0, 0x494f5a31u);
    WriteU32LE(output, 4, static_cast<uint32_t>(context->sequence));
    WriteU32LE(output, 8, payload_size);
    WriteU32LE(output, 12, checksum);
    return 0;
}

int DefaultNetworkCallback(const RuntimeTaskContext *context) {
    if (context->event_id != RT_HOOK_COMM_CYCLE) {
        return 0;
    }
    if (context->output_data == nullptr || context->output_size < 20) {
        return 1;
    }
    auto *output = static_cast<uint8_t *>(context->output_data);
    std::memset(output, 0, context->output_size);
    const uint32_t packet_size =
        std::min<uint32_t>(context->input_data == nullptr
                               ? 256
                               : context->input_size,
                           context->output_size - 20);
    uint32_t checksum = 0;
    for (uint32_t i = 0; i < packet_size; ++i) {
        const uint8_t value = InputByte(context, i, context->sequence);
        output[20 + i] = value;
        checksum += value;
        checksum = (checksum << 1) | (checksum >> 31);
    }
    WriteU32LE(output, 0, 0x4e455431u);
    WriteU32LE(output, 4, static_cast<uint32_t>(context->sequence));
    WriteU32LE(output, 8, packet_size);
    WriteU32LE(output, 12, checksum);
    WriteU32LE(output, 16, static_cast<uint32_t>(context->input_sequence));
    return 0;
}

} // namespace

TaskCallbackRegistry::TaskCallbackRegistry() {
    callbacks_[TypeKey(Config::TaskType::IECTASK)]["default"] =
        DefaultIecCallback;
    callbacks_[TypeKey(Config::TaskType::FILE_IO)]["default"] =
        DefaultIoCallback;
    callbacks_[TypeKey(Config::TaskType::NETWORK_IO)]["default"] =
        DefaultNetworkCallback;
}

TaskCallbackRegistry &TaskCallbackRegistry::Instance() {
    static TaskCallbackRegistry registry;
    return registry;
}

uint32_t TaskCallbackRegistry::TypeKey(Config::TaskType type) {
    return static_cast<uint32_t>(type);
}

bool TaskCallbackRegistry::Register(Config::TaskType type,
                                    const std::string &name,
                                    RuntimeTaskCallback callback) {
    if (name.empty() || callback == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto &registered = callbacks_[TypeKey(type)];
    return registered.emplace(name, callback).second;
}

bool TaskCallbackRegistry::Unregister(Config::TaskType type,
                                      const std::string &name,
                                      RuntimeTaskCallback expected_callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto type_it = callbacks_.find(TypeKey(type));
    if (type_it == callbacks_.end()) {
        return false;
    }
    auto callback_it = type_it->second.find(name);
    if (callback_it == type_it->second.end() ||
        callback_it->second != expected_callback) {
        return false;
    }
    type_it->second.erase(callback_it);
    return true;
}

RuntimeTaskCallback TaskCallbackRegistry::Resolve(
    Config::TaskType type,
    const std::string &name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto type_it = callbacks_.find(TypeKey(type));
    if (type_it == callbacks_.end()) {
        return nullptr;
    }
    auto callback_it = type_it->second.find(name);
    return callback_it == type_it->second.end() ? nullptr
                                                : callback_it->second;
}
