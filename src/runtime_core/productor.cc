#include "runtime_core/productor.h"
#include "cjson/cJSON.h"
#include "libipc/ipc.h"
#include "runtime_core/global.h"
#include "runtime_core/tasks.h"
#include <iostream>

static bool ParseTaskCommandJson(const std::string &cmd,
                                 Config::TaskData &outData) {
  cJSON *root = cJSON_Parse(cmd.c_str());
  if (!root) {
    return false;
  }

  auto get_string = [&](const char *key) -> std::string {
    cJSON *node = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(node) && (node->valuestring != nullptr)) {
      return std::string(node->valuestring);
    }
    return std::string();
  };

  auto get_int = [&](const char *key, int defaultValue) -> int {
    cJSON *node = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(node)) {
      return node->valueint;
    }
    return defaultValue;
  };

  std::string type = get_string("type");
  std::string label = get_string("label");
  int iv = get_int("intervalMs", -1);
  int pr = get_int("priority", -1);

  cJSON_Delete(root);

  if (type.empty() || label.empty() || iv <= 0 || pr < 0) {
    return false;
  }

  Config::TaskType t = Config::TaskType::FILE_IO;
  if (type == "iectask" || type == "iec")
    t = Config::TaskType::IECTASK;
  else if (type == "net" || type == "network")
    t = Config::TaskType::NETWORK_IO;
  else if (type == "file")
    t = Config::TaskType::FILE_IO;

  outData.type = t;
  outData.intervalMs = static_cast<uint32_t>(iv);
  outData.priority = pr;
  outData.taskLabel = label;
  outData.isPeriodic = true;
  return true;
}

void TaskProducer(int runId) {
  auto *cfg = Config::Instance();

  // 检查 IPC 是否启用
  if (!cfg->ipcConfig.enabled) {
    std::cerr << "[TaskProducer] IPC 已禁用\n";
    return;
  }

  ipc::route receiver(cfg->ipcConfig.routeName.c_str(), ipc::receiver);
  if (!receiver.valid()) {
    std::cerr << "[TaskProducer] libipc receiver 连接失败: "
              << cfg->ipcConfig.routeName << "\n";
    return;
  }

  std::cout << "[TaskProducer] 轮次 " << runId << " 开始监听任务指令...\n";

  // 循环接收任务指令直到 stopFlag 被设置
  // 使用非阻塞方式以便快速响应stopFlag
  while (!cfg->stopFlag) {
    // 非阻塞接收 - 如果没有数据立即返回  
    auto buff = receiver.recv(cfg->ipcConfig.waitTimeoutUs);
    if (buff.empty()) {
      // 没有数据，短暂睡眠然后轮询
      std::this_thread::sleep_for(std::chrono::microseconds(100));
      continue;
    }
    
    std::string cmd((const char *)buff.data(), buff.size());

    Config::TaskData taskData;
    bool parsed = false;
    parsed = ParseTaskCommandJson(cmd, taskData);

    if (parsed) {
      taskData.runId = runId;
      taskData.taskId = 0;

      std::cout << taskData.taskLabel << " | 轮次" << taskData.runId
                << " | 任务类型: " << (taskData.type == Config::TaskType::IECTASK
                                          ? "IECTASK"
                                          : (taskData.type == Config::TaskType::FILE_IO
                                                 ? "FILE_IO"
                                                 : "NETWORK_IO"))
                << " | 间隔: " << taskData.intervalMs << "ms"
                << " | 优先级: " << taskData.priority << "\n";

      // 注册任务
      if (taskData.type == Config::TaskType::FILE_IO) {
        auto *task = new FileIOConsumerTask(taskData);
        cfg->ioTasksPending.fetch_add(1, std::memory_order_relaxed);
        // FILE_IO 作为 TaskSet（非Pinned），可以在任何线程执行，不需要指定threadNum
        {
          std::lock_guard<std::mutex> lock(cfg->periodicTasksMutex);
          cfg->periodicTasks.push_back({false, task, nullptr, taskData,
                                        std::chrono::steady_clock::now()});
        }
      } else if (taskData.type == Config::TaskType::IECTASK) {
        auto *task = new IecTask(taskData);
        {
          std::lock_guard<std::mutex> lock(cfg->periodicTasksMutex);
          cfg->periodicTasks.push_back({false, task, nullptr, taskData,
                                        std::chrono::steady_clock::now()});
        }
      } else if (taskData.type == Config::TaskType::NETWORK_IO) {
        auto *task = new NetworkIOConsumerTask(taskData);
        cfg->ioTasksPending.fetch_add(1, std::memory_order_relaxed);
        {
          std::lock_guard<std::mutex> lock(cfg->periodicTasksMutex);
          cfg->periodicTasks.push_back({false, task, nullptr, taskData,
                                        std::chrono::steady_clock::now()});
        }
      }
    } else if (!cmd.empty()) {
      std::cerr << "[TaskProducer] 指令解析失败: " << cmd << "\n";
    }
  }

  std::cout << "[TaskProducer] 轮次 " << runId << " 停止监听\n";
}
