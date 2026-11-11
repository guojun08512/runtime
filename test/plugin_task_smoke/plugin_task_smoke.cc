#include "runtime_core/global.h"
#include "runtime_core/logging.h"
#include "runtime_core/tasks.h"
#include "runtime_core/triple_buffer.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

namespace {

std::atomic<uint64_t> g_iec_callbacks{0};
std::atomic<uint64_t> g_io_callbacks{0};
std::atomic<uint64_t> g_network_callbacks{0};

uint32_t ReadU32LE(const unsigned char *data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

void WriteU32LE(unsigned char *data, uint32_t value) {
    std::memcpy(data, &value, sizeof(value));
}

int CustomIecCallback(const RuntimeTaskContext *context) {
    if (context->event_id != RT_HOOK_COMM_CYCLE) {
        return 0;
    }
    if (context->output_data == nullptr || context->output_size < 8) {
        return 1;
    }
    auto *output = static_cast<unsigned char *>(context->output_data);
    WriteU32LE(output, 0x43494543u);
    output[4] = static_cast<unsigned char>(context->sequence);
    output[5] = context->input_data == nullptr
                    ? 0
                    : static_cast<const unsigned char *>(
                          context->input_data)[0];
    ++g_iec_callbacks;
    return 0;
}

int CustomIoCallback(const RuntimeTaskContext *context) {
    if (context->event_id != RT_HOOK_COMM_CYCLE) {
        return 0;
    }
    if (context->output_data == nullptr || context->output_size < 8) {
        return 1;
    }
    auto *output = static_cast<unsigned char *>(context->output_data);
    WriteU32LE(output, 0x43494f49u);
    output[4] = static_cast<unsigned char>(context->sequence);
    output[5] = context->input_data == nullptr
                    ? 0
                    : static_cast<const unsigned char *>(
                          context->input_data)[0];
    ++g_io_callbacks;
    return 0;
}

int CustomNetworkCallback(const RuntimeTaskContext *context) {
    if (context->event_id != RT_HOOK_COMM_CYCLE) {
        return 0;
    }
    if (context->output_data == nullptr || context->output_size < 8) {
        return 1;
    }
    auto *output = static_cast<unsigned char *>(context->output_data);
    WriteU32LE(output, 0x434e4554u);
    output[4] = static_cast<unsigned char>(context->sequence);
    output[5] = context->input_data == nullptr
                    ? 0
                    : static_cast<const unsigned char *>(
                          context->input_data)[0];
    ++g_network_callbacks;
    return 0;
}

Config::TaskData MakeTaskData(const std::string &label,
                              double interval_ms,
                              int priority,
                              Config::TaskType type,
                              const std::string &executor) {
    Config::TaskData data;
    data.type = type;
    data.runId = 1;
    data.taskId = 0;
    data.taskLabel = label;
    data.isPeriodic = true;
    data.executorName = executor;
    data.intervalMs =
        static_cast<uint32_t>(interval_ms);
    data.priority = priority;
    return data;
}

} // namespace

int main(int argc, char **argv) {
    runtime_logging::Init(argv[0]);
    if (argc != 1) {
        LOG(ERROR) << "usage: runtime_plugin_task_smoke";
        return 2;
    }

    auto *cfg = Config::Instance();
    cfg->plugin_manager = nullptr;
    TaskCallbackRegistry::Instance().Register(
        Config::TaskType::IECTASK, "custom", CustomIecCallback);
    TaskCallbackRegistry::Instance().Register(
        Config::TaskType::FILE_IO, "custom", CustomIoCallback);
    TaskCallbackRegistry::Instance().Register(
        Config::TaskType::NETWORK_IO, "custom", CustomNetworkCallback);
    cfg->ioTasksPending.store(2);
    cfg->ioTasksCompleted.store(0);
    cfg->ioTasksFailed.store(0);

    Config::TaskData io_data = MakeTaskData("native-io-task",
                                            1,
                                            10,
                                            Config::TaskType::FILE_IO,
                                            "custom");
    Config::TaskData network_data = MakeTaskData("native-network-task",
                                                 1,
                                                 9,
                                                 Config::TaskType::NETWORK_IO,
                                                 "custom");
    Config::TaskData iec_data = MakeTaskData("native-iec-task",
                                             1,
                                             8,
                                             Config::TaskType::IECTASK,
                                             "custom");

    FileIOConsumerTask io_task(io_data);
    NetworkIOConsumerTask network_task(network_data);
    IecTask iec_task(iec_data);

    const std::string io_input_name =
        runtime_core::TaskBuffers::InputName(io_data.taskLabel);
    const std::string network_input_name =
        runtime_core::TaskBuffers::InputName(network_data.taskLabel);
    const std::string iec_input_name =
        runtime_core::TaskBuffers::InputName(iec_data.taskLabel);
    runtime_core::TripleBuffer io_input(
        io_input_name,
        io_data.inputBytes,
        runtime_core::TripleBuffer::Role::Writer);
    runtime_core::TripleBuffer network_input(
        network_input_name,
        network_data.inputBytes,
        runtime_core::TripleBuffer::Role::Writer);
    runtime_core::TripleBuffer iec_input(
        iec_input_name,
        iec_data.inputBytes,
        runtime_core::TripleBuffer::Role::Writer);
    auto *io_input_data = static_cast<unsigned char *>(io_input.AcquireWrite());
    auto *network_input_data =
        static_cast<unsigned char *>(network_input.AcquireWrite());
    auto *iec_input_data =
        static_cast<unsigned char *>(iec_input.AcquireWrite());
    io_input_data[0] = 0x31;
    network_input_data[0] = 0x32;
    iec_input_data[0] = 0x33;
    io_input.Publish();
    network_input.Publish();
    iec_input.Publish();

    {
        std::lock_guard<std::mutex> lock(cfg->periodicTasksMutex);
        cfg->periodicTasks.push_back(
            {false, &io_task, nullptr, io_data,
             std::chrono::steady_clock::now()});
        cfg->periodicTasks.push_back(
            {false, &network_task, nullptr, network_data,
             std::chrono::steady_clock::now()});
        cfg->periodicTasks.push_back(
            {false, &iec_task, nullptr, iec_data,
             std::chrono::steady_clock::now()});
    }

    enki::TaskSchedulerConfig scheduler_config;
    scheduler_config.numTaskThreadsToCreate +=
        static_cast<uint32_t>(Config::IOThreadId::NUM);
    cfg->ts.Initialize(scheduler_config);
    cfg->ioThreadStartIndex =
        cfg->ts.GetNumTaskThreads() -
        static_cast<uint32_t>(Config::IOThreadId::NUM);

    IECLoopTask io_loops[static_cast<uint32_t>(Config::IOThreadId::NUM)];
    for (uint32_t i = 0; i < static_cast<uint32_t>(Config::IOThreadId::NUM);
         ++i) {
        io_loops[i].threadNum = cfg->ioThreadStartIndex + i;
        cfg->ts.AddPinnedTask(&io_loops[i]);
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((io_task.GetState() != Config::TaskState::COMPLETED ||
            network_task.GetState() != Config::TaskState::COMPLETED ||
            iec_task.GetState() != Config::TaskState::COMPLETED) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    cfg->stopFlag = true;
    cfg->ts.WaitforAllAndShutdown();

    runtime_core::TripleBuffer io_output(
        runtime_core::TaskBuffers::OutputName(io_data.taskLabel),
        io_data.outputBytes,
        runtime_core::TripleBuffer::Role::Reader);
    runtime_core::TripleBuffer network_output(
        runtime_core::TaskBuffers::OutputName(network_data.taskLabel),
        network_data.outputBytes,
        runtime_core::TripleBuffer::Role::Reader);
    runtime_core::TripleBuffer iec_output(
        runtime_core::TaskBuffers::OutputName(iec_data.taskLabel),
        iec_data.outputBytes,
        runtime_core::TripleBuffer::Role::Reader);
    const auto *io_output_data =
        static_cast<const unsigned char *>(io_output.AcquireRead(nullptr));
    const auto *network_output_data = static_cast<const unsigned char *>(
        network_output.AcquireRead(nullptr));
    const auto *iec_output_data =
        static_cast<const unsigned char *>(iec_output.AcquireRead(nullptr));

    if (io_task.GetState() != Config::TaskState::COMPLETED ||
        network_task.GetState() != Config::TaskState::COMPLETED ||
        iec_task.GetState() != Config::TaskState::COMPLETED ||
        cfg->ioTasksFailed.load() != 0 ||
        io_output_data == nullptr || network_output_data == nullptr ||
        iec_output_data == nullptr ||
        ReadU32LE(io_output_data) != 0x43494f49u ||
        ReadU32LE(network_output_data) != 0x434e4554u ||
        ReadU32LE(iec_output_data) != 0x43494543u ||
        io_output_data[5] != 0x31 ||
        network_output_data[5] != 0x32 ||
        iec_output_data[5] != 0x33 ||
        g_io_callbacks.load() == 0 ||
        g_network_callbacks.load() == 0 ||
        g_iec_callbacks.load() == 0) {
        LOG(ERROR) << "native tasks did not execute registered callbacks";
        return 1;
    }

    {
        std::lock_guard<std::mutex> lock(cfg->periodicTasksMutex);
        cfg->periodicTasks.clear();
    }
    LOG(INFO) << "native IO/NETWORK/IEC task smoke test passed";
    return 0;
}
