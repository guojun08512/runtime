#pragma once

#include "runtime_plugin/plugin_api.h"

#include <cstddef>
#include <memory>
#include <string>

namespace runtime_plugin {

struct LoadOptions {
    bool auto_start_default_driver = false;
    std::string config_json = "{}";
};

class DriverHandle;

class PluginManager {
public:
    PluginManager();
    ~PluginManager();

    PluginManager(const PluginManager &) = delete;
    PluginManager &operator=(const PluginManager &) = delete;

    RtResult LoadPlugin(const std::string &path,
                        const LoadOptions &options,
                        std::string *plugin_id,
                        std::string *error);

    RtResult LoadManifest(const std::string &path, std::string *error);

    RtResult UnloadPlugin(const std::string &plugin_id, std::string *error);
    RtResult CallHook(const std::string &plugin_id,
                      uint32_t hook,
                      uintptr_t param1,
                      uintptr_t param2,
                      std::string *error);
    RtResult CallAllHooks(uint32_t hook,
                          uintptr_t param1,
                          uintptr_t param2,
                          std::string *error);

    RtResult CreateDriver(const std::string &plugin_id,
                          const std::string &driver_type_id,
                          const std::string &config_json,
                          DriverHandle **driver,
                          std::string *error);

    RtResult StartDriver(DriverHandle *driver, std::string *error);
    RtResult StopDriver(DriverHandle *driver, std::string *error);
    // Call only after the scheduler and all I/O users of this handle stopped.
    RtResult DestroyDriver(DriverHandle *driver, std::string *error);

    RtResult ReadInputs(DriverHandle *driver,
                        RtIoChannelV1 *channels,
                        uint32_t count);
    RtResult WriteOutputs(DriverHandle *driver,
                          RtIoChannelV1 *channels,
                          uint32_t count);
    RtResult GetDriverInfo(DriverHandle *driver, RtIoDriverInfoV1 *info);
    RtResult UpdateConfiguration(DriverHandle *driver,
                                 const std::string &config_json);
    RtResult UpdateMapping(DriverHandle *driver,
                           const std::string &mapping_json);
    RtResult IdentifyDriver(DriverHandle *driver);
    RtResult StartBusCycle(DriverHandle *driver);
    RtResult ScanModules(DriverHandle *driver);
    RtResult GetDiagnosis(DriverHandle *driver, uint32_t *diagnosis);
    RtResult TriggerWatchdog(DriverHandle *driver);

    RtResult ReadParameter(DriverHandle *driver,
                           uint32_t parameter_id,
                           void *data,
                           uint32_t size_bytes,
                           uint32_t bit_offset);
    RtResult WriteParameter(DriverHandle *driver,
                            uint32_t parameter_id,
                            const void *data,
                            uint32_t size_bytes,
                            uint32_t bit_offset);

    void StopAllDrivers();
    void DestroyAllDrivers();
    void UnloadAll();

    bool HasPlugin(const std::string &plugin_id) const;
    size_t plugin_count() const;
    size_t instance_count() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace runtime_plugin
