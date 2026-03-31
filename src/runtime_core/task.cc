#include "runtime_core/tasks.h"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>

// -----------------------------------------------------------------------------
void IECLoopTask::Execute() {
  auto *cfg = Config::Instance();
  std::cout << "[IEC:IO" << threadNum << "] 已启动\n";

  while (!cfg->ts.GetIsShutdownRequested() && !cfg->stopFlag) {
    bool isScheduler = (threadNum == cfg->ioThreadStartIndex);

    if (isScheduler) {
      // 周期性任务调度：根据 TaskData.intervalMs 来触发执行
      auto now = std::chrono::steady_clock::now();
      std::vector<Config::PeriodicTaskInfo*> dueTasks;

      {
        std::lock_guard<std::mutex> lock(cfg->periodicTasksMutex);
        for (auto &entry : cfg->periodicTasks) {
          if (entry.data.isPeriodic && now >= entry.nextRun) {
            dueTasks.push_back(&entry);
          }
        }
      }

      // 优先级从高到低调度
      std::sort(dueTasks.begin(), dueTasks.end(), [](const Config::PeriodicTaskInfo* a, const Config::PeriodicTaskInfo* b) {
        return a->data.priority > b->data.priority;
      });

      for (auto *entry : dueTasks) {
        if (entry->isPinned && entry->pinnedTask) {
          cfg->ts.AddPinnedTask(entry->pinnedTask);
        } else if (!entry->isPinned && entry->taskSet) {
          cfg->ts.AddTaskSetToPipe(entry->taskSet);
        }

        // 基于上次预定时间连续计算，避免调度延迟越积越大
        auto next = entry->nextRun + std::chrono::milliseconds(entry->data.intervalMs);
        while (next <= now) {
          next += std::chrono::milliseconds(entry->data.intervalMs);
        }
        entry->nextRun = next;
      }

      // 计算下一次唤醒时间
      std::chrono::steady_clock::time_point nextWake = std::chrono::steady_clock::time_point::max();
      {
        std::lock_guard<std::mutex> lock(cfg->periodicTasksMutex);
        for (auto &entry : cfg->periodicTasks) {
          if (entry.data.isPeriodic) {
            nextWake = std::min(nextWake, entry.nextRun);
          }
        }
      }

      auto now2 = std::chrono::steady_clock::now();
      if (nextWake != std::chrono::steady_clock::time_point::max()) {
        auto sleepDuration = std::chrono::duration_cast<std::chrono::milliseconds>(nextWake - now2);
        if (sleepDuration < std::chrono::milliseconds(1)) {
          sleepDuration = std::chrono::milliseconds(1);
        }
        std::this_thread::sleep_for(sleepDuration);
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    } else {
      // 其他 IO 线程只执行已刷入的 pinned 任务，避免重复调度
      cfg->ts.RunPinnedTasks();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  std::cout << "[IEC:IO" << threadNum << "] 已退出\n";
}

// -----------------------------------------------------------------------------

IecTask::IecTask(const Config::TaskData &data) : m_data(data) {
  m_SetSize = 1;
  m_data.state = Config::TaskState::PENDING;
}

void IecTask::ExecuteRange(enki::TaskSetPartition, uint32_t threadNum) {
  auto *cfg = Config::Instance();
  const bool isIECThread = threadNum >= cfg->ts.GetNumTaskThreads() -
                                            (uint32_t)Config::IOThreadId::NUM;

  if (isIECThread) {
    std::cout << "[错误] 计算任务在IEC线程 " << threadNum << " 执行\n";
    return;
  }
  m_state.store(Config::TaskState::RUNNING, std::memory_order_relaxed);

  try {
    m_state.store(Config::TaskState::COMPLETED, std::memory_order_relaxed);
    std::cout << "[IEC] 线程" << threadNum << " | 轮次" << m_data.runId
              << " | 任务" << m_data.taskLabel << " | 状态: COMPLETED\n";
  } catch (...) {
    m_state.store(Config::TaskState::FAILED, std::memory_order_relaxed);
    cfg->ioTasksFailed.fetch_add(1, std::memory_order_relaxed);
    std::cout << "[IEC] 线程" << threadNum << " | 轮次" << m_data.runId
              << " | 任务" << m_data.taskLabel << " | 状态: FAILED\n";
  }
}

// -----------------------------------------------------------------------------

FileIOConsumerTask::FileIOConsumerTask(const Config::TaskData &data)
    : m_data(data) {
  m_data.state = Config::TaskState::PENDING;
}

void FileIOConsumerTask::ExecuteRange(enki::TaskSetPartition range,
                                      uint32_t threadNum) {
  auto *cfg = Config::Instance();

  cfg->ioTasksPending.fetch_sub(1, std::memory_order_relaxed);
  m_state.store(Config::TaskState::RUNNING, std::memory_order_relaxed);
  cfg->ioTasksRunning.fetch_add(1, std::memory_order_relaxed);

  try {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    m_state.store(Config::TaskState::COMPLETED, std::memory_order_relaxed);
    cfg->ioTasksCompleted.fetch_add(1, std::memory_order_relaxed);
    std::cout << "[文件IO] 线程" << threadNum << " | 轮次" << m_data.runId
              << " | 任务" << m_data.taskLabel << " | 状态: COMPLETED\n";
  } catch (...) {
    m_state.store(Config::TaskState::FAILED, std::memory_order_relaxed);
    cfg->ioTasksFailed.fetch_add(1, std::memory_order_relaxed);
    std::cout << "[文件IO] 线程" << threadNum << " | 轮次" << m_data.runId
              << " | 任务" << m_data.taskId << " | 状态: FAILED\n";
  }

  cfg->ioTasksRunning.fetch_sub(1, std::memory_order_relaxed);
}

// -----------------------------------------------------------------------------

NetworkIOConsumerTask::NetworkIOConsumerTask(const Config::TaskData &data)
    : m_data(data) {
  m_data.state = Config::TaskState::PENDING;
}

void NetworkIOConsumerTask::ExecuteRange(enki::TaskSetPartition range,
                                         uint32_t threadNum) {
  auto *cfg = Config::Instance();
  cfg->ioTasksPending.fetch_sub(1, std::memory_order_relaxed);
  m_state.store(Config::TaskState::RUNNING, std::memory_order_relaxed);
  cfg->ioTasksRunning.fetch_add(1, std::memory_order_relaxed);

  try {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    m_state.store(Config::TaskState::COMPLETED, std::memory_order_relaxed);
    cfg->ioTasksCompleted.fetch_add(1, std::memory_order_relaxed);
    std::cout << "[网络IO] 线程" << threadNum << " | 轮次" << m_data.runId
              << " | 任务" << m_data.taskLabel << " | 状态: COMPLETED\n";
  } catch (...) {
    m_state.store(Config::TaskState::FAILED, std::memory_order_relaxed);
    cfg->ioTasksFailed.fetch_add(1, std::memory_order_relaxed);
    std::cout << "[网络IO] 线程" << threadNum << " | 轮次" << m_data.runId
              << " | 任务" << m_data.taskId << " | 状态: FAILED\n";
  }

  cfg->ioTasksRunning.fetch_sub(1, std::memory_order_relaxed);
}
