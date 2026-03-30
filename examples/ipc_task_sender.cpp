#include <iostream>
#include <thread>
#include <chrono>
#include "libipc/ipc.h"

int main() {
    const char *routeName = "runtime_task_route";

    ipc::route sender(routeName, ipc::sender);
    if (!sender.valid()) {
        std::cerr << "[sender] open route failed: " << routeName << std::endl;
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
        std::cerr << "[sender] send json1 failed" << std::endl;
        return 2;
    }
    std::cout << "[sender] sent: " << json1 << std::endl;

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 发送JSON任务2
    std::string json2 = R"({
        "type":"file",
        "intervalMs":10,
        "priority":12,
        "label":"file-test-1"
    })";

    if (!sender.send(json2.c_str(), json2.size() + 1, 2000000ull)) {
        std::cerr << "[sender] send json2 failed" << std::endl;
        return 3;
    }
    std::cout << "[sender] sent: " << json2 << std::endl;

    std::cout << "[sender] wait for 3s for runtime to execute tasks..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(3));
    std::cout << "[sender] done" << std::endl;
    return 0;
}
