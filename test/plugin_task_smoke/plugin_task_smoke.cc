#include "runtime_core/global.h"
#include "runtime_core/logging.h"
#include "runtime_core/plugin_control.h"
#include "runtime_core/tasks.h"
#include "runtime_plugin/plugin_manager.h"

#include <filesystem>
#include <chrono>
#include <sstream>
#include <string>
#include <thread>

int main(int argc, char **argv) {
    runtime_logging::Init(argv[0]);
    if (argc != 2) {
        LOG(ERROR) << "usage: runtime_plugin_task_smoke <plugin.so>";
        return 2;
    }

    auto *cfg = Config::Instance();
    runtime_plugin::PluginManager manager;
    cfg->plugin_manager = &manager;
    cfg->ioTasksCompleted.store(0);
    cfg->ioTasksFailed.store(0);

    const std::string plugin_path =
        std::filesystem::absolute(argv[1]).string();
    std::ostringstream load_command;
    load_command << R"({"command":"plugin.load",)"
                 << R"("pluginId":"runtime.example_plugin",)"
                 << R"("path":")" << plugin_path << R"(",)"
                 << R"("autoStart":false})";
    if (!HandlePluginControlCommand(load_command.str(), cfg, 1) ||
        !manager.HasPlugin("runtime.example_plugin")) {
        LOG(ERROR) << "plugin.load command did not load the plugin";
        return 1;
    }

    std::ostringstream task_command;
    task_command << R"({"command":"plugin.io_task",)"
            << R"("label":"plugin-task-smoke",)"
            << R"("intervalMs":1,)"
            << R"("priority":10,)"
            << R"("pluginId":"runtime.example_plugin",)"
            << R"("driverType":"runtime.example_io",)"
            << R"("readBytes":4,)"
            << R"("writeBytes":4,)"
            << R"("parameterId":1,)"
            << R"("parameterBytes":4})";

    if (!HandlePluginControlCommand(task_command.str(), cfg, 1)) {
        LOG(ERROR) << "plugin command was not handled";
        return 1;
    }

    std::ostringstream hook_command;
    hook_command << R"({"command":"plugin.hook_task",)"
                 << R"("label":"plugin-hook-smoke",)"
                 << R"("intervalMs":1,)"
                 << R"("priority":9,)"
                 << R"("pluginId":"runtime.example_plugin"})";
    if (!HandlePluginControlCommand(hook_command.str(), cfg, 1)) {
        LOG(ERROR) << "hook command was not handled";
        return 1;
    }

    PluginIOTask *io_task = nullptr;
    PluginHookTask *hook_task = nullptr;
    {
        std::lock_guard<std::mutex> lock(cfg->periodicTasksMutex);
        if (cfg->periodicTasks.size() != 2) {
            LOG(ERROR) << "plugin tasks were not installed";
            return 1;
        }
        for (const auto &entry : cfg->periodicTasks) {
            if (entry.data.type == Config::TaskType::PLUGIN_IO) {
                io_task = static_cast<PluginIOTask *>(entry.taskSet);
            } else if (entry.data.type == Config::TaskType::PLUGIN_HOOK) {
                hook_task = static_cast<PluginHookTask *>(entry.taskSet);
            }
        }
    }
    if (io_task == nullptr || hook_task == nullptr) {
        LOG(ERROR) << "plugin task handles are null";
        return 1;
    }

    enki::TaskSchedulerConfig scheduler_config;
    scheduler_config.numTaskThreadsToCreate +=
        static_cast<uint32_t>(Config::IOThreadId::NUM);
    cfg->ts.Initialize(scheduler_config);
    cfg->ioThreadStartIndex =
        cfg->ts.GetNumTaskThreads() -
        static_cast<uint32_t>(Config::IOThreadId::NUM);

    IECLoopTask io_loops[static_cast<uint32_t>(Config::IOThreadId::NUM)];
    for (uint32_t i = 0; i < static_cast<uint32_t>(Config::IOThreadId::NUM);
         ++i) {
        io_loops[i].threadNum = cfg->ioThreadStartIndex + i;
        cfg->ts.AddPinnedTask(&io_loops[i]);
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (cfg->ioTasksCompleted.load() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    cfg->stopFlag = true;
    cfg->ts.WaitforAllAndShutdown();

    auto *driver = io_task->GetDriver();
    if (io_task->GetState() != Config::TaskState::COMPLETED ||
        hook_task->GetState() != Config::TaskState::COMPLETED ||
        cfg->ioTasksCompleted.load() == 0 ||
        cfg->ioTasksFailed.load() != 0 ||
        driver == nullptr) {
        LOG(ERROR) << "plugin task did not execute its driver interface";
        return 1;
    }

    std::string error;
    if (manager.StopDriver(driver, &error) != RT_OK ||
        manager.DestroyDriver(driver, &error) != RT_OK ||
        manager.UnloadPlugin("runtime.example_plugin", &error) != RT_OK) {
        LOG(ERROR) << "plugin cleanup failed: " << error;
        return 1;
    }
    {
        std::lock_guard<std::mutex> lock(cfg->periodicTasksMutex);
        cfg->periodicTasks.clear();
    }
    delete io_task;
    delete hook_task;
    cfg->plugin_manager = nullptr;

    LOG(INFO) << "plugin task smoke test passed";
    return 0;
}
