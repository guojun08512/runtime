#include "runtime_plugin/plugin_manager.h"
#include "runtime_core/logging.h"

#include <string>

int main(int argc, char **argv) {
    runtime_logging::Init(argv[0]);
    if (argc != 3) {
        LOG(ERROR) << "usage: runtime_multi_plugin_smoke <io.so> <counter.so>";
        return 2;
    }

    runtime_plugin::PluginManager manager;
    runtime_plugin::LoadOptions options;
    std::string io_id;
    std::string counter_id;
    std::string error;

    if (manager.LoadPlugin(argv[1], options, &io_id, &error) != RT_OK) {
        LOG(ERROR) << "load io plugin failed: " << error;
        return 1;
    }
    if (manager.LoadPlugin(argv[2], options, &counter_id, &error) != RT_OK) {
        LOG(ERROR) << "load counter plugin failed: " << error;
        return 1;
    }
    if (manager.plugin_count() != 2 ||
        !manager.HasPlugin("runtime.example_plugin") ||
        !manager.HasPlugin("runtime.example_counter")) {
        LOG(ERROR) << "both plugins were not registered independently";
        return 1;
    }

    for (uint32_t i = 0; i < 100; ++i) {
        if (manager.CallAllHooks(RT_HOOK_COMM_CYCLE, i, 0, &error) != RT_OK) {
            LOG(ERROR) << "multi-plugin hook dispatch failed: " << error;
            return 1;
        }
    }

    if (manager.UnloadPlugin(counter_id, &error) != RT_OK ||
        manager.UnloadPlugin(io_id, &error) != RT_OK ||
        manager.plugin_count() != 0) {
        LOG(ERROR) << "multi-plugin cleanup failed: " << error;
        return 1;
    }

    LOG(INFO) << "multi plugin smoke test passed";
    return 0;
}
