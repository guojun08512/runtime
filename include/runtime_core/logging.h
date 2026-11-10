#pragma once

#include <glog/logging.h>

#include <mutex>

namespace runtime_logging {

inline void Init(const char *program_name) {
    static std::once_flag init_flag;
    std::call_once(init_flag,
                   [program_name]() {
                       google::InitGoogleLogging(program_name);
                       google::SetStderrLogging(google::INFO);
                   });
}

} // namespace runtime_logging
