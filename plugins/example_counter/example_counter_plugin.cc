#include "runtime_plugin/plugin_api.h"

#include <atomic>
#include <cstdio>

namespace {

const RtHostApiV1 *g_host = nullptr;
std::atomic<uint64_t> g_cycles{0};

void Log(const char *message) {
    if (g_host != nullptr && g_host->log != nullptr) {
        g_host->log(0, message);
    }
}

RtResult RT_PLUGIN_CALL Init(const RtHostApiV1 *host) {
    if (host == nullptr || host->abi_version != RT_PLUGIN_ABI_V1 ||
        host->struct_size < sizeof(RtHostApiV1)) {
        return RT_ERR_ABI;
    }
    g_host = host;
    Log("counter plugin initialized");
    return RT_OK;
}

void RT_PLUGIN_CALL Shutdown() {
    Log("counter plugin shutdown");
    g_host = nullptr;
}

RtResult RT_PLUGIN_CALL HookFunction(uint32_t hook,
                                     uintptr_t param1,
                                     uintptr_t param2) {
    switch (hook) {
    case RT_HOOK_INIT_SYSTEM:
        Log("counter plugin hook INIT_SYSTEM");
        break;
    case RT_HOOK_INIT:
        Log("counter plugin hook INIT");
        break;
    case RT_HOOK_INIT2:
        g_cycles.store(0, std::memory_order_relaxed);
        Log("counter plugin hook INIT2");
        break;
    case RT_HOOK_INIT3:
        Log("counter plugin hook INIT3");
        break;
    case RT_HOOK_INIT_TASKS:
        Log("counter plugin hook INIT_TASKS");
        break;
    case RT_HOOK_INIT_COMM:
        Log("counter plugin hook INIT_COMM");
        break;
    case RT_HOOK_COMM_CYCLE: {
        const uint64_t cycle =
            g_cycles.fetch_add(1, std::memory_order_relaxed) + 1;
        if (cycle == 1 || cycle % 1000 == 0) {
            char message[128];
            std::snprintf(message,
                          sizeof(message),
                          "counter plugin hook COMM_CYCLE: %llu",
                          static_cast<unsigned long long>(cycle));
            Log(message);
        }
        break;
    }
    case RT_HOOK_EXIT_COMM:
        Log("counter plugin hook EXIT_COMM");
        break;
    case RT_HOOK_EXIT_TASKS:
        Log("counter plugin hook EXIT_TASKS");
        break;
    case RT_HOOK_EXIT3:
        Log("counter plugin hook EXIT3");
        break;
    case RT_HOOK_EXIT2:
        Log("counter plugin hook EXIT2");
        break;
    case RT_HOOK_EXIT:
        Log("counter plugin hook EXIT");
        break;
    case RT_HOOK_EXIT_SYSTEM:
        Log("counter plugin hook EXIT_SYSTEM");
        break;
    default:
        break;
    }
    return RT_OK;
}

const RtPluginV1 kPlugin = {
    RT_PLUGIN_ABI_V1,
    sizeof(RtPluginV1),
    "runtime.example_counter",
    "Runtime Example Counter Plugin",
    0x00010000,
    nullptr,
    0,
    Init,
    Shutdown,
    HookFunction,
};

} // namespace

extern "C" RT_PLUGIN_EXPORT const RtPluginV1 *RT_PLUGIN_CALL
runtime_plugin_query_v1(uint32_t host_abi_version) {
    return host_abi_version == RT_PLUGIN_ABI_V1 ? &kPlugin : nullptr;
}
