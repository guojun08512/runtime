#include "runtime_core/global.h"
#include "runtime_core/logging.h"
#include "runtime_core/productor.h"
#include "runtime_core/tasks.h"
#include "runtime_plugin/plugin_manager.h"
#include <cstdlib>
#include <sched.h>
#include <cstring>
#include <cerrno>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>
#include <filesystem>
#include <memory>
#include <vector>

std::atomic<bool> g_shutdown_requested(false);

void signal_handler(int signum) {
  if (signum == SIGINT) {
    g_shutdown_requested = true;
  }
}

static std::string ResolvePluginManifest(int argc, char **argv,
                                         bool *explicitly_requested,
                                         std::string *error) {
  *explicitly_requested = false;
  for (int i = 1; i < argc; ++i) {
    const std::string argument(argv[i]);
    if (argument == "--plugins") {
      *explicitly_requested = true;
      if (i + 1 >= argc) {
        *error = "--plugins requires a manifest path";
        return {};
      }
      return argv[++i];
    }
    const std::string prefix = "--plugins=";
    if (argument.compare(0, prefix.size(), prefix) == 0) {
      *explicitly_requested = true;
      const std::string path = argument.substr(prefix.size());
      if (path.empty()) {
        *error = "--plugins requires a manifest path";
      }
      return path;
    }
  }

  const char *from_environment = std::getenv("RUNTIME_PLUGIN_CONFIG");
  if (from_environment != nullptr && from_environment[0] != '\0') {
    *explicitly_requested = true;
    return from_environment;
  }

  std::vector<std::filesystem::path> candidates = {
      std::filesystem::current_path() / "plugins.json",
      std::filesystem::current_path() / "config" / "plugins.json",
  };

  std::error_code path_error;
  const std::filesystem::path executable =
      std::filesystem::absolute(argv[0], path_error);
  if (!path_error) {
    candidates.push_back(executable.parent_path() / "plugins.json");
    candidates.push_back(executable.parent_path().parent_path() /
                         "plugins.json");
  }

  for (const auto &candidate : candidates) {
    if (std::filesystem::exists(candidate, path_error) && !path_error) {
      return candidate.string();
    }
    path_error.clear();
  }
  return {};
}

int main(int argc, char **argv) {
  runtime_logging::Init(argv[0]);
  auto *cfg = Config::Instance();

  // 注册信号处理器
  std::signal(SIGINT, signal_handler);

  // 插件必须在实时调度器和 IEC 线程启动前完成加载。
  bool manifest_requested = false;
  std::string manifest_error;
  const std::string manifest_path =
      ResolvePluginManifest(argc, argv, &manifest_requested, &manifest_error);
  if (!manifest_error.empty()) {
    LOG(ERROR) << "[plugin] " << manifest_error;
    return 2;
  }

  runtime_plugin::PluginManager plugin_manager;
  if (!manifest_path.empty()) {
    std::string error;
    const RtResult result = plugin_manager.LoadManifest(manifest_path, &error);
    if (result != RT_OK) {
      LOG(ERROR) << "[plugin] 加载清单失败: " << manifest_path
                 << " | " << error;
      return 1;
    }
    LOG(INFO) << "[plugin] 已加载 " << plugin_manager.plugin_count()
              << " 个插件";
  } else if (manifest_requested) {
    LOG(ERROR) << "[plugin] 插件清单路径为空";
    return 2;
  } else {
    LOG(WARNING) << "[plugin] 未找到 plugins.json，插件加载已跳过";
  }
  cfg->plugin_manager = &plugin_manager;

  const char *hook_interval_env =
      std::getenv("RUNTIME_PLUGIN_HOOK_INTERVAL_MS");
  if (hook_interval_env != nullptr && hook_interval_env[0] != '\0') {
    const unsigned long interval = std::strtoul(hook_interval_env, nullptr, 10);
    cfg->pluginHookCycleIntervalMs = static_cast<uint32_t>(interval);
  }
  const char *hook_priority_env =
      std::getenv("RUNTIME_PLUGIN_HOOK_PRIORITY");
  if (hook_priority_env != nullptr && hook_priority_env[0] != '\0') {
    cfg->pluginHookCyclePriority =
        static_cast<int>(std::strtol(hook_priority_env, nullptr, 10));
  }
  
  // 初始化调度器
  enki::TaskSchedulerConfig schedulerConfig;
  schedulerConfig.numTaskThreadsToCreate += (uint32_t)Config::IOThreadId::NUM;
  cfg->ts.Initialize(schedulerConfig);

  cfg->ioThreadStartIndex =
      cfg->ts.GetNumTaskThreads() - (uint32_t)Config::IOThreadId::NUM;

  LOG(INFO) << "总线程：" << cfg->ts.GetNumTaskThreads() << " | 计算线程："
            << cfg->ts.GetNumTaskThreads() - (uint32_t)Config::IOThreadId::NUM
            << " | IO线程：" << (uint32_t)Config::IOThreadId::NUM;

  // 启动 IEC 常驻线程
  IECLoopTask ioLoops[(uint32_t)Config::IOThreadId::NUM];
  for (uint32_t i = 0; i < (uint32_t)Config::IOThreadId::NUM; ++i) {
    ioLoops[i].threadNum = cfg->ioThreadStartIndex + i;
    cfg->ts.AddPinnedTask(&ioLoops[i]);
  }

  std::unique_ptr<PluginHookCycleTask> plugin_hook_cycle;
  if (cfg->pluginHookCycleIntervalMs > 0) {
    Config::TaskData hook_task_data;
    hook_task_data.type = Config::TaskType::PLUGIN_HOOK;
    hook_task_data.runId = cfg->runCount.load();
    hook_task_data.taskId = 0;
    hook_task_data.taskLabel = "plugin-hook-cycle-all";
    hook_task_data.isPeriodic = true;
    hook_task_data.intervalMs = cfg->pluginHookCycleIntervalMs;
    hook_task_data.priority = cfg->pluginHookCyclePriority;

    plugin_hook_cycle =
        std::make_unique<PluginHookCycleTask>(hook_task_data);
    {
      std::lock_guard<std::mutex> lock(cfg->periodicTasksMutex);
      cfg->periodicTasks.push_back(
          {false, plugin_hook_cycle.get(), nullptr, hook_task_data,
           std::chrono::steady_clock::now()});
    }
    LOG(INFO) << "[plugin] 全局 Hook 周期: "
              << cfg->pluginHookCycleIntervalMs << "ms，优先级 "
              << cfg->pluginHookCyclePriority;
  }

  // 生产消费循环 - TaskProducer 在后台线程中运行
  std::thread producerThread([](){ TaskProducer(Config::Instance()->runCount); });
  
  // 主线程循环，直到收到Ctrl+C信号
  LOG(INFO) << "[主程序] 运行中... 按Ctrl+C退出";
  while (!g_shutdown_requested) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  LOG(INFO) << "[主程序] 收到Ctrl+C信号，准备退出...";
  
  // 标记停止
  cfg->stopFlag = true;
  
  // 短暂等待生产者线程退出（最多500ms）
  LOG(INFO) << "[主程序] 正在停止...";
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  
  // 尝试等待生产者线程，但不要无限期等待
  while (producerThread.joinable()) {
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) {
      LOG(WARNING) << "[主程序] 生产者线程未在规定时间内退出，继续关闭";
      break;
    }
    if (producerThread.joinable()) {
      producerThread.join();
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  LOG(INFO) << "[RunSummary] 任务统计:"
            << " pending=" << cfg->ioTasksPending.load()
            << " running=" << cfg->ioTasksRunning.load()
            << " completed=" << cfg->ioTasksCompleted.load()
            << " failed=" << cfg->ioTasksFailed.load();

  // 最后等待所有任务和scheduler完成
  LOG(INFO) << "[主程序] 等待任务调度器关闭...";
  cfg->ts.WaitforAllAndShutdown();

  // 调度器停止后，插件实例才能安全停止和卸载。
  {
    std::lock_guard<std::mutex> lock(cfg->periodicTasksMutex);
    cfg->periodicTasks.clear();
  }
  plugin_hook_cycle.reset();
  plugin_manager.StopAllDrivers();
  plugin_manager.DestroyAllDrivers();
  plugin_manager.UnloadAll();
  cfg->plugin_manager = nullptr;

  LOG(INFO) << "程序已退出";
  return 0;
}
