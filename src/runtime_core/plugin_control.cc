#include "runtime_core/plugin_control.h"

#include "cjson/cJSON.h"
#include <glog/logging.h>
#include "runtime_core/tasks.h"
#include "runtime_plugin/plugin_manager.h"

#include <new>

namespace {

std::string GetString(cJSON *object, const char *key) {
    cJSON *node = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsString(node) && node->valuestring != nullptr) {
        return node->valuestring;
    }
    return {};
}

int GetInt(cJSON *object, const char *key, int default_value) {
    cJSON *node = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(node) ? node->valueint : default_value;
}

bool GetBool(cJSON *object, const char *key, bool default_value) {
    cJSON *node = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsBool(node)
               ? cJSON_IsTrue(node) != 0
               : default_value;
}

bool GetConfigJson(cJSON *object, std::string *config_json) {
    cJSON *config = cJSON_GetObjectItemCaseSensitive(object, "config");
    if (config == nullptr) {
        *config_json = "{}";
        return true;
    }
    if (!cJSON_IsObject(config)) {
        return false;
    }

    char *printed = cJSON_PrintUnformatted(config);
    if (printed == nullptr) {
        return false;
    }
    *config_json = printed;
    cJSON_free(printed);
    return true;
}

void PrintPluginFailure(const char *command, const std::string &error) {
    LOG(ERROR) << "[TaskProducer] " << command << " 失败: " << error;
}

bool LoadPluginFromCommand(cJSON *root,
                           Config *cfg,
                           std::string *plugin_id,
                           std::string *error) {
    const std::string requested_id = GetString(root, "pluginId");
    if (!requested_id.empty() && cfg->plugin_manager->HasPlugin(requested_id)) {
        *plugin_id = requested_id;
        return true;
    }

    std::string path = GetString(root, "path");
    if (path.empty()) {
        path = GetString(root, "pluginPath");
    }
    if (path.empty()) {
        *error = "缺少 path";
        return false;
    }

    std::string config_json;
    if (!GetConfigJson(root, &config_json)) {
        *error = "config 必须是 JSON 对象";
        return false;
    }

    runtime_plugin::LoadOptions options;
    options.config_json = std::move(config_json);
    options.auto_start_default_driver =
        GetBool(root, "autoStart", false);

    std::string loaded_id;
    RtResult result =
        cfg->plugin_manager->LoadPlugin(path, options, &loaded_id, error);
    if (result != RT_OK) {
        return false;
    }
    if (!requested_id.empty() && requested_id != loaded_id) {
        *error = "插件 ID 不匹配: 请求 " + requested_id +
                 "，实际 " + loaded_id;
        if (!options.auto_start_default_driver) {
            cfg->plugin_manager->UnloadPlugin(loaded_id, nullptr);
        }
        return false;
    }
    *plugin_id = loaded_id;
    return true;
}

bool EnsurePluginLoaded(cJSON *root,
                        Config *cfg,
                        const std::string &config_json,
                        std::string *plugin_id,
                        bool *loaded_here,
                        std::string *error) {
    *loaded_here = false;
    *plugin_id = GetString(root, "pluginId");
    std::string path = GetString(root, "path");
    if (path.empty()) {
        path = GetString(root, "pluginPath");
    }

    if (!plugin_id->empty() && cfg->plugin_manager->HasPlugin(*plugin_id)) {
        return true;
    }
    if (path.empty()) {
        *error = "插件未加载且未提供 path/pluginPath";
        return false;
    }

    runtime_plugin::LoadOptions options;
    options.config_json = config_json;
    std::string loaded_id;
    RtResult result = cfg->plugin_manager->LoadPlugin(
        path, options, &loaded_id, error);
    if (result != RT_OK) {
        return false;
    }
    *loaded_here = true;

    if (!plugin_id->empty() && *plugin_id != loaded_id) {
        *error = "插件 ID 不匹配: 请求 " + *plugin_id +
                 "，实际 " + loaded_id;
        cfg->plugin_manager->UnloadPlugin(loaded_id, nullptr);
        return false;
    }
    *plugin_id = loaded_id;
    return true;
}

bool CreatePluginIoTask(cJSON *root, Config *cfg, int32_t runId) {
    const std::string label = GetString(root, "label");
    const int interval_ms = GetInt(root, "intervalMs", -1);
    const int priority = GetInt(root, "priority", 0);
    const std::string driver_type = GetString(root, "driverType");
    const int read_bytes = GetInt(root, "readBytes", 4);
    const int write_bytes = GetInt(root, "writeBytes", 4);
    const int parameter_id = GetInt(root, "parameterId", -1);
    const int parameter_bytes = GetInt(root, "parameterBytes", 0);

    if (label.empty() || interval_ms <= 0 || priority < 0 ||
        driver_type.empty() || read_bytes < 0 || write_bytes < 0 ||
        (read_bytes == 0 && write_bytes == 0)) {
        LOG(ERROR) << "[TaskProducer] plugin.io_task 参数无效";
        return false;
    }
    if (parameter_bytes < 0 ||
        (parameter_bytes > 0 && parameter_id < 0) ||
        (parameter_bytes == 0 && parameter_id >= 0)) {
        LOG(ERROR) << "[TaskProducer] plugin.io_task 参数访问配置无效";
        return false;
    }

    std::string config_json;
    if (!GetConfigJson(root, &config_json)) {
        LOG(ERROR) << "[TaskProducer] plugin.io_task config 必须是 JSON 对象";
        return false;
    }

    std::string plugin_id;
    bool loaded_here = false;
    std::string load_error;
    if (!EnsurePluginLoaded(root,
                            cfg,
                            config_json,
                            &plugin_id,
                            &loaded_here,
                            &load_error)) {
        PrintPluginFailure("plugin.io_task load", load_error);
        return false;
    }

    runtime_plugin::DriverHandle *driver = nullptr;
    std::string error;
    RtResult result = cfg->plugin_manager->CreateDriver(
        plugin_id, driver_type, config_json, &driver, &error);
    if (result != RT_OK) {
        PrintPluginFailure("CreateDriver", error);
        if (loaded_here) {
            cfg->plugin_manager->UnloadPlugin(plugin_id, nullptr);
        }
        return false;
    }

    RtIoDriverInfoV1 driver_info{};
    uint32_t diagnosis = 0;
    result = cfg->plugin_manager->GetDriverInfo(driver, &driver_info);
    if (result == RT_OK) {
        result = cfg->plugin_manager->UpdateConfiguration(driver, config_json);
    }
    if (result == RT_OK) {
        result = cfg->plugin_manager->UpdateMapping(driver, config_json);
    }
    if (result == RT_OK) {
        result = cfg->plugin_manager->IdentifyDriver(driver);
    }
    if (result == RT_OK) {
        result = cfg->plugin_manager->ScanModules(driver);
    }
    if (result == RT_OK) {
        result = cfg->plugin_manager->GetDiagnosis(driver, &diagnosis);
    }
    if (result != RT_OK) {
        LOG(ERROR) << "[TaskProducer] 初始化驱动完整接口失败: " << result;
        cfg->plugin_manager->DestroyDriver(driver, nullptr);
        if (loaded_here) {
            cfg->plugin_manager->UnloadPlugin(plugin_id, nullptr);
        }
        return false;
    }

    result = cfg->plugin_manager->StartDriver(driver, &error);
    if (result != RT_OK) {
        PrintPluginFailure("StartDriver", error);
        cfg->plugin_manager->DestroyDriver(driver, nullptr);
        if (loaded_here) {
            cfg->plugin_manager->UnloadPlugin(plugin_id, nullptr);
        }
        return false;
    }

    Config::TaskData task_data;
    task_data.type = Config::TaskType::PLUGIN_IO;
    task_data.runId = runId;
    task_data.taskId = 0;
    task_data.taskLabel = label;
    task_data.isPeriodic = true;
    task_data.intervalMs = static_cast<uint32_t>(interval_ms);
    task_data.priority = priority;
    task_data.pluginId = plugin_id;
    task_data.driverTypeId = driver_type;
    task_data.readBytes = static_cast<uint32_t>(read_bytes);
    task_data.writeBytes = static_cast<uint32_t>(write_bytes);
    task_data.parameterId =
        parameter_id < 0 ? UINT32_MAX : static_cast<uint32_t>(parameter_id);
    task_data.parameterBytes = static_cast<uint32_t>(parameter_bytes);

    PluginIOTask *task = new (std::nothrow) PluginIOTask(task_data, driver);
    if (task == nullptr) {
        cfg->plugin_manager->DestroyDriver(driver, nullptr);
        if (loaded_here) {
            cfg->plugin_manager->UnloadPlugin(plugin_id, nullptr);
        }
        LOG(ERROR) << "[TaskProducer] PluginIOTask 分配失败";
        return false;
    }

    try {
        std::lock_guard<std::mutex> lock(cfg->periodicTasksMutex);
        cfg->periodicTasks.push_back(
            {false, task, nullptr, task_data,
             std::chrono::steady_clock::now()});
    } catch (...) {
        cfg->plugin_manager->DestroyDriver(driver, nullptr);
        if (loaded_here) {
            cfg->plugin_manager->UnloadPlugin(plugin_id, nullptr);
        }
        delete task;
        LOG(ERROR) << "[TaskProducer] 注册插件任务失败";
        return false;
    }

    LOG(INFO) << "[TaskProducer] 已安装插件任务 " << label
              << " | 插件: " << plugin_id
              << " | 驱动: " << driver_type
              << " | 间隔: " << interval_ms << "ms"
              << " | 优先级: " << priority;
    return true;
}

bool CreatePluginHookTask(cJSON *root, Config *cfg, int32_t runId) {
    const std::string label = GetString(root, "label");
    const int interval_ms = GetInt(root, "intervalMs", -1);
    const int priority = GetInt(root, "priority", 0);
    if (label.empty() || interval_ms <= 0 || priority < 0) {
        LOG(ERROR) << "[TaskProducer] plugin.hook_task 参数无效";
        return false;
    }

    std::string config_json;
    if (!GetConfigJson(root, &config_json)) {
        LOG(ERROR) << "[TaskProducer] plugin.hook_task config 必须是 JSON 对象";
        return false;
    }

    std::string plugin_id;
    bool loaded_here = false;
    std::string error;
    if (!EnsurePluginLoaded(root,
                            cfg,
                            config_json,
                            &plugin_id,
                            &loaded_here,
                            &error)) {
        PrintPluginFailure("plugin.hook_task load", error);
        return false;
    }

    Config::TaskData task_data;
    task_data.type = Config::TaskType::PLUGIN_HOOK;
    task_data.runId = runId;
    task_data.taskId = 0;
    task_data.taskLabel = label;
    task_data.isPeriodic = true;
    task_data.intervalMs = static_cast<uint32_t>(interval_ms);
    task_data.priority = priority;
    task_data.pluginId = plugin_id;

    PluginHookTask *task =
        new (std::nothrow) PluginHookTask(task_data, cfg->plugin_manager);
    if (task == nullptr) {
        if (loaded_here) {
            cfg->plugin_manager->UnloadPlugin(plugin_id, nullptr);
        }
        LOG(ERROR) << "[TaskProducer] PluginHookTask 分配失败";
        return false;
    }

    try {
        std::lock_guard<std::mutex> lock(cfg->periodicTasksMutex);
        cfg->periodicTasks.push_back(
            {false, task, nullptr, task_data,
             std::chrono::steady_clock::now()});
    } catch (...) {
        delete task;
        if (loaded_here) {
            cfg->plugin_manager->UnloadPlugin(plugin_id, nullptr);
        }
        LOG(ERROR) << "[TaskProducer] 注册 Hook 任务失败";
        return false;
    }

    LOG(INFO) << "[TaskProducer] 已安装 Hook 任务 " << label
              << " | 插件: " << plugin_id
              << " | Hook: RT_HOOK_COMM_CYCLE"
              << " | 间隔: " << interval_ms << "ms"
              << " | 优先级: " << priority;
    return true;
}

} // namespace

bool HandlePluginControlCommand(const std::string &json,
                                Config *cfg,
                                int32_t runId) {
    cJSON *root = cJSON_Parse(json.c_str());
    if (root == nullptr) {
        return false;
    }

    const std::string command = GetString(root, "command");
    const std::string type = GetString(root, "type");
    const bool is_plugin_command =
        command.rfind("plugin.", 0) == 0 ||
        type == "plugin_io" || type == "plugin_hook";
    if (!is_plugin_command) {
        cJSON_Delete(root);
        return false;
    }

    if (cfg == nullptr || cfg->plugin_manager == nullptr) {
        LOG(ERROR) << "[TaskProducer] 插件管理器不可用";
        cJSON_Delete(root);
        return true;
    }

    if (command == "plugin.load") {
        std::string plugin_id;
        std::string error;
        if (!LoadPluginFromCommand(root, cfg, &plugin_id, &error)) {
            PrintPluginFailure("plugin.load", error);
        } else {
            LOG(INFO) << "[TaskProducer] 插件已加载: " << plugin_id;
        }
    } else if (command == "plugin.unload") {
        const std::string plugin_id = GetString(root, "pluginId");
        std::string error;
        if (plugin_id.empty()) {
            LOG(ERROR) << "[TaskProducer] plugin.unload 缺少 pluginId";
        } else if (cfg->plugin_manager->UnloadPlugin(plugin_id, &error) !=
                   RT_OK) {
            PrintPluginFailure("plugin.unload", error);
        } else {
            LOG(INFO) << "[TaskProducer] 插件已卸载: " << plugin_id;
        }
    } else if (command == "plugin.io_task" || type == "plugin_io") {
        CreatePluginIoTask(root, cfg, runId);
    } else if (command == "plugin.hook_task" || type == "plugin_hook") {
        CreatePluginHookTask(root, cfg, runId);
    } else {
        LOG(ERROR) << "[TaskProducer] 未知插件命令: " << command;
    }

    cJSON_Delete(root);
    return true;
}
