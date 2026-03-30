#include "runtime_core/global.h"
#include "runtime_core/productor.h"
#include "runtime_core/tasks.h"
#include <iostream>
#include <sched.h>
#include <cstring>
#include <cerrno>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>

std::atomic<bool> g_shutdown_requested(false);

void signal_handler(int signum) {
  if (signum == SIGINT) {
    std::cout << "\n[主程序] 收到Ctrl+C信号，准备退出...\n";
    g_shutdown_requested = true;
  }
}

int main() {
  auto *cfg = Config::Instance();
  
  // 注册信号处理器
  std::signal(SIGINT, signal_handler);
  
  // 初始化调度器
  enki::TaskSchedulerConfig schedulerConfig;
  schedulerConfig.numTaskThreadsToCreate += (uint32_t)Config::IOThreadId::NUM;
  cfg->ts.Initialize(schedulerConfig);

  cfg->ioThreadStartIndex =
      cfg->ts.GetNumTaskThreads() - (uint32_t)Config::IOThreadId::NUM;

  std::cout << "总线程：" << cfg->ts.GetNumTaskThreads() << " | 计算线程："
            << cfg->ts.GetNumTaskThreads() - (uint32_t)Config::IOThreadId::NUM
            << " | IO线程：" << (uint32_t)Config::IOThreadId::NUM << "\n";

  // 启动 IEC 常驻线程
  IECLoopTask ioLoops[(uint32_t)Config::IOThreadId::NUM];
  for (uint32_t i = 0; i < (uint32_t)Config::IOThreadId::NUM; ++i) {
    ioLoops[i].threadNum = cfg->ioThreadStartIndex + i;
    cfg->ts.AddPinnedTask(&ioLoops[i]);
  }

  // 生产消费循环 - TaskProducer 在后台线程中运行
  std::thread producerThread([](){ TaskProducer(Config::Instance()->runCount); });
  
  // 主线程循环，直到收到Ctrl+C信号
  std::cout << "[主程序] 运行中... 按Ctrl+C退出\n";
  while (!g_shutdown_requested) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  
  // 标记停止
  cfg->stopFlag = true;
  
  // 短暂等待生产者线程退出（最多500ms）
  std::cout << "[主程序] 正在停止...\n";
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  
  // 尝试等待生产者线程，但不要无限期等待
  while (producerThread.joinable()) {
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) {
      std::cout << "[主程序] 生产者线程未在规定时间内退出，继续关闭\n";
      break;
    }
    if (producerThread.joinable()) {
      producerThread.join();
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  std::cout << "[RunSummary] 任务统计:"
            << " pending=" << cfg->ioTasksPending.load()
            << " running=" << cfg->ioTasksRunning.load()
            << " completed=" << cfg->ioTasksCompleted.load()
            << " failed=" << cfg->ioTasksFailed.load() << "\n";

  // 最后等待所有任务和scheduler完成
  std::cout << "[主程序] 等待任务调度器关闭...\n";
  cfg->ts.WaitforAllAndShutdown();

  std::cout << "\n✅ 程序已退出！\n";
  return 0;
}
