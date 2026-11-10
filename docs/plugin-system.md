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

Install a periodic task that loads the plugin when necessary, creates a driver,
starts it, and invokes its I/O interface from the enkiTS task execution path:

```json
{
  "command": "plugin.io_task",
  "label": "axis-io-1",
  "intervalMs": 1,
  "priority": 20,
  "pluginId": "runtime.example_plugin",
  "pluginPath": "/opt/runtime/plugins/libruntime_example_io_plugin.so",
  "driverType": "runtime.example_io",
  "config": {
    "poll_interval_ms": 1
  },
  "readBytes": 4,
  "writeBytes": 4,
  "parameterId": 1,
  "parameterBytes": 4
}
```

Each execution calls `write_outputs`, `read_inputs`, and optionally
`read_parameter` through the `DriverHandle` bound to that task. Omit
`readBytes`, `writeBytes`, or parameter fields to skip the corresponding
operation. The sender example is `examples/plugin_task_sender.cpp`.

For CODESYS-style components whose functionality is implemented mainly in the
hook function, install a hook task:

```json
{
  "command": "plugin.hook_task",
  "label": "plugin-hook-1",
  "intervalMs": 1,
  "priority": 20,
  "pluginId": "runtime.example_plugin",
  "pluginPath": "/opt/runtime/plugins/libruntime_example_io_plugin.so",
  "config": {}
}
```

`plugin.hook_task` calls `RT_HOOK_COMM_CYCLE` through the plugin's hook function
on every task execution. The host automatically calls:

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

The hook task sender example is `examples/plugin_hook_sender.cpp`.

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

Set `RUNTIME_PLUGIN_HOOK_INTERVAL_MS=0` to disable the global hook-cycle task,
for example when every plugin uses an explicit `plugin.hook_task` with a
different interval. The example `plugins.json` contains both
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
