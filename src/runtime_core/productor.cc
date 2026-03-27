#include "runtime_core/productor.h"
#include "runtime_core/global.h"
#include "runtime_core/tasks.h"
#include <iostream>

void TaskProducer(int runId) {
  auto *cfg = Config::Instance();
  std::cout << "\n==================== 生产者 轮次 " << runId
            << " ====================\n";

  // 计算任务
  for (int i = 0; i < 4; ++i) {
    Config::TaskData data{Config::TaskType::COMPUTE, runId, i};
    auto *task = new ComputeConsumerTask(data);
    cfg->ts.AddTaskSetToPipe(task);
  }

  // 文件 IO
  {
    Config::TaskData data{Config::TaskType::FILE_IO, runId, 0};
    auto *task = new FileIOConsumerTask(data);
    task->threadNum =
        cfg->ioThreadStartIndex + (uint32_t)Config::IOThreadId::FILE_IO;
    cfg->ts.AddPinnedTask(task);
  }

  // 网络 IO
  for (int i = 0; i < 3; ++i) {
    Config::TaskData data{Config::TaskType::NETWORK_IO, runId, i};
    auto *task = new NetworkIOConsumerTask(data);
    task->threadNum = cfg->ioThreadStartIndex +
                      (uint32_t)Config::IOThreadId::NETWORK_IO_0 + i;
    cfg->ts.AddPinnedTask(task);
  }
}