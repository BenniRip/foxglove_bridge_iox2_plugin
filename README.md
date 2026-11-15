# foxglove_bridge_iox2_plugin

A [foxglove_bridge](https://github.com/foxglove/foxglove-sdk/tree/main/ros/src/foxglove_bridge) channel source plugin that serves [iceoryx2](https://github.com/eclipse-iceoryx/iceoryx2) FlatBuffer services as Foxglove channels, next to the ROS topics. The Foxglove app can't merge several live connections, so this shows iceoryx2 and ROS data in one.

## How it works

- **Discovery.** Every 200 ms, each configured service is checked. Its channel is advertised while the service has a publisher and removed when the last one leaves.
- **Never creates services.** It only opens existing ones, so publishers own the service configuration.
- **Subscribes only while watched.** An iceoryx2 subscriber exists only while a Foxglove client subscribes to the channel.
- **Forwards bytes verbatim.** No generated code for your schemas is needed. Log time is the receive time on the bridge node's clock, so it follows `use_sim_time`.
- **Schemas must match.** Each `.bfbs` file is both the channel schema and the schema checked against the publisher's.
- **Protects blocking publishers.** Services without safe overflow are skipped unless `iox2.allow_blocking_publishers` is set.

## Requirements

- **foxglove_bridge with the channel source plugin API**, until released upstream from [BenniRip/foxglove-sdk#3](https://github.com/BenniRip/foxglove-sdk/pull/3). The plugin only loads into the bridge version it was built against (3.6.3).
- **iceoryx2 v0.10.0** built with `IOX2_FEATURE_FLATBUFFERS=ON`, from the environment rather than rosdep. Add its prefix to `CMAKE_PREFIX_PATH`. It is linked statically; rebuild the plugin when iceoryx2 changes.
- **The publishers' iceoryx2 environment:** same user, `/dev/shm`, iceoryx2 root directory (default `/tmp/iceoryx2`) and config (`iox2.config_file`). In containers, share the IPC namespace and mount the root directory.
- **Byte-identical `.bfbs` files** to the ones the publishers register. A different `flatc` version or flags produce different bytes.

## Building

In a ROS 2 workspace with the bridge installed:

```bash
cd ~/ws && CMAKE_PREFIX_PATH=/opt/iceoryx2:$CMAKE_PREFIX_PATH colcon build --packages-select foxglove_bridge_iox2_plugin
```

As .deb packages of the plugin and the bridge it was built against, into `dist/`:

```bash
docker bake deb
```

CI uploads both as the `debs-<distro>` artifact.

## Running

Load an instance, here named `iox2`, through the bridge's parameters (see `config/iox2_services.example.yaml`):

```yaml
/**:
  ros__parameters:
    channel_source_plugins: ["iox2"]
    iox2:
      plugin: "foxglove_bridge_iox2_plugin::Iox2Source"
      schema_dir: "/path/to/bfbs"
      service_names: ["/lidar/points"]
      topics: ["/lidar/points"]
      schema_names: ["foxglove.PointCloud"]
      bfbs_files: ["PointCloud.bfbs"]
```

```bash
ros2 run foxglove_bridge foxglove_bridge --ros-args --params-file config/iox2_services.example.yaml
```

Several instances can run side by side under different names.

### Parameters

| Parameter | Type | Default | Description |
| :-- | :-- | :-- | :-- |
| `iox2.config_file` | string | `""` | iceoryx2 config shared with the publishers; empty uses the default |
| `iox2.schema_dir` | string | `""` | Directory of the `.bfbs` files |
| `iox2.service_names` | string[] | `[]` | Services with `iox2::Flatbuffer<T>` payloads |
| `iox2.topics` | string[] | `[]` | Foxglove topic per service |
| `iox2.schema_names` | string[] | `[]` | Fully qualified FlatBuffer root table per service, e.g. `foxglove.PointCloud` |
| `iox2.bfbs_files` | string[] | `[]` | `.bfbs` file per service, relative to `schema_dir` |
| `iox2.poll_interval_ms` | int | `10` | How often subscribed services are drained (1–1000) |
| `iox2.allow_blocking_publishers` | bool | `false` | Also bridge services without safe overflow |
| `iox2.problem_repeat_interval_s` | int | `30` | How often a persisting warning or error is repeated (1–3600) |

Index `i` of the four lists describes one service. Invalid configuration stops the plugin from loading; the bridge logs why and keeps serving ROS data.

### Logging

The plugin logs as `<bridge node>.<instance name>`, e.g. `foxglove_bridge.iox2`, so its level can be set separately (`--log-level foxglove_bridge.iox2:=info`).

- **DEBUG:** configured services that don't exist yet or have no publisher.
- **INFO:** channels and iceoryx2 subscribers created and removed.
- **WARN/ERROR:** services that cannot be bridged, e.g. on a schema mismatch. Repeated every `problem_repeat_interval_s`, since they usually occur at startup, before anyone watches.

## Known iceoryx2 issue

In iceoryx2 v0.10.0, opening a FlatBuffer service with a mismatching schema deletes its registered schema, and every later `open()` fails until the service is recreated. The plugin therefore reads the registered schema from iceoryx2's storage and compares it first. If that storage layout cannot be read, the service is not opened.

## Code layout

Headers are in `include/foxglove_bridge_iox2_plugin/`, sources in `src/`:

- `iox2_source`: the plugin.
- `iox2_source_settings`: its parameters.
- `iox2_node`: loads the iceoryx2 config and creates the node.
- `iox2_service_storage`: the workaround above.
- `plugin_support/`: no iceoryx2 code; a starting point for other channel source plugins.

The plugin is C++23. Its interface to the C++17 bridge uses only C++17 types.

## Development

```bash
docker bake                # lint, unit tests, smoke test and .deb packages into dist/
docker bake test           # unit tests
docker bake smoke          # the installed bridge loads the installed plugin
docker bake lint           # clang-format check
docker bake dev            # the dev image
```

The devcontainer (`.devcontainer/`) uses the same dev image. Build and test in `/ws` with `colcon build` and `colcon test`; `.clangd` points clangd at the build.

## License

MIT; see `LICENSE`.
