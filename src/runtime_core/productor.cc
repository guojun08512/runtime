#include "runtime_core/productor.h"
#include "runtime_core/global.h"
#include "runtime_core/tasks.h"
#include <iostream>

void TaskProducer(int runId) {
  auto *cfg = Config::Instance();

  // 计算任务（周期 + 优先级调度）
  const std::array<int,4> intervals = {1, 2, 10, 100};
  for (int i = 0; i < 2; ++i) {
    Config::TaskData data{Config::TaskType::IECTASK, runId, i};
    data.taskLabel = "iectask-" + std::to_string(runId) + "-" + std::to_string(i);
    data.isPeriodic = true;
    data.intervalMs = intervals[i];
    data.priority = 10 - i;           // 优先级由 10 降到 7

    auto *task = new IecTask(data);
    {
      std::lock_guard<std::mutex> lock(cfg->periodicTasksMutex);
      cfg->periodicTasks.push_back({false, task, nullptr, data, std::chrono::steady_clock::now()});
    }

    std::cout << "[生产者] 轮次" << runId << " | 周期计算任务 " << data.taskLabel
              << " interval=" << data.intervalMs << "ms priority=" << data.priority << " 注册\n";
  }

  // 文件 IO（周期任务）
  {
    Config::TaskData data{Config::TaskType::FILE_IO, runId, 0};
    data.state = Config::TaskState::PENDING;
    data.taskLabel = "file-" + std::to_string(runId) + "-0";
    data.isPeriodic = true;
    data.intervalMs = 1;
    data.priority = 12;
    auto *task = new FileIOConsumerTask(data);
    cfg->ioTasksPending.fetch_add(1, std::memory_order_relaxed);
    task->threadNum = cfg->ioThreadStartIndex + (uint32_t)Config::IOThreadId::FILE_IO;

    {
      std::lock_guard<std::mutex> lock(cfg->periodicTasksMutex);
      cfg->periodicTasks.push_back({true, nullptr, task, data, std::chrono::steady_clock::now()});
    }

    std::cout << "[生产者] 轮次" << runId << " | 周期文件IO 任务 " << data.taskLabel << " 每 " << data.intervalMs << "ms 注册\n";
  }

  // 网络 IO（周期任务）
  // for (int i = 0; i < 3; ++i) {
  //   Config::TaskData data{Config::TaskType::NETWORK_IO, runId, i};
  //   data.state = Config::TaskState::PENDING;
  //   data.taskLabel = "net-" + std::to_string(runId) + "-" + std::to_string(i);
  //   data.isPeriodic = true;
  //   data.intervalMs = 200;

  //   auto *task = new NetworkIOConsumerTask(data);
  //   cfg->ioTasksPending.fetch_add(1, std::memory_order_relaxed);
  //   task->threadNum = cfg->ioThreadStartIndex +
  //                     (uint32_t)Config::IOThreadId::NETWORK_IO_0 + i;

  //   {
  //     std::lock_guard<std::mutex> lock(cfg->periodicTasksMutex);
  //     cfg->periodicTasks.push_back({true, nullptr, task, data, std::chrono::steady_clock::now()});
  //   }

  //   std::cout << "[生产者] 轮次" << runId << " | 周期网络IO 任务 " << data.taskLabel << " 每 " << data.intervalMs << "ms 注册\n";
  // }
}
