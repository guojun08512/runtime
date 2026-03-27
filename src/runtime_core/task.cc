#include "runtime_core/tasks.h"
#include <chrono>
#include <iostream>
#include <thread>

ComputeConsumerTask::ComputeConsumerTask(const Config::TaskData &data)
    : m_data(data) {
  m_SetSize = 1;
}

void ComputeConsumerTask::ExecuteRange(enki::TaskSetPartition,
                                       uint32_t threadNum) {
  auto *cfg = Config::Instance();
  const bool isIOThread = threadNum >= cfg->ts.GetNumTaskThreads() -
                                           (uint32_t)Config::IOThreadId::NUM;

  if (isIOThread) {
    std::cout << "[错误] 计算任务在IO线程 " << threadNum << " 执行\n";
    return;
  }

  std::cout << "[计算消费者] 线程" << threadNum << " | 轮次" << m_data.runId
            << " | 任务" << m_data.taskId << " | 执行计算\n";

  std::this_thread::sleep_for(std::chrono::milliseconds(2));
}

// -----------------------------------------------------------------------------

void IOLoopTask::Execute() {
  auto *cfg = Config::Instance();
  std::cout << "[IO线程" << threadNum << "] 已启动\n";

  while (!cfg->ts.GetIsShutdownRequested() && !cfg->stopFlag) {
    cfg->ts.WaitForNewPinnedTasks();
    cfg->ts.RunPinnedTasks();
  }

  std::cout << "[IO线程" << threadNum << "] 已退出\n";
}

// -----------------------------------------------------------------------------

FileIOConsumerTask::FileIOConsumerTask(const Config::TaskData &data)
    : m_data(data) {}

void FileIOConsumerTask::Execute() {
  std::cout << "[文件IO] 线程" << threadNum << " | 轮次" << m_data.runId
            << " | 任务" << m_data.taskId << " | 读取文件\n";

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

// -----------------------------------------------------------------------------

NetworkIOConsumerTask::NetworkIOConsumerTask(const Config::TaskData &data)
    : m_data(data) {}

void NetworkIOConsumerTask::Execute() {
  std::cout << "[网络IO] 线程" << threadNum << " | 轮次" << m_data.runId
            << " | 任务" << m_data.taskId << " | 收发数据\n";

  std::this_thread::sleep_for(std::chrono::milliseconds(5));
}