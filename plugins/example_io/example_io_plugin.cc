#include "runtime_plugin/plugin_api.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>

namespace {

const RtHostApiV1 *g_host = nullptr;
std::atomic<uint64_t> g_hook_cycles{0};

struct ExampleIoDriver {
    const RtHostApiV1 *host = nullptr;
    bool running = false;
    bool identified = false;
    unsigned char pattern = 0x10;
    unsigned char outputs[32]{};
    unsigned char parameters[16] = {
        0x11, 0x22, 0x33, 0x44,
    };
    uint32_t output_count = 0;
    uint32_t config_revision = 0;
    uint32_t mapping_revision = 0;
    uint32_t scan_count = 0;
    uint32_t bus_cycles = 0;
    uint32_t watchdog_count = 0;
    char config[256]{};
    char mapping[256]{};
};

void Log(const char *message) {
    if (g_host != nullptr && g_host->log != nullptr) {
        g_host->log(0, message);
    }
}

RtResult RT_PLUGIN_CALL Create(const RtHostApiV1 *host,
                               const char *json_config,
                               void **instance) {
    (void)json_config;
    if (host == nullptr || instance == nullptr ||
        host->abi_version != RT_PLUGIN_ABI_V1 ||
        host->struct_size < sizeof(RtHostApiV1)) {
        return RT_ERR_PARAMETER;
    }

    auto *driver = new (std::nothrow) ExampleIoDriver();
    if (driver == nullptr) {
        return RT_ERR_NOMEMORY;
    }
    driver->host = host;
    *instance = driver;
    return RT_OK;
}

void RT_PLUGIN_CALL Destroy(void *instance) {
    delete static_cast<ExampleIoDriver *>(instance);
}

RtResult RT_PLUGIN_CALL Configure(void *instance, const char *json_config) {
    auto *driver = static_cast<ExampleIoDriver *>(instance);
    if (driver == nullptr || json_config == nullptr) {
        return RT_ERR_PARAMETER;
    }
    std::strncpy(driver->config,
                 json_config,
                 sizeof(driver->config) - 1);
    driver->config[sizeof(driver->config) - 1] = '\0';
    ++driver->config_revision;
    Log("example I/O driver configured");
    return RT_OK;
}

RtResult RT_PLUGIN_CALL Start(void *instance) {
    auto *driver = static_cast<ExampleIoDriver *>(instance);
    if (driver == nullptr) {
        return RT_ERR_PARAMETER;
    }
    driver->running = true;
    Log("example I/O driver started");
    return RT_OK;
}

void RT_PLUGIN_CALL Stop(void *instance) {
    auto *driver = static_cast<ExampleIoDriver *>(instance);
    if (driver != nullptr) {
        driver->running = false;
        Log("example I/O driver stopped");
    }
}

RtResult RT_PLUGIN_CALL ReadInputs(void *instance,
                                    RtIoChannelV1 *channels,
                                    uint32_t count) {
    auto *driver = static_cast<ExampleIoDriver *>(instance);
    if (driver == nullptr || !driver->running ||
        (count != 0 && channels == nullptr)) {
        return RT_ERR_PARAMETER;
    }

    for (uint32_t i = 0; i < count; ++i) {
        if (channels[i].data == nullptr) {
            return RT_ERR_PARAMETER;
        }
        std::memset(channels[i].data,
                    static_cast<int>(driver->pattern + i),
                    channels[i].size_bytes);
    }
    ++driver->pattern;
    return RT_OK;
}

RtResult RT_PLUGIN_CALL WriteOutputs(void *instance,
                                      RtIoChannelV1 *channels,
                                      uint32_t count) {
    auto *driver = static_cast<ExampleIoDriver *>(instance);
    if (driver == nullptr || !driver->running ||
        (count != 0 && channels == nullptr)) {
        return RT_ERR_PARAMETER;
    }

    driver->output_count = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (channels[i].data == nullptr) {
            return RT_ERR_PARAMETER;
        }
        const uint32_t copy_size =
            channels[i].size_bytes <
                    sizeof(driver->outputs) - driver->output_count
                ? channels[i].size_bytes
                : static_cast<uint32_t>(sizeof(driver->outputs) -
                                        driver->output_count);
        std::memcpy(driver->outputs + driver->output_count,
                    channels[i].data,
                    copy_size);
        driver->output_count += copy_size;
    }
    return RT_OK;
}

RtResult RT_PLUGIN_CALL ScanModules(void *instance) {
    auto *driver = static_cast<ExampleIoDriver *>(instance);
    if (driver == nullptr) {
        return RT_ERR_PARAMETER;
    }
    ++driver->scan_count;
    return RT_OK;
}

RtResult RT_PLUGIN_CALL GetDiagnosis(void *instance, uint32_t *diagnosis) {
    auto *driver = static_cast<ExampleIoDriver *>(instance);
    if (driver == nullptr || diagnosis == nullptr) {
        return RT_ERR_PARAMETER;
    }
    *diagnosis = 0x01;
    if (driver->running) {
        *diagnosis |= 0x02;
    }
    if (driver->identified) {
        *diagnosis |= 0x04;
    }
    if (driver->config_revision != 0) {
        *diagnosis |= 0x08;
    }
    if (driver->mapping_revision != 0) {
        *diagnosis |= 0x10;
    }
    return RT_OK;
}

RtResult RT_PLUGIN_CALL TriggerWatchdog(void *instance) {
    auto *driver = static_cast<ExampleIoDriver *>(instance);
    if (driver == nullptr) {
        return RT_ERR_PARAMETER;
    }
    ++driver->watchdog_count;
    return RT_OK;
}

RtResult RT_PLUGIN_CALL GetInfo(void *instance, RtIoDriverInfoV1 *info) {
    auto *driver = static_cast<ExampleIoDriver *>(instance);
    if (driver == nullptr || info == nullptr) {
        return RT_ERR_PARAMETER;
    }
    *info = {
        RT_PLUGIN_ABI_V1,
        sizeof(RtIoDriverInfoV1),
        "Runtime Example I/O Driver",
        "Example I/O Device",
        "Runtime",
        "1.0.0",
        40100,
        0x00010000,
    };
    return RT_OK;
}

RtResult RT_PLUGIN_CALL UpdateConfiguration(void *instance,
                                            const char *json_config) {
    auto *driver = static_cast<ExampleIoDriver *>(instance);
    if (driver == nullptr || json_config == nullptr) {
        return RT_ERR_PARAMETER;
    }
    std::strncpy(driver->config,
                 json_config,
                 sizeof(driver->config) - 1);
    driver->config[sizeof(driver->config) - 1] = '\0';
    ++driver->config_revision;
    return RT_OK;
}

RtResult RT_PLUGIN_CALL UpdateMapping(void *instance,
                                      const char *json_mapping) {
    auto *driver = static_cast<ExampleIoDriver *>(instance);
    if (driver == nullptr || json_mapping == nullptr) {
        return RT_ERR_PARAMETER;
    }
    std::strncpy(driver->mapping,
                 json_mapping,
                 sizeof(driver->mapping) - 1);
    driver->mapping[sizeof(driver->mapping) - 1] = '\0';
    ++driver->mapping_revision;
    return RT_OK;
}

RtResult RT_PLUGIN_CALL Identify(void *instance) {
    auto *driver = static_cast<ExampleIoDriver *>(instance);
    if (driver == nullptr) {
        return RT_ERR_PARAMETER;
    }
    driver->identified = true;
    Log("example I/O driver identified");
    return RT_OK;
}

RtResult RT_PLUGIN_CALL StartBusCycle(void *instance) {
    auto *driver = static_cast<ExampleIoDriver *>(instance);
    if (driver == nullptr || !driver->running) {
        return RT_ERR_PARAMETER;
    }
    ++driver->bus_cycles;
    if (driver->output_count != 0) {
        driver->pattern = driver->outputs[0];
    }
    return RT_OK;
}

RtResult RT_PLUGIN_CALL ReadParameter(void *instance,
                                       uint32_t parameter_id,
                                       void *data,
                                       uint32_t size_bytes,
                                       uint32_t bit_offset) {
    if (instance == nullptr || data == nullptr || bit_offset != 0) {
        return RT_ERR_PARAMETER;
    }

    auto *driver = static_cast<ExampleIoDriver *>(instance);
    if (parameter_id == 1) {
        const uint32_t copy_size =
            size_bytes < sizeof(driver->parameters)
                ? size_bytes
                : static_cast<uint32_t>(sizeof(driver->parameters));
        std::memcpy(data, driver->parameters, copy_size);
        return RT_OK;
    }

    return RT_ERR_NOT_SUPPORTED;
}

RtResult RT_PLUGIN_CALL WriteParameter(void *instance,
                                        uint32_t parameter_id,
                                        const void *data,
                                        uint32_t size_bytes,
                                        uint32_t bit_offset) {
    if (instance == nullptr || data == nullptr || bit_offset != 0) {
        return RT_ERR_PARAMETER;
    }
    auto *driver = static_cast<ExampleIoDriver *>(instance);
    if (parameter_id == 1) {
        const uint32_t copy_size =
            size_bytes < sizeof(driver->parameters)
                ? size_bytes
                : static_cast<uint32_t>(sizeof(driver->parameters));
        std::memcpy(driver->parameters, data, copy_size);
        return RT_OK;
    }
    if (parameter_id != 2) {
        return RT_ERR_NOT_SUPPORTED;
    }

    const uint32_t copy_size =
        size_bytes < sizeof(driver->outputs) ? size_bytes
                                            : sizeof(driver->outputs);
    std::memcpy(driver->outputs, data, copy_size);
    driver->output_count = copy_size;
    return RT_OK;
}

const RtIoDriverOpsV1 kIoOps = {
    RT_PLUGIN_ABI_V1,
    sizeof(RtIoDriverOpsV1),
    Create,
    Destroy,
    Configure,
    Start,
    Stop,
    ReadInputs,
    WriteOutputs,
    ScanModules,
    GetDiagnosis,
    TriggerWatchdog,
    GetInfo,
    UpdateConfiguration,
    UpdateMapping,
    Identify,
    StartBusCycle,
};

const RtIoParameterOpsV1 kParameterOps = {
    RT_PLUGIN_ABI_V1,
    sizeof(RtIoParameterOpsV1),
    ReadParameter,
    WriteParameter,
};

const RtDriverDescV1 kDriver = {
    RT_PLUGIN_ABI_V1,
    sizeof(RtDriverDescV1),
    "runtime.example_io",
    "Runtime Example I/O Driver",
    "Example I/O Device",
    "Runtime",
    "1.0.0",
    0x00010000,
    &kIoOps,
    &kParameterOps,
};

const RtDriverDescV1 *const kDrivers[] = {&kDriver};

RtResult RT_PLUGIN_CALL PluginInit(const RtHostApiV1 *host) {
    if (host == nullptr || host->abi_version != RT_PLUGIN_ABI_V1 ||
        host->struct_size < sizeof(RtHostApiV1)) {
        return RT_ERR_ABI;
    }
    g_host = host;
    Log("example plugin initialized");
    return RT_OK;
}

void RT_PLUGIN_CALL PluginShutdown() {
    Log("example plugin shutdown");
    g_host = nullptr;
}

RtResult RT_PLUGIN_CALL HookFunction(uint32_t hook,
                                     uintptr_t param1,
                                     uintptr_t param2) {
    (void)param1;
    (void)param2;

    switch (hook) {
    case RT_HOOK_INIT_SYSTEM:
        Log("example plugin hook INIT_SYSTEM");
        break;
    case RT_HOOK_INIT:
        Log("example plugin hook INIT");
        break;
    case RT_HOOK_INIT2:
        g_hook_cycles.store(0, std::memory_order_relaxed);
        Log("example plugin hook INIT2");
        break;
    case RT_HOOK_INIT3:
        Log("example plugin hook INIT3");
        break;
    case RT_HOOK_INIT_TASKS:
        Log("example plugin hook INIT_TASKS");
        break;
    case RT_HOOK_INIT_COMM:
        Log("example plugin hook INIT_COMM");
        break;
    case RT_HOOK_COMM_CYCLE: {
        const uint64_t cycle =
            g_hook_cycles.fetch_add(1, std::memory_order_relaxed) + 1;
        if (cycle == 1 || cycle % 1000 == 0) {
            char message[128];
            std::snprintf(message,
                          sizeof(message),
                          "example plugin hook IO COMM_CYCLE: %llu",
                          static_cast<unsigned long long>(cycle));
            Log(message);
        }
        break;
    }
    case RT_HOOK_EXIT_COMM:
        Log("example plugin hook EXIT_COMM");
        break;
    case RT_HOOK_EXIT_TASKS:
        Log("example plugin hook EXIT_TASKS");
        break;
    case RT_HOOK_EXIT3:
        Log("example plugin hook EXIT3");
        break;
    case RT_HOOK_EXIT2:
        Log("example plugin hook EXIT2");
        break;
    case RT_HOOK_EXIT:
        Log("example plugin hook EXIT");
        break;
    case RT_HOOK_EXIT_SYSTEM:
        Log("example plugin hook EXIT_SYSTEM");
        break;
    default:
        break;
    }
    return RT_OK;
}

const RtPluginV1 kPlugin = {
    RT_PLUGIN_ABI_V1,
    sizeof(RtPluginV1),
    "runtime.example_plugin",
    "Runtime Example Plugin",
    0x00010000,
    kDrivers,
    1,
    PluginInit,
    PluginShutdown,
    HookFunction,
};

} // namespace

extern "C" RT_PLUGIN_EXPORT const RtPluginV1 *RT_PLUGIN_CALL
runtime_plugin_query_v1(uint32_t host_abi_version) {
    return host_abi_version == RT_PLUGIN_ABI_V1 ? &kPlugin : nullptr;
}
