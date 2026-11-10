#include "runtime_plugin/plugin_manager.h"

#include <cjson/cJSON.h>
#include <glog/logging.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#error "The runtime plugin loader currently supports POSIX dlopen only."
#else
#include <dlfcn.h>
#endif

namespace runtime_plugin {
namespace {

constexpr const char *kPluginEntrySymbol = "runtime_plugin_query_v1";
constexpr uint32_t kMaxDriverCount = 1024;

void SetError(std::string *error, const std::string &message) {
    if (error != nullptr) {
        *error = message;
    }
}

void RT_PLUGIN_CALL HostLog(int level, const char *message) {
    const char *text = message == nullptr ? "" : message;
    if (level >= 2) {
        LOG(ERROR) << "[plugin] " << text;
    } else if (level == 1) {
        LOG(WARNING) << "[plugin] " << text;
    } else {
        LOG(INFO) << "[plugin] " << text;
    }
}

void *RT_PLUGIN_CALL HostAlloc(size_t size) {
    return std::malloc(size);
}

void RT_PLUGIN_CALL HostFree(void *ptr) {
    std::free(ptr);
}

bool IsValidText(const char *text) {
    return text != nullptr && text[0] != '\0';
}

struct PluginRecord {
    void *module = nullptr;
    const RtPluginV1 *descriptor = nullptr;
    std::string canonical_path;
    std::atomic<bool> accepting_calls{true};
    std::atomic<uint32_t> active_calls{0};
    std::atomic<uint32_t> active_instances{0};

    bool Enter() {
        if (!accepting_calls.load(std::memory_order_acquire)) {
            return false;
        }

        active_calls.fetch_add(1, std::memory_order_acq_rel);
        if (!accepting_calls.load(std::memory_order_acquire)) {
            active_calls.fetch_sub(1, std::memory_order_acq_rel);
            return false;
        }
        return true;
    }

    void Leave() {
        active_calls.fetch_sub(1, std::memory_order_acq_rel);
    }
};

const RtDriverDescV1 *FindDriver(const PluginRecord &plugin,
                                 const std::string &driver_type_id) {
    for (uint32_t i = 0; i < plugin.descriptor->driver_count; ++i) {
        const RtDriverDescV1 *driver = plugin.descriptor->drivers[i];
        if (driver != nullptr && driver_type_id == driver->type_id) {
            return driver;
        }
    }
    return nullptr;
}

bool ValidatePluginDescriptor(const RtPluginV1 *plugin,
                              std::string *error) {
    if (plugin == nullptr) {
        SetError(error, "runtime_plugin_query_v1 returned null");
        return false;
    }
    if (plugin->abi_version != RT_PLUGIN_ABI_V1 ||
        plugin->struct_size < sizeof(RtPluginV1)) {
        SetError(error, "incompatible plugin ABI or descriptor size");
        return false;
    }
    if (!IsValidText(plugin->plugin_id) ||
        !IsValidText(plugin->display_name) ||
        plugin->init == nullptr || plugin->shutdown == nullptr ||
        plugin->hook == nullptr) {
        SetError(error, "plugin descriptor is missing required fields");
        return false;
    }
    if (plugin->driver_count > kMaxDriverCount ||
        (plugin->driver_count != 0 && plugin->drivers == nullptr)) {
        SetError(error, "plugin driver table is invalid");
        return false;
    }

    for (uint32_t i = 0; i < plugin->driver_count; ++i) {
        const RtDriverDescV1 *driver = plugin->drivers[i];
        if (driver == nullptr ||
            driver->abi_version != RT_PLUGIN_ABI_V1 ||
            driver->struct_size < sizeof(RtDriverDescV1) ||
            !IsValidText(driver->type_id) ||
            !IsValidText(driver->display_name) ||
            driver->io_ops == nullptr) {
            SetError(error, "invalid driver descriptor in plugin");
            return false;
        }

        const RtIoDriverOpsV1 *ops = driver->io_ops;
        if (ops->abi_version != RT_PLUGIN_ABI_V1 ||
            ops->struct_size < sizeof(RtIoDriverOpsV1) ||
            ops->create == nullptr || ops->destroy == nullptr) {
            SetError(error, "invalid I/O driver operation table");
            return false;
        }

        if (driver->parameter_ops != nullptr) {
            const RtIoParameterOpsV1 *parameter_ops = driver->parameter_ops;
            if (parameter_ops->abi_version != RT_PLUGIN_ABI_V1 ||
                parameter_ops->struct_size <
                    sizeof(RtIoParameterOpsV1)) {
                SetError(error, "invalid I/O parameter operation table");
                return false;
            }
        }
    }
    return true;
}

} // namespace

class DriverHandle {
public:
    DriverHandle(std::shared_ptr<PluginRecord> plugin,
                 const RtDriverDescV1 *descriptor,
                 void *instance)
        : plugin(std::move(plugin)),
          descriptor(descriptor),
          instance(instance) {}

    std::shared_ptr<PluginRecord> plugin;
    const RtDriverDescV1 *descriptor = nullptr;
    void *instance = nullptr;
    std::mutex state_mutex;
    bool started = false;
};

class PluginManager::Impl {
public:
    Impl() {
        host_api.abi_version = RT_PLUGIN_ABI_V1;
        host_api.struct_size = sizeof(RtHostApiV1);
        host_api.log = HostLog;
        host_api.alloc = HostAlloc;
        host_api.free = HostFree;
    }

    ~Impl() {
        UnloadAll();
    }

    RtResult LoadPlugin(const std::string &path,
                        const LoadOptions &options,
                        std::string *plugin_id,
                        std::string *error) {
        std::lock_guard<std::mutex> load_lock(load_mutex);

        std::error_code fs_error;
        std::filesystem::path absolute =
            std::filesystem::absolute(path, fs_error);
        if (fs_error) {
            SetError(error, "cannot resolve plugin path: " + fs_error.message());
            return RT_ERR_PARAMETER;
        }

        std::filesystem::path canonical =
            std::filesystem::canonical(absolute, fs_error);
        if (fs_error || !std::filesystem::is_regular_file(canonical)) {
            SetError(error, "plugin file does not exist: " + absolute.string());
            return RT_ERR_NOT_FOUND;
        }

        const std::string canonical_path = canonical.string();
        {
            std::lock_guard<std::mutex> registry_lock(registry_mutex);
            for (const auto &entry : plugins) {
                if (entry.second->canonical_path == canonical_path) {
                    SetError(error, "plugin file is already loaded: " +
                                        canonical_path);
                    return RT_ERR_BUSY;
                }
            }
        }

        dlerror();
        void *module = dlopen(canonical_path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (module == nullptr) {
            const char *reason = dlerror();
            SetError(error, reason == nullptr ? "dlopen failed" : reason);
            return RT_ERR_FAILED;
        }

        dlerror();
        void *symbol = dlsym(module, kPluginEntrySymbol);
        if (symbol == nullptr) {
            const char *reason = dlerror();
            SetError(error, reason == nullptr ? "plugin entry symbol missing"
                                             : reason);
            dlclose(module);
            return RT_ERR_ABI;
        }

        RtPluginQueryV1 query = nullptr;
        static_assert(sizeof(query) == sizeof(symbol),
                      "function and object pointers must have equal size");
        std::memcpy(&query, &symbol, sizeof(query));

        const RtPluginV1 *descriptor = query(RT_PLUGIN_ABI_V1);
        if (!ValidatePluginDescriptor(descriptor, error)) {
            dlclose(module);
            return RT_ERR_ABI;
        }

        RtResult result = descriptor->init(&host_api);
        if (result != RT_OK) {
            SetError(error, "plugin initialization failed");
            dlclose(module);
            return result;
        }

        auto record = std::make_shared<PluginRecord>();
        record->module = module;
        record->descriptor = descriptor;
        record->canonical_path = canonical_path;

        const std::string id(descriptor->plugin_id);
        {
            std::lock_guard<std::mutex> registry_lock(registry_mutex);
            if (plugins.find(id) != plugins.end()) {
                SetError(error, "duplicate plugin id: " + id);
                descriptor->shutdown();
                dlclose(module);
                return RT_ERR_BUSY;
            }
            plugins.emplace(id, record);
        }

        if (plugin_id != nullptr) {
            *plugin_id = id;
        }

        const uint32_t init_hooks[] = {
            RT_HOOK_INIT_SYSTEM,
            RT_HOOK_INIT,
            RT_HOOK_INIT2,
            RT_HOOK_INIT3,
            RT_HOOK_INIT_TASKS,
            RT_HOOK_INIT_COMM,
        };
        for (uint32_t hook : init_hooks) {
            if (!record->Enter()) {
                SetError(error, "plugin is being unloaded");
                UnloadPlugin(id, nullptr);
                return RT_ERR_BUSY;
            }
            result = descriptor->hook(hook, 0, 0);
            record->Leave();
            if (result != RT_OK) {
                SetError(error, "plugin hook initialization failed: " +
                                    std::to_string(hook));
                UnloadPlugin(id, nullptr);
                return result;
            }
        }

        if (options.auto_start_default_driver) {
            if (descriptor->driver_count == 0) {
                SetError(error, "plugin has no driver to auto-start");
                UnloadPlugin(id, nullptr);
                return RT_ERR_NOT_FOUND;
            }

            DriverHandle *driver = nullptr;
            result = CreateDriver(id,
                                  descriptor->drivers[0]->type_id,
                                  options.config_json,
                                  &driver,
                                  error);
            if (result == RT_OK) {
                result = StartDriver(driver, error);
                if (result != RT_OK) {
                    DestroyDriver(driver, nullptr);
                }
            }
            if (result != RT_OK) {
                UnloadPlugin(id, nullptr);
                return result;
            }
        }

        return RT_OK;
    }

    RtResult LoadManifest(const std::string &path, std::string *error) {
        std::ifstream input(path);
        if (!input) {
            SetError(error, "cannot open plugin manifest: " + path);
            return RT_ERR_NOT_FOUND;
        }

        std::ostringstream content;
        content << input.rdbuf();
        const std::string json = content.str();
        cJSON *root = cJSON_Parse(json.c_str());
        if (root == nullptr) {
            SetError(error, "invalid plugin manifest JSON: " + path);
            return RT_ERR_PARAMETER;
        }

        cJSON *entries = cJSON_GetObjectItemCaseSensitive(root, "plugins");
        if (!cJSON_IsArray(entries)) {
            cJSON_Delete(root);
            SetError(error, "manifest must contain a plugins array");
            return RT_ERR_PARAMETER;
        }

        const std::filesystem::path manifest_dir =
            std::filesystem::absolute(path).parent_path();
        RtResult final_result = RT_OK;

        cJSON *entry = nullptr;
        cJSON_ArrayForEach(entry, entries) {
            cJSON *path_node =
                cJSON_GetObjectItemCaseSensitive(entry, "path");
            if (!cJSON_IsString(path_node) ||
                path_node->valuestring == nullptr) {
                SetError(error, "plugin entry is missing a path");
                final_result = RT_ERR_PARAMETER;
                break;
            }

            cJSON *auto_start_node =
                cJSON_GetObjectItemCaseSensitive(entry, "auto_start");
            if (auto_start_node != nullptr &&
                !cJSON_IsBool(auto_start_node)) {
                SetError(error, "plugin auto_start must be boolean");
                final_result = RT_ERR_PARAMETER;
                break;
            }

            cJSON *config_node =
                cJSON_GetObjectItemCaseSensitive(entry, "config");
            if (config_node != nullptr &&
                !cJSON_IsObject(config_node)) {
                SetError(error, "plugin config must be an object");
                final_result = RT_ERR_PARAMETER;
                break;
            }

            std::filesystem::path plugin_path(path_node->valuestring);
            if (plugin_path.is_relative()) {
                plugin_path = manifest_dir / plugin_path;
            }

            std::string config_text = "{}";
            if (config_node != nullptr) {
                char *printed = cJSON_PrintUnformatted(config_node);
                if (printed == nullptr) {
                    SetError(error, "cannot serialize plugin config");
                    final_result = RT_ERR_NOMEMORY;
                    break;
                }
                config_text = printed;
                cJSON_free(printed);
            }

            LoadOptions options;
            options.auto_start_default_driver =
                cJSON_IsTrue(auto_start_node);
            options.config_json = std::move(config_text);

            std::string loaded_id;
            std::string load_error;
            final_result = LoadPlugin(plugin_path.string(),
                                      options,
                                      &loaded_id,
                                      &load_error);
            if (final_result != RT_OK) {
                SetError(error, "failed to load " + plugin_path.string() +
                                    ": " + load_error);
                break;
            }
            LOG(INFO) << "[plugin] loaded " << loaded_id << " from "
                      << plugin_path.string();
        }

        cJSON_Delete(root);
        return final_result;
    }

    RtResult UnloadPlugin(const std::string &plugin_id,
                          std::string *error) {
        std::shared_ptr<PluginRecord> record;
        {
            std::lock_guard<std::mutex> registry_lock(registry_mutex);
            auto found = plugins.find(plugin_id);
            if (found == plugins.end()) {
                SetError(error, "plugin is not loaded: " + plugin_id);
                return RT_ERR_NOT_FOUND;
            }

            record = found->second;
            if (record->active_instances.load(std::memory_order_acquire) != 0) {
                SetError(error, "plugin still has active driver instances: " +
                                    plugin_id);
                return RT_ERR_BUSY;
            }

            record->accepting_calls.store(false, std::memory_order_release);
            plugins.erase(found);
        }

        while (record->active_calls.load(std::memory_order_acquire) != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        const uint32_t exit_hooks[] = {
            RT_HOOK_EXIT_COMM,
            RT_HOOK_EXIT_TASKS,
            RT_HOOK_EXIT3,
            RT_HOOK_EXIT2,
            RT_HOOK_EXIT,
            RT_HOOK_EXIT_SYSTEM,
        };
        for (uint32_t hook : exit_hooks) {
            RtResult hook_result =
                record->descriptor->hook(hook, 0, 0);
            if (hook_result != RT_OK && error != nullptr && error->empty()) {
                SetError(error, "plugin hook shutdown failed: " +
                                    std::to_string(hook));
            }
        }

        record->descriptor->shutdown();
        if (dlclose(record->module) != 0) {
            const char *reason = dlerror();
            SetError(error, reason == nullptr ? "dlclose failed" : reason);
            return RT_ERR_FAILED;
        }

        LOG(INFO) << "[plugin] unloaded " << plugin_id;
        return RT_OK;
    }

    RtResult CallHook(const std::string &plugin_id,
                      uint32_t hook,
                      uintptr_t param1,
                      uintptr_t param2,
                      std::string *error) {
        std::shared_ptr<PluginRecord> record;
        {
            std::lock_guard<std::mutex> registry_lock(registry_mutex);
            auto found = plugins.find(plugin_id);
            if (found == plugins.end()) {
                SetError(error, "plugin is not loaded: " + plugin_id);
                return RT_ERR_NOT_FOUND;
            }
            record = found->second;
        }

        if (!record->Enter()) {
            SetError(error, "plugin is being unloaded: " + plugin_id);
            return RT_ERR_BUSY;
        }
        RtResult result = record->descriptor->hook(hook, param1, param2);
        record->Leave();
        if (result != RT_OK) {
            SetError(error, "plugin hook failed: " + std::to_string(hook));
        }
        return result;
    }

    RtResult CallAllHooks(uint32_t hook,
                          uintptr_t param1,
                          uintptr_t param2,
                          std::string *error) {
        std::vector<std::pair<std::string, std::shared_ptr<PluginRecord>>>
            snapshot;
        {
            std::lock_guard<std::mutex> registry_lock(registry_mutex);
            snapshot.reserve(plugins.size());
            for (const auto &entry : plugins) {
                snapshot.emplace_back(entry.first, entry.second);
            }
        }

        RtResult final_result = RT_OK;
        std::string failures;
        for (const auto &entry : snapshot) {
            if (!entry.second->Enter()) {
                if (!failures.empty()) {
                    failures += ", ";
                }
                failures += entry.first + "(busy)";
                final_result = RT_ERR_BUSY;
                continue;
            }

            RtResult result =
                entry.second->descriptor->hook(hook, param1, param2);
            entry.second->Leave();
            if (result != RT_OK) {
                if (!failures.empty()) {
                    failures += ", ";
                }
                failures += entry.first + "(" + std::to_string(result) + ")";
                final_result = result;
            }
        }

        if (final_result != RT_OK) {
            SetError(error,
                     "plugin hook failures: " +
                         (failures.empty() ? std::string("unknown")
                                           : failures));
        }
        return final_result;
    }

    RtResult CreateDriver(const std::string &plugin_id,
                          const std::string &driver_type_id,
                          const std::string &config_json,
                          DriverHandle **driver,
                          std::string *error) {
        if (driver == nullptr) {
            SetError(error, "driver output pointer is null");
            return RT_ERR_PARAMETER;
        }
        *driver = nullptr;

        std::shared_ptr<PluginRecord> record;
        {
            std::lock_guard<std::mutex> registry_lock(registry_mutex);
            auto found = plugins.find(plugin_id);
            if (found == plugins.end()) {
                SetError(error, "plugin is not loaded: " + plugin_id);
                return RT_ERR_NOT_FOUND;
            }
            record = found->second;
        }

        const RtDriverDescV1 *descriptor =
            FindDriver(*record, driver_type_id);
        if (descriptor == nullptr) {
            SetError(error, "driver type is not provided by plugin: " +
                                driver_type_id);
            return RT_ERR_NOT_FOUND;
        }

        if (!record->Enter()) {
            SetError(error, "plugin is being unloaded: " + plugin_id);
            return RT_ERR_BUSY;
        }

        void *instance = nullptr;
        const RtIoDriverOpsV1 *ops = descriptor->io_ops;
        RtResult result = ops->create(&host_api,
                                      config_json.c_str(),
                                      &instance);
        if (result == RT_OK && instance == nullptr) {
            result = RT_ERR_FAILED;
        }
        if (result == RT_OK && ops->configure != nullptr) {
            result = ops->configure(instance, config_json.c_str());
        }
        if (result != RT_OK) {
            if (instance != nullptr) {
                ops->destroy(instance);
            }
            record->Leave();
            SetError(error, "failed to create driver instance");
            return result;
        }

        DriverHandle *created =
            new (std::nothrow) DriverHandle(record, descriptor, instance);
        if (created == nullptr) {
            ops->destroy(instance);
            record->Leave();
            SetError(error, "cannot allocate driver handle");
            return RT_ERR_NOMEMORY;
        }

        record->active_instances.fetch_add(1, std::memory_order_acq_rel);
        {
            std::lock_guard<std::mutex> instance_lock(instance_mutex);
            instances.emplace(created, std::unique_ptr<DriverHandle>(created));
        }
        record->Leave();
        *driver = created;
        return RT_OK;
    }

    RtResult StartDriver(DriverHandle *driver, std::string *error) {
        if (driver == nullptr) {
            SetError(error, "driver handle is null");
            return RT_ERR_PARAMETER;
        }

        std::lock_guard<std::mutex> state_lock(driver->state_mutex);
        if (driver->started) {
            return RT_OK;
        }

        const RtIoDriverOpsV1 *ops = driver->descriptor->io_ops;
        if (ops->start == nullptr) {
            SetError(error, "driver does not support start");
            return RT_ERR_NOT_SUPPORTED;
        }
        if (!driver->plugin->Enter()) {
            SetError(error, "plugin is being unloaded");
            return RT_ERR_BUSY;
        }

        RtResult result = ops->start(driver->instance);
        driver->plugin->Leave();
        if (result == RT_OK) {
            driver->started = true;
        } else {
            SetError(error, "driver start failed");
        }
        return result;
    }

    RtResult StopDriver(DriverHandle *driver, std::string *error) {
        if (driver == nullptr) {
            SetError(error, "driver handle is null");
            return RT_ERR_PARAMETER;
        }

        std::lock_guard<std::mutex> state_lock(driver->state_mutex);
        if (!driver->started) {
            return RT_OK;
        }

        const RtIoDriverOpsV1 *ops = driver->descriptor->io_ops;
        if (!driver->plugin->Enter()) {
            SetError(error, "plugin is being unloaded");
            return RT_ERR_BUSY;
        }
        if (ops->stop != nullptr) {
            ops->stop(driver->instance);
        }
        driver->plugin->Leave();
        driver->started = false;
        return RT_OK;
    }

    RtResult DestroyDriver(DriverHandle *driver, std::string *error) {
        if (driver == nullptr) {
            SetError(error, "driver handle is null");
            return RT_ERR_PARAMETER;
        }

        RtResult result = StopDriver(driver, error);
        if (result != RT_OK) {
            return result;
        }

        if (!driver->plugin->Enter()) {
            SetError(error, "plugin is being unloaded");
            return RT_ERR_BUSY;
        }
        driver->descriptor->io_ops->destroy(driver->instance);
        driver->instance = nullptr;
        driver->plugin->Leave();
        driver->plugin->active_instances.fetch_sub(
            1, std::memory_order_acq_rel);

        {
            std::lock_guard<std::mutex> instance_lock(instance_mutex);
            instances.erase(driver);
        }
        return RT_OK;
    }

    RtResult ReadInputs(DriverHandle *driver,
                        RtIoChannelV1 *channels,
                        uint32_t count) {
        return InvokeChannels(driver, channels, count, true);
    }

    RtResult WriteOutputs(DriverHandle *driver,
                          RtIoChannelV1 *channels,
                          uint32_t count) {
        return InvokeChannels(driver, channels, count, false);
    }

    RtResult GetDriverInfo(DriverHandle *driver, RtIoDriverInfoV1 *info) {
        if (driver == nullptr || info == nullptr) {
            return RT_ERR_PARAMETER;
        }
        const RtIoDriverOpsV1 *ops = driver->descriptor->io_ops;
        if (ops->get_info == nullptr) {
            return RT_ERR_NOT_SUPPORTED;
        }
        if (!driver->plugin->Enter()) {
            return RT_ERR_BUSY;
        }
        RtResult result = ops->get_info(driver->instance, info);
        driver->plugin->Leave();
        return result;
    }

    RtResult UpdateConfiguration(DriverHandle *driver,
                                 const std::string &config_json) {
        if (driver == nullptr) {
            return RT_ERR_PARAMETER;
        }
        const RtIoDriverOpsV1 *ops = driver->descriptor->io_ops;
        if (ops->update_configuration == nullptr) {
            return RT_ERR_NOT_SUPPORTED;
        }
        if (!driver->plugin->Enter()) {
            return RT_ERR_BUSY;
        }
        RtResult result =
            ops->update_configuration(driver->instance, config_json.c_str());
        driver->plugin->Leave();
        return result;
    }

    RtResult UpdateMapping(DriverHandle *driver,
                           const std::string &mapping_json) {
        if (driver == nullptr) {
            return RT_ERR_PARAMETER;
        }
        const RtIoDriverOpsV1 *ops = driver->descriptor->io_ops;
        if (ops->update_mapping == nullptr) {
            return RT_ERR_NOT_SUPPORTED;
        }
        if (!driver->plugin->Enter()) {
            return RT_ERR_BUSY;
        }
        RtResult result =
            ops->update_mapping(driver->instance, mapping_json.c_str());
        driver->plugin->Leave();
        return result;
    }

    RtResult IdentifyDriver(DriverHandle *driver) {
        if (driver == nullptr) {
            return RT_ERR_PARAMETER;
        }
        const RtIoDriverOpsV1 *ops = driver->descriptor->io_ops;
        if (ops->identify == nullptr) {
            return RT_ERR_NOT_SUPPORTED;
        }
        if (!driver->plugin->Enter()) {
            return RT_ERR_BUSY;
        }
        RtResult result = ops->identify(driver->instance);
        driver->plugin->Leave();
        return result;
    }

    RtResult StartBusCycle(DriverHandle *driver) {
        if (driver == nullptr) {
            return RT_ERR_PARAMETER;
        }
        const RtIoDriverOpsV1 *ops = driver->descriptor->io_ops;
        if (ops->start_bus_cycle == nullptr) {
            return RT_ERR_NOT_SUPPORTED;
        }
        if (!driver->plugin->Enter()) {
            return RT_ERR_BUSY;
        }
        RtResult result = ops->start_bus_cycle(driver->instance);
        driver->plugin->Leave();
        return result;
    }

    RtResult ScanModules(DriverHandle *driver) {
        if (driver == nullptr) {
            return RT_ERR_PARAMETER;
        }
        const RtIoDriverOpsV1 *ops = driver->descriptor->io_ops;
        if (ops->scan_modules == nullptr) {
            return RT_ERR_NOT_SUPPORTED;
        }
        if (!driver->plugin->Enter()) {
            return RT_ERR_BUSY;
        }
        RtResult result = ops->scan_modules(driver->instance);
        driver->plugin->Leave();
        return result;
    }

    RtResult GetDiagnosis(DriverHandle *driver, uint32_t *diagnosis) {
        if (driver == nullptr || diagnosis == nullptr) {
            return RT_ERR_PARAMETER;
        }
        const RtIoDriverOpsV1 *ops = driver->descriptor->io_ops;
        if (ops->get_diagnosis == nullptr) {
            return RT_ERR_NOT_SUPPORTED;
        }
        if (!driver->plugin->Enter()) {
            return RT_ERR_BUSY;
        }
        RtResult result = ops->get_diagnosis(driver->instance, diagnosis);
        driver->plugin->Leave();
        return result;
    }

    RtResult TriggerWatchdog(DriverHandle *driver) {
        if (driver == nullptr) {
            return RT_ERR_PARAMETER;
        }
        const RtIoDriverOpsV1 *ops = driver->descriptor->io_ops;
        if (ops->watchdog_trigger == nullptr) {
            return RT_ERR_NOT_SUPPORTED;
        }
        if (!driver->plugin->Enter()) {
            return RT_ERR_BUSY;
        }
        RtResult result = ops->watchdog_trigger(driver->instance);
        driver->plugin->Leave();
        return result;
    }

    RtResult ReadParameter(DriverHandle *driver,
                           uint32_t parameter_id,
                           void *data,
                           uint32_t size_bytes,
                           uint32_t bit_offset) {
        if (driver == nullptr || data == nullptr) {
            return RT_ERR_PARAMETER;
        }
        const RtIoParameterOpsV1 *ops = driver->descriptor->parameter_ops;
        if (ops == nullptr || ops->read_parameter == nullptr) {
            return RT_ERR_NOT_SUPPORTED;
        }
        if (!driver->plugin->Enter()) {
            return RT_ERR_BUSY;
        }
        RtResult result = ops->read_parameter(driver->instance,
                                              parameter_id,
                                              data,
                                              size_bytes,
                                              bit_offset);
        driver->plugin->Leave();
        return result;
    }

    RtResult WriteParameter(DriverHandle *driver,
                            uint32_t parameter_id,
                            const void *data,
                            uint32_t size_bytes,
                            uint32_t bit_offset) {
        if (driver == nullptr || data == nullptr) {
            return RT_ERR_PARAMETER;
        }
        const RtIoParameterOpsV1 *ops = driver->descriptor->parameter_ops;
        if (ops == nullptr || ops->write_parameter == nullptr) {
            return RT_ERR_NOT_SUPPORTED;
        }
        if (!driver->plugin->Enter()) {
            return RT_ERR_BUSY;
        }
        RtResult result = ops->write_parameter(driver->instance,
                                               parameter_id,
                                               data,
                                               size_bytes,
                                               bit_offset);
        driver->plugin->Leave();
        return result;
    }

    void StopAllDrivers() {
        std::vector<DriverHandle *> snapshot;
        {
            std::lock_guard<std::mutex> instance_lock(instance_mutex);
            snapshot.reserve(instances.size());
            for (const auto &entry : instances) {
                snapshot.push_back(entry.first);
            }
        }

        for (DriverHandle *driver : snapshot) {
            std::string error;
            RtResult result = StopDriver(driver, &error);
            if (result != RT_OK) {
                HostLog(2, error.c_str());
            }
        }
    }

    void DestroyAllDrivers() {
        std::vector<DriverHandle *> snapshot;
        {
            std::lock_guard<std::mutex> instance_lock(instance_mutex);
            snapshot.reserve(instances.size());
            for (const auto &entry : instances) {
                snapshot.push_back(entry.first);
            }
        }

        for (DriverHandle *driver : snapshot) {
            std::string error;
            RtResult result = DestroyDriver(driver, &error);
            if (result != RT_OK) {
                HostLog(2, error.c_str());
            }
        }
    }

    void UnloadAll() {
        DestroyAllDrivers();

        std::vector<std::string> plugin_ids;
        {
            std::lock_guard<std::mutex> registry_lock(registry_mutex);
            plugin_ids.reserve(plugins.size());
            for (const auto &entry : plugins) {
                plugin_ids.push_back(entry.first);
            }
        }

        for (const std::string &plugin_id : plugin_ids) {
            std::string error;
            RtResult result = UnloadPlugin(plugin_id, &error);
            if (result != RT_OK) {
                HostLog(2, error.c_str());
            }
        }
    }

    bool HasPlugin(const std::string &plugin_id) const {
        std::lock_guard<std::mutex> registry_lock(registry_mutex);
        return plugins.find(plugin_id) != plugins.end();
    }

    size_t plugin_count() const {
        std::lock_guard<std::mutex> registry_lock(registry_mutex);
        return plugins.size();
    }

    size_t instance_count() const {
        std::lock_guard<std::mutex> instance_lock(instance_mutex);
        return instances.size();
    }

private:
    RtResult InvokeChannels(DriverHandle *driver,
                            RtIoChannelV1 *channels,
                            uint32_t count,
                            bool read) {
        if (driver == nullptr || (count != 0 && channels == nullptr)) {
            return RT_ERR_PARAMETER;
        }

        const RtIoDriverOpsV1 *ops = driver->descriptor->io_ops;
        auto operation = read ? ops->read_inputs : ops->write_outputs;
        if (operation == nullptr) {
            return RT_ERR_NOT_SUPPORTED;
        }
        if (!driver->plugin->Enter()) {
            return RT_ERR_BUSY;
        }
        RtResult result = operation(driver->instance, channels, count);
        driver->plugin->Leave();
        return result;
    }

    RtHostApiV1 host_api{};
    mutable std::mutex load_mutex;
    mutable std::mutex registry_mutex;
    mutable std::mutex instance_mutex;
    std::unordered_map<std::string, std::shared_ptr<PluginRecord>> plugins;
    std::unordered_map<DriverHandle *, std::unique_ptr<DriverHandle>>
        instances;
};

PluginManager::PluginManager() : impl_(new Impl()) {}

PluginManager::~PluginManager() = default;

RtResult PluginManager::LoadPlugin(const std::string &path,
                                   const LoadOptions &options,
                                   std::string *plugin_id,
                                   std::string *error) {
    return impl_->LoadPlugin(path, options, plugin_id, error);
}

RtResult PluginManager::LoadManifest(const std::string &path,
                                     std::string *error) {
    return impl_->LoadManifest(path, error);
}

RtResult PluginManager::UnloadPlugin(const std::string &plugin_id,
                                     std::string *error) {
    return impl_->UnloadPlugin(plugin_id, error);
}

RtResult PluginManager::CallHook(const std::string &plugin_id,
                                 uint32_t hook,
                                 uintptr_t param1,
                                 uintptr_t param2,
                                 std::string *error) {
    return impl_->CallHook(plugin_id, hook, param1, param2, error);
}

RtResult PluginManager::CallAllHooks(uint32_t hook,
                                     uintptr_t param1,
                                     uintptr_t param2,
                                     std::string *error) {
    return impl_->CallAllHooks(hook, param1, param2, error);
}

RtResult PluginManager::CreateDriver(const std::string &plugin_id,
                                     const std::string &driver_type_id,
                                     const std::string &config_json,
                                     DriverHandle **driver,
                                     std::string *error) {
    return impl_->CreateDriver(
        plugin_id, driver_type_id, config_json, driver, error);
}

RtResult PluginManager::StartDriver(DriverHandle *driver,
                                    std::string *error) {
    return impl_->StartDriver(driver, error);
}

RtResult PluginManager::StopDriver(DriverHandle *driver,
                                   std::string *error) {
    return impl_->StopDriver(driver, error);
}

RtResult PluginManager::DestroyDriver(DriverHandle *driver,
                                      std::string *error) {
    return impl_->DestroyDriver(driver, error);
}

RtResult PluginManager::ReadInputs(DriverHandle *driver,
                                   RtIoChannelV1 *channels,
                                   uint32_t count) {
    return impl_->ReadInputs(driver, channels, count);
}

RtResult PluginManager::WriteOutputs(DriverHandle *driver,
                                     RtIoChannelV1 *channels,
                                     uint32_t count) {
    return impl_->WriteOutputs(driver, channels, count);
}

RtResult PluginManager::GetDriverInfo(DriverHandle *driver,
                                      RtIoDriverInfoV1 *info) {
    return impl_->GetDriverInfo(driver, info);
}

RtResult PluginManager::UpdateConfiguration(DriverHandle *driver,
                                            const std::string &config_json) {
    return impl_->UpdateConfiguration(driver, config_json);
}

RtResult PluginManager::UpdateMapping(DriverHandle *driver,
                                      const std::string &mapping_json) {
    return impl_->UpdateMapping(driver, mapping_json);
}

RtResult PluginManager::IdentifyDriver(DriverHandle *driver) {
    return impl_->IdentifyDriver(driver);
}

RtResult PluginManager::StartBusCycle(DriverHandle *driver) {
    return impl_->StartBusCycle(driver);
}

RtResult PluginManager::ScanModules(DriverHandle *driver) {
    return impl_->ScanModules(driver);
}

RtResult PluginManager::GetDiagnosis(DriverHandle *driver,
                                     uint32_t *diagnosis) {
    return impl_->GetDiagnosis(driver, diagnosis);
}

RtResult PluginManager::TriggerWatchdog(DriverHandle *driver) {
    return impl_->TriggerWatchdog(driver);
}

RtResult PluginManager::ReadParameter(DriverHandle *driver,
                                      uint32_t parameter_id,
                                      void *data,
                                      uint32_t size_bytes,
                                      uint32_t bit_offset) {
    return impl_->ReadParameter(
        driver, parameter_id, data, size_bytes, bit_offset);
}

RtResult PluginManager::WriteParameter(DriverHandle *driver,
                                       uint32_t parameter_id,
                                       const void *data,
                                       uint32_t size_bytes,
                                       uint32_t bit_offset) {
    return impl_->WriteParameter(
        driver, parameter_id, data, size_bytes, bit_offset);
}

void PluginManager::StopAllDrivers() {
    impl_->StopAllDrivers();
}

void PluginManager::DestroyAllDrivers() {
    impl_->DestroyAllDrivers();
}

void PluginManager::UnloadAll() {
    impl_->UnloadAll();
}

bool PluginManager::HasPlugin(const std::string &plugin_id) const {
    return impl_->HasPlugin(plugin_id);
}

size_t PluginManager::plugin_count() const {
    return impl_->plugin_count();
}

size_t PluginManager::instance_count() const {
    return impl_->instance_count();
}

} // namespace runtime_plugin
