#include "libipc/ipc.h"
#include "runtime_core/logging.h"

#include <chrono>
#include <sstream>
#include <string>
#include <thread>

namespace {

bool SendTask(ipc::route &sender,
              const std::string &type,
              const std::string &label,
              int priority) {
    std::ostringstream command;
    command << R"({"type":")" << type << R"(",)"
            << R"("label":")" << label << R"(",)"
            << R"("intervalMs":1,)"
            << R"("priority":)" << priority << R"(,)"
            << R"("config":{"taskType":")" << type << R"("}})";

    const std::string payload = command.str();
    if (!sender.send(payload.c_str(), payload.size() + 1, 2000000ull)) {
        LOG(ERROR) << "[plugin-task-sender] send failed: " << label;
        return false;
    }
    LOG(INFO) << "[plugin-task-sender] sent: " << payload;
    return true;
}

} // namespace

int main(int argc, char **argv) {
    runtime_logging::Init(argv[0]);
    if (argc != 1) {
        LOG(ERROR) << "usage: plugin_task_sender";
        return 2;
    }

    ipc::route sender("runtime_task_route", ipc::sender);
    if (!sender.valid()) {
        LOG(ERROR) << "[plugin-task-sender] open route failed";
        return 1;
    }

    if (!SendTask(sender, "iectask", "native-iec-task", 12) ||
        !SendTask(sender, "io", "native-io-task", 11) ||
        !SendTask(sender, "network", "native-network-task", 10)) {
        return 3;
    }

    std::this_thread::sleep_for(std::chrono::seconds(3));
    return 0;
}
