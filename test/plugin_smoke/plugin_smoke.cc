#include "runtime_plugin/plugin_manager.h"
#include "runtime_core/logging.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

namespace {

bool Check(RtResult result, const std::string &operation, std::string *error) {
    if (result == RT_OK) {
        return true;
    }
    LOG(ERROR) << operation << " failed: result=" << result;
    if (error != nullptr && !error->empty()) {
        LOG(ERROR) << operation << " error: " << *error;
    }
    return false;
}

} // namespace

int main(int argc, char **argv) {
    runtime_logging::Init(argv[0]);
    if (argc != 2) {
        LOG(ERROR) << "usage: runtime_plugin_smoke <plugin.so>";
        return 2;
    }

    runtime_plugin::PluginManager manager;
    std::string plugin_id;
    std::string error;

    runtime_plugin::LoadOptions options;
    if (!Check(manager.LoadPlugin(argv[1], options, &plugin_id, &error),
               "LoadPlugin",
               &error)) {
        return 1;
    }

    runtime_plugin::DriverHandle *driver = nullptr;
    if (!Check(manager.CreateDriver(plugin_id,
                                    "runtime.example_io",
                                    "{}",
                                    &driver,
                                    &error),
               "CreateDriver",
               &error)) {
        return 1;
    }

    RtIoDriverInfoV1 info{};
    if (!Check(manager.GetDriverInfo(driver, &info),
               "GetDriverInfo",
               &error) ||
        info.driver_name == nullptr ||
        std::strcmp(info.driver_name, "Runtime Example I/O Driver") != 0 ||
        !Check(manager.UpdateConfiguration(driver, "{\"test\":1}"),
               "UpdateConfiguration",
               &error) ||
        !Check(manager.UpdateMapping(driver, "{\"map\":1}"),
               "UpdateMapping",
               &error) ||
        !Check(manager.IdentifyDriver(driver), "IdentifyDriver", &error)) {
        return 1;
    }

    if (!Check(manager.StartDriver(driver, &error), "StartDriver", &error)) {
        return 1;
    }

    if (!Check(manager.StartBusCycle(driver), "StartBusCycle", &error) ||
        !Check(manager.ScanModules(driver), "ScanModules", &error) ||
        !Check(manager.TriggerWatchdog(driver),
               "TriggerWatchdog",
               &error)) {
        return 1;
    }

    const unsigned char output[2] = {0xAA, 0x55};
    RtIoChannelV1 output_channel = {
        nullptr,
        const_cast<unsigned char *>(output),
        sizeof(output),
        0,
    };
    if (!Check(manager.WriteOutputs(driver, &output_channel, 1),
               "WriteOutputs",
               &error)) {
        return 1;
    }
    if (!Check(manager.StartBusCycle(driver), "StartBusCycle", &error)) {
        return 1;
    }

    unsigned char input[4] = {};
    RtIoChannelV1 input_channel = {nullptr, input, sizeof(input), 0};
    if (!Check(manager.ReadInputs(driver, &input_channel, 1),
               "ReadInputs",
               &error)) {
        return 1;
    }
    if (input[0] != 0xAA || input[3] != 0xAA) {
        LOG(ERROR) << "unexpected input data";
        return 1;
    }

    unsigned char parameter[4] = {};
    if (!Check(manager.ReadParameter(driver, 1, parameter, sizeof(parameter), 0),
               "ReadParameter",
               &error)) {
        return 1;
    }
    if (parameter[0] != 0x11 || parameter[3] != 0x44) {
        LOG(ERROR) << "unexpected parameter data";
        return 1;
    }

    const unsigned char parameter_write[2] = {0x77, 0x66};
    if (!Check(manager.WriteParameter(driver,
                                      1,
                                      parameter_write,
                                      sizeof(parameter_write),
                                      0),
               "WriteParameter",
               &error) ||
        !Check(manager.ReadParameter(driver, 1, parameter, sizeof(parameter), 0),
               "ReadParameter",
               &error) ||
        parameter[0] != 0x77 || parameter[1] != 0x66) {
        return 1;
    }

    uint32_t diagnosis = 0;
    if (!Check(manager.GetDiagnosis(driver, &diagnosis),
               "GetDiagnosis",
               &error) ||
        diagnosis == 0) {
        return 1;
    }

    if (!Check(manager.StopDriver(driver, &error), "StopDriver", &error) ||
        !Check(manager.DestroyDriver(driver, &error),
               "DestroyDriver",
               &error) ||
        !Check(manager.UnloadPlugin(plugin_id, &error),
               "UnloadPlugin",
               &error)) {
        return 1;
    }

    if (manager.plugin_count() != 0 || manager.instance_count() != 0) {
        LOG(ERROR) << "plugin manager did not release all resources";
        return 1;
    }

    const std::filesystem::path manifest =
        std::filesystem::temp_directory_path() /
        ("runtime_plugin_smoke_" + std::to_string(getpid()) + ".json");
    const std::string absolute_plugin =
        std::filesystem::absolute(argv[1]).string();
    {
        std::ofstream output(manifest);
        output << "{\"plugins\":[{\"path\":\"" << absolute_plugin
               << "\",\"auto_start\":true,\"config\":{}}]}";
    }

    if (!Check(manager.LoadManifest(manifest.string(), &error),
               "LoadManifest",
               &error)) {
        std::filesystem::remove(manifest);
        return 1;
    }
    if (manager.plugin_count() != 1 || manager.instance_count() != 1) {
        LOG(ERROR) << "manifest did not create the expected plugin instance";
        std::filesystem::remove(manifest);
        return 1;
    }

    manager.StopAllDrivers();
    manager.DestroyAllDrivers();
    manager.UnloadAll();
    std::filesystem::remove(manifest);

    LOG(INFO) << "plugin smoke test passed";
    return 0;
}
