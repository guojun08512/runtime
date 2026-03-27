#include "runtime_core/global.h"
#include "runtime_core/productor.h"
#include "runtime_core/tasks.h"
#include <iostream>

int main() {
  auto *cfg = Config::Instance();

  // 初始化调度器
  enki::TaskSchedulerConfig schedulerConfig;
  schedulerConfig.numTaskThreadsToCreate += (uint32_t)Config::IOThreadId::NUM;
  cfg->ts.Initialize(schedulerConfig);

  cfg->ioThreadStartIndex =
      cfg->ts.GetNumTaskThreads() - (uint32_t)Config::IOThreadId::NUM;

  std::cout << "总线程：" << cfg->ts.GetNumTaskThreads() << " | 计算线程："
            << cfg->ts.GetNumTaskThreads() - (uint32_t)Config::IOThreadId::NUM
            << " | IO线程：" << (uint32_t)Config::IOThreadId::NUM << "\n";

  // 启动 IO 常驻线程
  IOLoopTask ioLoops[(uint32_t)Config::IOThreadId::NUM];
  for (uint32_t i = 0; i < (uint32_t)Config::IOThreadId::NUM; ++i) {
    ioLoops[i].threadNum = cfg->ioThreadStartIndex + i;
    cfg->ts.AddPinnedTask(&ioLoops[i]);
  }

  // 生产消费循环
  const int TOTAL_RUNS = 5;
  for (cfg->runCount = 1; cfg->runCount <= TOTAL_RUNS; ++cfg->runCount) {
    TaskProducer(cfg->runCount);
    cfg->ts.WaitforAll();
    std::cout << "==================== 轮次 " << cfg->runCount
              << " 完成 ====================\n";
  }

  // 退出
  cfg->stopFlag = true;
  cfg->ts.WaitforAllAndShutdown();

  std::cout << "\n✅ 多文件工程版运行完成！\n";
  return 0;
}