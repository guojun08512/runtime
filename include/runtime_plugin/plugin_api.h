#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define RT_PLUGIN_EXPORT __declspec(dllexport)
#define RT_PLUGIN_CALL __cdecl
#else
#define RT_PLUGIN_EXPORT __attribute__((visibility("default")))
#define RT_PLUGIN_CALL
#endif

#define RT_PLUGIN_ABI_V1 1u

typedef uint32_t RtResult;

enum {
    RT_OK = 0,
    RT_ERR_PARAMETER = 1,
    RT_ERR_ABI = 2,
    RT_ERR_NOMEMORY = 3,
    RT_ERR_FAILED = 4,
    RT_ERR_NOT_SUPPORTED = 5,
    RT_ERR_BUSY = 6,
    RT_ERR_NOT_FOUND = 7
};

typedef void (RT_PLUGIN_CALL *RtLogFn)(int level, const char *message);
typedef void *(RT_PLUGIN_CALL *RtAllocFn)(size_t size);
typedef void (RT_PLUGIN_CALL *RtFreeFn)(void *ptr);

typedef struct RtHostApiV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    RtLogFn log;
    RtAllocFn alloc;
    RtFreeFn free;
} RtHostApiV1;

typedef struct RtIoChannelV1 {
    void *driver_specific;
    void *data;
    uint32_t size_bytes;
    uint32_t bit_offset;
} RtIoChannelV1;

typedef struct RtIoDriverInfoV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    const char *driver_name;
    const char *device_name;
    const char *vendor_name;
    const char *firmware_version;
    uint32_t module_type;
    uint32_t version;
} RtIoDriverInfoV1;

typedef struct RtIoDriverOpsV1 {
    uint32_t abi_version;
    uint32_t struct_size;

    RtResult (RT_PLUGIN_CALL *create)(const RtHostApiV1 *host,
                                      const char *json_config,
                                      void **instance);
    void (RT_PLUGIN_CALL *destroy)(void *instance);
    RtResult (RT_PLUGIN_CALL *configure)(void *instance,
                                         const char *json_config);
    RtResult (RT_PLUGIN_CALL *start)(void *instance);
    void (RT_PLUGIN_CALL *stop)(void *instance);
    RtResult (RT_PLUGIN_CALL *read_inputs)(void *instance,
                                           RtIoChannelV1 *channels,
                                           uint32_t count);
    RtResult (RT_PLUGIN_CALL *write_outputs)(void *instance,
                                             RtIoChannelV1 *channels,
                                             uint32_t count);
    RtResult (RT_PLUGIN_CALL *scan_modules)(void *instance);
    RtResult (RT_PLUGIN_CALL *get_diagnosis)(void *instance,
                                             uint32_t *diagnosis);
    RtResult (RT_PLUGIN_CALL *watchdog_trigger)(void *instance);
    RtResult (RT_PLUGIN_CALL *get_info)(void *instance,
                                        RtIoDriverInfoV1 *info);
    RtResult (RT_PLUGIN_CALL *update_configuration)(
        void *instance, const char *json_config);
    RtResult (RT_PLUGIN_CALL *update_mapping)(
        void *instance, const char *json_mapping);
    RtResult (RT_PLUGIN_CALL *identify)(void *instance);
    RtResult (RT_PLUGIN_CALL *start_bus_cycle)(void *instance);
} RtIoDriverOpsV1;

typedef struct RtIoParameterOpsV1 {
    uint32_t abi_version;
    uint32_t struct_size;

    RtResult (RT_PLUGIN_CALL *read_parameter)(void *instance,
                                              uint32_t parameter_id,
                                              void *data,
                                              uint32_t size_bytes,
                                              uint32_t bit_offset);
    RtResult (RT_PLUGIN_CALL *write_parameter)(void *instance,
                                               uint32_t parameter_id,
                                               const void *data,
                                               uint32_t size_bytes,
                                               uint32_t bit_offset);
} RtIoParameterOpsV1;

typedef enum RtHookIdV1 {
    RT_HOOK_INIT_SYSTEM = 1,
    RT_HOOK_INIT,
    RT_HOOK_INIT2,
    RT_HOOK_INIT3,
    RT_HOOK_INIT_TASKS,
    RT_HOOK_INIT_COMM,
    RT_HOOK_COMM_CYCLE,
    RT_HOOK_EXIT_COMM,
    RT_HOOK_EXIT_TASKS,
    RT_HOOK_EXIT3,
    RT_HOOK_EXIT2,
    RT_HOOK_EXIT,
    RT_HOOK_EXIT_SYSTEM
} RtHookIdV1;

typedef RtResult (RT_PLUGIN_CALL *RtHookFunctionV1)(uint32_t hook,
                                                    uintptr_t param1,
                                                    uintptr_t param2);

typedef enum RtTaskKindV1 {
    RT_TASK_KIND_IEC = 1,
    RT_TASK_KIND_IO = 2,
    RT_TASK_KIND_NETWORK = 3
} RtTaskKindV1;

typedef struct RtTaskContextV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t event_id;
    uint32_t task_kind;
    uint64_t sequence;
    const char *task_id;
    const char *config_json;
    const void *input_data;
    uint32_t input_size;
    uint64_t input_sequence;
    void *output_data;
    uint32_t output_size;
} RtTaskContextV1;

typedef int (RT_PLUGIN_CALL *RtTaskCallbackV1)(
    const RtTaskContextV1 *context);

typedef struct RtTaskCallbackDescV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    const char *executor_name;
    uint32_t task_kind;
    RtTaskCallbackV1 callback;
} RtTaskCallbackDescV1;

typedef struct RtDriverDescV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    const char *type_id;
    const char *display_name;
    const char *device_name;
    const char *vendor_name;
    const char *firmware_version;
    uint32_t version;
    const RtIoDriverOpsV1 *io_ops;
    const RtIoParameterOpsV1 *parameter_ops;
} RtDriverDescV1;

typedef struct RtPluginV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    const char *plugin_id;
    const char *display_name;
    uint32_t version;
    const RtDriverDescV1 *const *drivers;
    uint32_t driver_count;
    RtResult (RT_PLUGIN_CALL *init)(const RtHostApiV1 *host);
    void (RT_PLUGIN_CALL *shutdown)(void);
    RtHookFunctionV1 hook;
    const RtTaskCallbackDescV1 *const *callbacks;
    uint32_t callback_count;
} RtPluginV1;

typedef const RtPluginV1 *(RT_PLUGIN_CALL *RtPluginQueryV1)(
    uint32_t host_abi_version);

#ifdef __cplusplus
extern "C" {
#endif

RT_PLUGIN_EXPORT const RtPluginV1 *RT_PLUGIN_CALL
runtime_plugin_query_v1(uint32_t host_abi_version);

#ifdef __cplusplus
}
#endif
