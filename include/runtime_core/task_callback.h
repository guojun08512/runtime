#pragma once

#include "runtime_core/global.h"
#include "runtime_plugin/plugin_api.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

using RuntimeTaskContext = RtTaskContextV1;
using RuntimeTaskCallback = RtTaskCallbackV1;

class TaskCallbackRegistry {
public:
    static TaskCallbackRegistry &Instance();

    bool Register(Config::TaskType type,
                  const std::string &name,
                  RuntimeTaskCallback callback);
    bool Unregister(Config::TaskType type,
                    const std::string &name,
                    RuntimeTaskCallback expected_callback);
    RuntimeTaskCallback Resolve(Config::TaskType type,
                                const std::string &name) const;

private:
    TaskCallbackRegistry();
    static uint32_t TypeKey(Config::TaskType type);

    mutable std::mutex mutex_;
    std::unordered_map<uint32_t, std::unordered_map<std::string,
                                                    RuntimeTaskCallback>>
        callbacks_;
};
