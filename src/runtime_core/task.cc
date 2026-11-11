#include "runtime_core/tasks.h"
#include "runtime_plugin/plugin_manager.h"
#include <glog/logging.h>
#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

namespace {

struct TaskBufferView {
  const void *input_data = nullptr;
  uint32_t input_size = 0;
  uint64_t input_sequence = 0;
  void *output_data = nullptr;
  uint32_t output_size = 0;
};

TaskBufferView AcquireTaskBuffers(runtime_core::TaskBuffers *buffers) {
  TaskBufferView view;
  if (buffers != nullptr && buffers->valid()) {
    view.input_data =
        buffers->AcquireInput(&view.input_size, &view.input_sequence);
    view.output_data = buffers->AcquireOutput(&view.output_size);
  }
  return view;
}

uint32_t TaskKind(Config::TaskType type) {
  switch (type) {
  case Config::TaskType::IECTASK:
    return RT_TASK_KIND_IEC;
  case Config::TaskType::FILE_IO:
    return RT_TASK_KIND_IO;
  case Config::TaskType::NETWORK_IO:
    return RT_TASK_KIND_NETWORK;
  default:
    return 0;
  }
}

int RunTaskCallback(const Config::TaskData &data,
                    RuntimeTaskCallback callback,
                    runtime_core::TaskBuffers *buffers,
                    uint32_t event_id,
                    uint64_t sequence) {
  if (callback == nullptr) {
    return 1;
  }
  TaskBufferView view;
  if (event_id == RT_HOOK_COMM_CYCLE) {
    view = AcquireTaskBuffers(buffers);
  }
  RuntimeTaskContext context = {
      1,
      sizeof(RuntimeTaskContext),
      event_id,
      TaskKind(data.type),
      sequence,
      data.taskLabel.c_str(),
      data.taskConfigJson.c_str(),
      view.input_data,
      view.input_size,
      view.input_sequence,
      view.output_data,
      view.output_size,
  };
  const int result = callback(&context);
  if (event_id == RT_HOOK_COMM_CYCLE &&
      result == 0 && view.output_data != nullptr && buffers != nullptr) {
    buffers->PublishOutput();
  }
  return result;
}

void NotifyTaskEvent(const Config::TaskData &data,
                     RuntimeTaskCallback callback,
                     uint32_t event_id) {
  RunTaskCallback(data, callback, nullptr, event_id, 0);
}

} // namespace

// -----------------------------------------------------------------------------
void IECLoopTask::Execute() {
  auto *cfg = Config::Instance();
  LOG(INFO) << "[IEC:IO" << threadNum << "] 已启动";

  while (!cfg->ts.GetIsShutdownRequested() && !cfg->stopFlag) {
    bool isScheduler = (threadNum == cfg->ioThreadStartIndex);

    if (isScheduler) {
      // 周期性任务调度：根据 TaskData.intervalMs 来触发执行
      auto now = std::chrono::steady_clock::now();
      std::vector<Config::PeriodicTaskInfo*> dueTasks;
      {
        std::lock_guard<std::mutex> lock(cfg->periodicTasksMutex);
        dueTasks.reserve(cfg->periodicTasks.size());
        for (auto &entry : cfg->periodicTasks) {
          if (entry.data.isPeriodic && now >= entry.nextRun) {
            dueTasks.push_back(&entry);
          }
        }

        // 优先级从高到低调度。持锁提交任务，防止生产线程扩容任务表后
        // dueTasks 中的指针失效。
        std::sort(dueTasks.begin(), dueTasks.end(),
                  [](const Config::PeriodicTaskInfo *a,
                     const Config::PeriodicTaskInfo *b) {
                    return a->data.priority > b->data.priority;
                  });

        for (auto *entry : dueTasks) {
          if (entry->isPinned && entry->pinnedTask) {
            cfg->ts.AddPinnedTask(entry->pinnedTask);
          } else if (!entry->isPinned && entry->taskSet) {
            cfg->ts.AddTaskSetToPipe(entry->taskSet);
          }

          // 基于上次预定时间连续计算，避免调度延迟越积越大
          auto next =
              entry->nextRun +
              std::chrono::milliseconds(entry->data.intervalMs);
          while (next <= now) {
            next += std::chrono::milliseconds(entry->data.intervalMs);
          }
          entry->nextRun = next;
        }
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

  LOG(INFO) << "[IEC:IO" << threadNum << "] 已退出";
}

// -----------------------------------------------------------------------------

IecTask::IecTask(const Config::TaskData &data)
    : m_data(data),
      m_callback(TaskCallbackRegistry::Instance().Resolve(
          data.type, data.executorName)),
      m_buffers(data.taskLabel, data.inputBytes, data.outputBytes) {
  m_SetSize = 1;
  m_data.state = Config::TaskState::PENDING;
  if (!m_buffers.valid()) {
    LOG(ERROR) << "IEC task shared buffers unavailable: " << data.taskLabel;
  } else {
    LOG(INFO) << "IEC task shared buffers: input=" << m_buffers.input_name()
              << " output=" << m_buffers.output_name();
  }
  NotifyTaskEvent(m_data, m_callback, RT_HOOK_INIT_TASKS);
}

IecTask::~IecTask() {
  NotifyTaskEvent(m_data, m_callback, RT_HOOK_EXIT_COMM);
  NotifyTaskEvent(m_data, m_callback, RT_HOOK_EXIT_TASKS);
}

void IecTask::ExecuteRange(enki::TaskSetPartition, uint32_t threadNum) {
  auto *cfg = Config::Instance();
  const bool isIECThread = threadNum >= cfg->ts.GetNumTaskThreads() -
                                            (uint32_t)Config::IOThreadId::NUM;

  if (isIECThread) {
    LOG(ERROR) << "计算任务在 IEC 线程 " << threadNum << " 执行";
    return;
  }
  m_state.store(Config::TaskState::RUNNING, std::memory_order_relaxed);
  if (!m_started.exchange(true, std::memory_order_relaxed)) {
    NotifyTaskEvent(m_data, m_callback, RT_HOOK_INIT_COMM);
  }

  try {
    const uint64_t sequence =
        m_executionCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (RunTaskCallback(m_data,
                        m_callback,
                        &m_buffers,
                        RT_HOOK_COMM_CYCLE,
                        sequence) == 0) {
      m_state.store(Config::TaskState::COMPLETED,
                    std::memory_order_relaxed);
      LOG(INFO) << "[IEC] 线程" << threadNum << " | 轮次" << m_data.runId
                << " | 任务" << m_data.taskLabel
                << " | 回调: " << m_data.executorName
                << " | 状态: COMPLETED";
    } else {
      m_state.store(Config::TaskState::FAILED, std::memory_order_relaxed);
      cfg->ioTasksFailed.fetch_add(1, std::memory_order_relaxed);
      LOG(ERROR) << "[IEC] 线程" << threadNum << " | 轮次" << m_data.runId
                 << " | 任务" << m_data.taskLabel
                 << " | 回调: " << m_data.executorName
                 << " | 状态: FAILED";
    }
  } catch (...) {
    m_state.store(Config::TaskState::FAILED, std::memory_order_relaxed);
    cfg->ioTasksFailed.fetch_add(1, std::memory_order_relaxed);
    LOG(ERROR) << "[IEC] 线程" << threadNum << " | 轮次" << m_data.runId
               << " | 任务" << m_data.taskLabel << " | 状态: FAILED";
  }
}

// -----------------------------------------------------------------------------

FileIOConsumerTask::FileIOConsumerTask(const Config::TaskData &data)
    : m_data(data),
      m_callback(TaskCallbackRegistry::Instance().Resolve(
          data.type, data.executorName)),
      m_buffers(data.taskLabel, data.inputBytes, data.outputBytes) {
  m_data.state = Config::TaskState::PENDING;
  if (!m_buffers.valid()) {
    LOG(ERROR) << "IO task shared buffers unavailable: " << data.taskLabel;
  } else {
    LOG(INFO) << "IO task shared buffers: input=" << m_buffers.input_name()
              << " output=" << m_buffers.output_name();
  }
  NotifyTaskEvent(m_data, m_callback, RT_HOOK_INIT_TASKS);
}

FileIOConsumerTask::~FileIOConsumerTask() {
  NotifyTaskEvent(m_data, m_callback, RT_HOOK_EXIT_COMM);
  NotifyTaskEvent(m_data, m_callback, RT_HOOK_EXIT_TASKS);
}

void FileIOConsumerTask::ExecuteRange(enki::TaskSetPartition range,
                                      uint32_t threadNum) {
  auto *cfg = Config::Instance();

  if (!m_pendingConsumed.exchange(true, std::memory_order_relaxed)) {
    cfg->ioTasksPending.fetch_sub(1, std::memory_order_relaxed);
  }
  m_state.store(Config::TaskState::RUNNING, std::memory_order_relaxed);
  cfg->ioTasksRunning.fetch_add(1, std::memory_order_relaxed);
  if (!m_started.exchange(true, std::memory_order_relaxed)) {
    NotifyTaskEvent(m_data, m_callback, RT_HOOK_INIT_COMM);
  }

  try {
    const uint64_t sequence =
        m_executionCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (RunTaskCallback(m_data,
                        m_callback,
                        &m_buffers,
                        RT_HOOK_COMM_CYCLE,
                        sequence) == 0) {
      m_state.store(Config::TaskState::COMPLETED,
                    std::memory_order_relaxed);
      cfg->ioTasksCompleted.fetch_add(1, std::memory_order_relaxed);
      LOG(INFO) << "[文件IO] 线程" << threadNum << " | 轮次"
                << m_data.runId << " | 任务" << m_data.taskLabel
                << " | 回调: " << m_data.executorName
                << " | 状态: COMPLETED";
    } else {
      m_state.store(Config::TaskState::FAILED, std::memory_order_relaxed);
      cfg->ioTasksFailed.fetch_add(1, std::memory_order_relaxed);
      LOG(ERROR) << "[文件IO] 线程" << threadNum << " | 轮次"
                 << m_data.runId << " | 任务" << m_data.taskLabel
                 << " | 回调: " << m_data.executorName
                 << " | 状态: FAILED";
    }
  } catch (...) {
    m_state.store(Config::TaskState::FAILED, std::memory_order_relaxed);
    cfg->ioTasksFailed.fetch_add(1, std::memory_order_relaxed);
    LOG(ERROR) << "[文件IO] 线程" << threadNum << " | 轮次" << m_data.runId
               << " | 任务" << m_data.taskId << " | 状态: FAILED";
  }

  cfg->ioTasksRunning.fetch_sub(1, std::memory_order_relaxed);
}
// -----------------------------------------------------------------------------

NetworkIOConsumerTask::NetworkIOConsumerTask(const Config::TaskData &data)
    : m_data(data),
      m_callback(TaskCallbackRegistry::Instance().Resolve(
          data.type, data.executorName)),
      m_buffers(data.taskLabel, data.inputBytes, data.outputBytes) {
  m_data.state = Config::TaskState::PENDING;
  if (!m_buffers.valid()) {
    LOG(ERROR) << "Network task shared buffers unavailable: "
               << data.taskLabel;
  } else {
    LOG(INFO) << "Network task shared buffers: input="
              << m_buffers.input_name()
              << " output=" << m_buffers.output_name();
  }
  NotifyTaskEvent(m_data, m_callback, RT_HOOK_INIT_TASKS);
}

NetworkIOConsumerTask::~NetworkIOConsumerTask() {
  NotifyTaskEvent(m_data, m_callback, RT_HOOK_EXIT_COMM);
  NotifyTaskEvent(m_data, m_callback, RT_HOOK_EXIT_TASKS);
}

void NetworkIOConsumerTask::ExecuteRange(enki::TaskSetPartition range,
                                         uint32_t threadNum) {
  auto *cfg = Config::Instance();
  if (!m_pendingConsumed.exchange(true, std::memory_order_relaxed)) {
    cfg->ioTasksPending.fetch_sub(1, std::memory_order_relaxed);
  }
  m_state.store(Config::TaskState::RUNNING, std::memory_order_relaxed);
  cfg->ioTasksRunning.fetch_add(1, std::memory_order_relaxed);
  if (!m_started.exchange(true, std::memory_order_relaxed)) {
    NotifyTaskEvent(m_data, m_callback, RT_HOOK_INIT_COMM);
  }

  try {
    const uint64_t sequence =
        m_executionCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (RunTaskCallback(m_data,
                        m_callback,
                        &m_buffers,
                        RT_HOOK_COMM_CYCLE,
                        sequence) == 0) {
      m_state.store(Config::TaskState::COMPLETED,
                    std::memory_order_relaxed);
      cfg->ioTasksCompleted.fetch_add(1, std::memory_order_relaxed);
      LOG(INFO) << "[网络IO] 线程" << threadNum << " | 轮次"
                << m_data.runId << " | 任务" << m_data.taskLabel
                << " | 回调: " << m_data.executorName
                << " | 状态: COMPLETED";
    } else {
      m_state.store(Config::TaskState::FAILED, std::memory_order_relaxed);
      cfg->ioTasksFailed.fetch_add(1, std::memory_order_relaxed);
      LOG(ERROR) << "[网络IO] 线程" << threadNum << " | 轮次"
                 << m_data.runId << " | 任务" << m_data.taskLabel
                 << " | 回调: " << m_data.executorName
                 << " | 状态: FAILED";
    }
  } catch (...) {
    m_state.store(Config::TaskState::FAILED, std::memory_order_relaxed);
    cfg->ioTasksFailed.fetch_add(1, std::memory_order_relaxed);
    LOG(ERROR) << "[网络IO] 线程" << threadNum << " | 轮次" << m_data.runId
               << " | 任务" << m_data.taskId << " | 状态: FAILED";
  }

  cfg->ioTasksRunning.fetch_sub(1, std::memory_order_relaxed);
}

// -----------------------------------------------------------------------------

PluginHookCycleTask::PluginHookCycleTask(const Config::TaskData &data)
    : m_data(data) {
  m_SetSize = 1;
  m_data.state = Config::TaskState::PENDING;
}

void PluginHookCycleTask::ExecuteRange(enki::TaskSetPartition,
                                       uint32_t threadNum) {
  auto *cfg = Config::Instance();
  auto *manager = cfg->plugin_manager;

  m_state.store(Config::TaskState::RUNNING, std::memory_order_relaxed);
  cfg->ioTasksRunning.fetch_add(1, std::memory_order_relaxed);

  std::string error;
  RtResult result =
      manager == nullptr
          ? RT_ERR_PARAMETER
          : manager->CallAllHooks(RT_HOOK_COMM_CYCLE,
                                  0,
                                  0,
                                  &error);

  const uint64_t completed =
      m_executionCount.fetch_add(1, std::memory_order_relaxed) + 1;
  if (result == RT_OK) {
    m_state.store(Config::TaskState::COMPLETED, std::memory_order_relaxed);
    cfg->ioTasksCompleted.fetch_add(1, std::memory_order_relaxed);
  } else {
    m_state.store(Config::TaskState::FAILED, std::memory_order_relaxed);
    cfg->ioTasksFailed.fetch_add(1, std::memory_order_relaxed);
  }

  if (completed == 1 || completed % 1000 == 0 || result != RT_OK) {
    if (result == RT_OK) {
      LOG(INFO) << "[插件HookAll] 线程" << threadNum << " | 任务"
                << m_data.taskLabel
                << " | 状态: COMPLETED"
                << " | 执行次数: " << completed;
    } else {
      LOG(ERROR) << "[插件HookAll] 线程" << threadNum << " | 任务"
                 << m_data.taskLabel
                 << " | 状态: FAILED"
                 << " | 执行次数: " << completed
                 << " | 错误: " << error;
    }
  }

  cfg->ioTasksRunning.fetch_sub(1, std::memory_order_relaxed);
}
