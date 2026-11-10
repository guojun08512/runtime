#pragma once

#include "runtime_core/global.h"

#include <cstdint>
#include <string>

// Returns true when the JSON is a plugin command, including command failures.
bool HandlePluginControlCommand(const std::string &json,
                                Config *cfg,
                                int32_t runId);
