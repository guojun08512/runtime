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
  if (type == "iectask" || type == "compute")
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
  while (!cfg->stopFlag) {
    if (receiver.wait_for_recv(cfg->ipcConfig.receiverCount,
                               cfg->ipcConfig.waitTimeoutUs)) {
      auto buff = receiver.recv();
      std::string cmd((const char *)buff.data(), buff.size());

      Config::TaskData ipcTask;
      bool parsed = false;
      parsed = ParseTaskCommandJson(cmd, ipcTask);

      if (parsed) {
        ipcTask.runId = runId;
        ipcTask.taskId = 0;

        // 注册任务
        if (ipcTask.type == Config::TaskType::FILE_IO) {
          auto *task = new FileIOConsumerTask(ipcTask);
          cfg->ioTasksPending.fetch_add(1, std::memory_order_relaxed);
          task->threadNum =
              cfg->ioThreadStartIndex + (uint32_t)Config::IOThreadId::FILE_IO;
          {
            std::lock_guard<std::mutex> lock(cfg->periodicTasksMutex);
            cfg->periodicTasks.push_back({true, nullptr, task, ipcTask,
                                          std::chrono::steady_clock::now()});
          }
        } else if (ipcTask.type == Config::TaskType::IECTASK) {
          auto *task = new IecTask(ipcTask);
          {
            std::lock_guard<std::mutex> lock(cfg->periodicTasksMutex);
            cfg->periodicTasks.push_back({false, task, nullptr, ipcTask,
                                          std::chrono::steady_clock::now()});
          }
        } else if (ipcTask.type == Config::TaskType::NETWORK_IO) {
          auto *task = new NetworkIOConsumerTask(ipcTask);
          cfg->ioTasksPending.fetch_add(1, std::memory_order_relaxed);
          task->threadNum = cfg->ioThreadStartIndex +
                            (uint32_t)Config::IOThreadId::NETWORK_IO_0;
          {
            std::lock_guard<std::mutex> lock(cfg->periodicTasksMutex);
            cfg->periodicTasks.push_back({true, nullptr, task, ipcTask,
                                          std::chrono::steady_clock::now()});
          }
        }
      } else if (!cmd.empty()) {
        std::cerr << "[TaskProducer] 指令解析失败: " << cmd << "\n";
      }
    }
  }

  std::cout << "[TaskProducer] 轮次 " << runId << " 停止监听\n";
}
