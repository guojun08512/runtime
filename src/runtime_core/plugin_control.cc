#include "runtime_core/plugin_control.h"

#include "cjson/cJSON.h"
#include <glog/logging.h>
#include "runtime_plugin/plugin_manager.h"

namespace {

std::string GetString(cJSON *object, const char *key) {
    cJSON *node = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsString(node) && node->valuestring != nullptr) {
        return node->valuestring;
    }
    return {};
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
        *error = "缺少 path/pluginPath";
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

} // namespace

bool HandlePluginControlCommand(const std::string &json,
                                Config *cfg,
                                int32_t runId) {
    (void)runId;
    cJSON *root = cJSON_Parse(json.c_str());
    if (root == nullptr) {
        return false;
    }

    const std::string command = GetString(root, "command");
    if (command.rfind("plugin.", 0) != 0) {
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
    } else {
        LOG(ERROR) << "[TaskProducer] 未知插件命令: " << command;
    }

    cJSON_Delete(root);
    return true;
}
