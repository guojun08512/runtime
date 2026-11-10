#include "runtime_core/logging.h"
#include <thread>
#include <chrono>
#include "libipc/ipc.h"

int main(int argc, char **argv) {
    runtime_logging::Init(argv[0]);
    const char *routeName = "runtime_task_route";

    ipc::route sender(routeName, ipc::sender);
    if (!sender.valid()) {
        LOG(ERROR) << "[sender] open route failed: " << routeName;
        return 1;
    }

    // 发送JSON任务1
    std::string json1 = R"({
        "type":"iectask",
        "intervalMs":1,
        "priority":10,
        "label":"iectask-test-1"
    })";

    if (!sender.send(json1.c_str(), json1.size() + 1, 2000000ull)) {
        LOG(ERROR) << "[sender] send json1 failed";
        return 2;
    }
    LOG(INFO) << "[sender] sent: " << json1;

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 发送JSON任务2
    std::string json2 = R"({
        "type":"file",
        "intervalMs":10,
        "priority":12,
        "label":"file-test-1"
    })";

    if (!sender.send(json2.c_str(), json2.size() + 1, 2000000ull)) {
        LOG(ERROR) << "[sender] send json2 failed";
        return 3;
    }
    LOG(INFO) << "[sender] sent: " << json2;

    LOG(INFO) << "[sender] wait for 3s for runtime to execute tasks...";
    std::this_thread::sleep_for(std::chrono::seconds(3));
    LOG(INFO) << "[sender] done";
    return 0;
}
