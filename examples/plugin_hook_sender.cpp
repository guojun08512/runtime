#include "libipc/ipc.h"
#include "runtime_core/logging.h"

#include <chrono>
#include <filesystem>
#include <sstream>
#include <string>
#include <thread>

namespace {

std::string EscapeJson(const std::string &value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char character : value) {
        if (character == '\\' || character == '"') {
            escaped.push_back('\\');
        }
        escaped.push_back(character);
    }
    return escaped;
}

} // namespace

int main(int argc, char **argv) {
    runtime_logging::Init(argv[0]);
    if (argc != 2) {
        LOG(ERROR) << "usage: plugin_hook_sender <plugin.so>";
        return 2;
    }

    const std::string plugin_path =
        std::filesystem::absolute(argv[1]).string();
    ipc::route sender("runtime_task_route", ipc::sender);
    if (!sender.valid()) {
        LOG(ERROR) << "[plugin-hook-sender] open route failed";
        return 1;
    }

    std::ostringstream command;
    command << R"({"command":"plugin.hook_task",)"
            << R"("label":"plugin-hook-task-1",)"
            << R"("intervalMs":1,)"
            << R"("priority":20,)"
            << R"("pluginId":"runtime.example_plugin",)"
            << R"("pluginPath":")" << EscapeJson(plugin_path) << R"(",)"
            << R"("config":{"poll_interval_ms":1}})";

    const std::string payload = command.str();
    if (!sender.send(payload.c_str(), payload.size() + 1, 2000000ull)) {
        LOG(ERROR) << "[plugin-hook-sender] send failed";
        return 3;
    }

    LOG(INFO) << "[plugin-hook-sender] sent: " << payload;
    std::this_thread::sleep_for(std::chrono::seconds(3));
    return 0;
}
