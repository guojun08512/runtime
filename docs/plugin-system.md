# Runtime Dynamic Plugin System

The runtime uses a small, versioned C ABI for dynamically loadable I/O
components. It follows the useful parts of the CODESYS component model but does
not use COM-style `AddRef`, `Release`, or `QueryInterface`.

## Design

- A plugin exports one symbol: `runtime_plugin_query_v1`.
- The host passes an immutable `RtHostApiV1` table to the plugin.
- The plugin returns immutable descriptors for itself and its drivers.
- Every plugin provides a `HookFunction`-style callback for lifecycle and
  cyclic work.
- Driver instances are opaque `void *` pointers owned by the plugin.
- The runtime owns all handles and calls the plugin only through function
  pointers.
- Unloading is rejected while an instance or callback is still active.

All cross-library structures use fixed-width integer types and plain C data.
Do not pass `std::string`, STL containers, or exceptions across the ABI.

## Manifest

The runtime accepts a manifest with `--plugins <path>` or
`--plugins=<path>`. The `RUNTIME_PLUGIN_CONFIG` environment variable is also
supported. Without an explicit option, the runtime searches for `plugins.json`
next to the working directory or executable.

```json
{
  "plugins": [
    {
      "path": "../plugins/libruntime_example_io_plugin.so",
      "auto_start": true,
      "config": {
        "poll_interval_ms": 1
      }
    }
  ]
}
```

Relative plugin paths are resolved from the manifest directory. `auto_start`
creates and starts the first driver declared by the plugin. If no instance is
needed at startup, omit it and create the instance through `PluginManager`.
After startup, scheduled tasks can access the manager through
`Config::Instance()->plugin_manager`.

## Task Commands

`TaskProducer` accepts plugin control JSON messages. Load a plugin without
starting an extra driver:

```json
{
  "command": "plugin.load",
  "path": "/opt/runtime/plugins/libvendor_io.so",
  "pluginId": "vendor.io_plugin",
  "autoStart": false,
  "config": {}
}
```

`plugin.unload` removes a plugin after its driver instances and tasks are no
longer executing. The host automatically calls the complete hook lifecycle:

```text
RT_HOOK_INIT_SYSTEM
RT_HOOK_INIT
RT_HOOK_INIT2
RT_HOOK_INIT3
RT_HOOK_INIT_TASKS
RT_HOOK_INIT_COMM
...
RT_HOOK_EXIT_COMM
RT_HOOK_EXIT_TASKS
RT_HOOK_EXIT3
RT_HOOK_EXIT2
RT_HOOK_EXIT
RT_HOOK_EXIT_SYSTEM
```

## Native Consumer Tasks

Execution logic belongs to the native RuntimeCore consumers:

```text
IecTask                 -> IEC data calculation
FileIOConsumerTask      -> IO channel transform and checksum
NetworkIOConsumerTask   -> network packet framing and checksum
```

Each task reads its input shared-memory triple buffer, executes the native
algorithm, writes the output triple buffer, and publishes it.

Execution algorithms are selected through `TaskCallbackRegistry`. The task
message can choose a registered callback with `executor`:

```json
{
  "type": "iectask",
  "label": "native-iec-task",
  "intervalMs": 1,
  "priority": 20,
  "executor": "custom"
}
```

Register callbacks before installing tasks:

```cpp
TaskCallbackRegistry::Instance().Register(
    Config::TaskType::IECTASK,
    "custom",
    MyIecExecutionCallback);
```

Callbacks can also be registered dynamically from a loadable component. A
plugin declares `RtTaskCallbackDescV1` entries in `RtPluginV1.callbacks`.
When the plugin is loaded, RuntimeCore registers all entries by task kind and
executor name. Unloading removes those callbacks before `dlclose`.

```c
static int MyIoCallback(const RtTaskContextV1 *context) {
    /* read context->input_data, write context->output_data */
    return 0;
}

static const RtTaskCallbackDescV1 kMyCallback = {
    RT_PLUGIN_ABI_V1,
    sizeof(RtTaskCallbackDescV1),
    "my.io.callback",
    2,                       /* RT task kind: IO */
    MyIoCallback,
};
```

The task selects it with `"executor":"my.io.callback"`. The example plugin
registers `example.iec`, `example.io`, and `example.network`.

All callbacks use `RuntimeTaskContext`, which contains task type, sequence,
configuration, and zero-copy input/output shared-memory pointers. The default
names and implementations are `default` IEC, IO, and network callbacks.

## Standard Event Flow

Native task callbacks and plugin `HookFunction` callbacks use the same
`RtHookIdV1` event identifiers:

```text
RT_HOOK_INIT_TASKS   task callback initialized
RT_HOOK_INIT_COMM    task callback started
RT_HOOK_COMM_CYCLE   cyclic task execution
RT_HOOK_EXIT_COMM    task stopped
RT_HOOK_EXIT_TASKS   task callback destroyed
```

`RuntimeTaskContext.event_id` identifies the current event. Input/output
three-buffer pointers are valid for `RT_HOOK_COMM_CYCLE`. The output buffer is
published only after the cyclic callback succeeds.

Example IEC task:

```json
{
  "type": "iectask",
  "label": "native-iec-task",
  "intervalMs": 1,
  "priority": 20,
  "config": {
    "mode": "native"
  }
}
```

The same message format works with:

```json
{"type": "io"}
{"type": "network"}
{"type": "iectask"}
```

`TaskProducer` installs the requested native consumer task directly. The
sender example is
`examples/plugin_task_sender.cpp`.

## Shared-Memory Triple Buffers

Every IEC, file-I/O, and network task owns two SPSC shared-memory channels:

```text
runtime.tb.<task-id>.in.<hash>   producer -> task
runtime.tb.<task-id>.out.<hash>  task -> consumer
```

Both channels use the classic EtherCAT-style three-buffer layout. Each channel
contains three equally sized slots:

```text
writer slot | published/middle slot | reader slot
```

The writer always writes its private slot. Publishing atomically swaps the
writer slot with the published slot. Since the reader owns a third slot, the
writer never overwrites memory that is currently being read.

The default input and output size is 4096 bytes per slot and can be overridden
in the task message:

```json
{
  "type": "io",
  "label": "axis-io-1",
  "intervalMs": 1,
  "priority": 10,
  "inputBytes": 8192,
  "outputBytes": 8192,
  "config": {}
}
```

The native task acquires zero-copy input/output pointers from its
`TaskBuffers`, writes its result to the private output slot, and publishes the
output after the algorithm completes.

The shared-memory names are logged when each task is installed. One writer and
one reader are supported per channel, matching the three-buffer ownership
model.

## Multiple Plugins

The runtime installs one global hook-cycle task by default. On each cycle it
iterates every loaded plugin and calls that plugin's `RT_HOOK_COMM_CYCLE`
exactly once. Plugins loaded later through `plugin.load` are picked up
automatically because the task takes a fresh registry snapshot every cycle.

The global cycle defaults to 1 ms and can be configured with:

```sh
RUNTIME_PLUGIN_HOOK_INTERVAL_MS=1
RUNTIME_PLUGIN_HOOK_PRIORITY=0
```

Set `RUNTIME_PLUGIN_HOOK_INTERVAL_MS=0` to disable the global hook-cycle task.
The example `plugins.json` contains both
`runtime.example_plugin` and `runtime.example_counter`, allowing
multi-plugin dispatch to be tested directly.

## Lifecycle

```text
dlopen(RTLD_NOW | RTLD_LOCAL)
  -> dlsym(runtime_plugin_query_v1)
  -> validate ABI and descriptor sizes
  -> plugin init(host_api)
  -> driver create(config)
  -> driver configure(config)
  -> driver start()
  -> read_inputs() / write_outputs()
  -> driver stop()
  -> driver destroy()
  -> plugin shutdown()
  -> dlclose()
```

The plugin compiler, architecture, C library, C++ standard library, and
compiler ABI must match the runtime. Load plugins during startup or on a Linux
maintenance thread. Do not call `dlopen`, `dlsym`, `dlclose`, or allocate memory
from a Xenomai real-time loop.

`DestroyDriver` and `UnloadPlugin` are maintenance operations. Wait until the
scheduler has quiesced all I/O callbacks before destroying a handle or
unloading its plugin.

## Example

Build the example plugin:

```sh
cmake --build <build-dir> --target runtime_example_io_plugin
```

Run the runtime with the generated manifest:

```sh
./runtime --plugins ../plugins.json
```

Run the standalone load/unload smoke test:

```sh
ctest --test-dir <build-dir> -R runtime_plugin_smoke --output-on-failure
```
