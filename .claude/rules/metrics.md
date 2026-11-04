---
paths:
  - "src/endgate/bstats/**"
  - "src/endgate/metrics.*"
  - "tests/unit/bstats/**"
---

# Metrics

## bstats-base

bStats' `bstats-base` 3.0.3 (`org.bstats`), 1:1, as a small library under `src/endgate/bstats` (`endgate::bstats`), the way Brigadier is. The base is a dependency of Velocity, not part of it, so it gets no place in the proxy's own directories and keeps its own names. Velocity's `Metrics` is the only code that uses it (`src/endgate/metrics.h`).

| bstats-base | endgate | file |
| --- | --- | --- |
| `org.bstats.MetricsBase` | `MetricsBase` | `bstats/base.h` |
| `org.bstats.config.MetricsConfig` | `MetricsConfig` | `bstats/config.h` |
| `org.bstats.json.JsonObjectBuilder`, `JsonObject` | `JsonObject`, an alias of `glz::generic` | `bstats/charts.h` |
| `org.bstats.charts.CustomChart` and its seven subclasses | the same names | `bstats/charts.h` |

Method names and semantics are bstats-base's: `addCustomChart`, `shutdown`, `getServerUUID`, `isEnabled`, `isLogErrorsEnabled`, `isLogSentDataEnabled`, `isLogResponseStatusTextEnabled`, `didExistBefore`, `getRequestJsonObject`, `getChartData`. The constructor takes `MetricsBase`'s parameters in the same order, with the `HttpClient` and `Overrides` in the place of `skipRelocateCheck`.

What it does, unchanged:

- The request is `POST https://bStats.org/api/v2/data/<platform>`, gzip-compressed JSON, with `Accept: application/json`, `Connection: close`, `Content-Encoding: gzip`, `Content-Length`, `Content-Type: application/json` and `User-Agent: Metrics-Service/1`.
- The payload is the platform data, then `service` (the service data, `id`, `customCharts`), `serverUUID` and `metricsVersion` (`3.0.3`), in that order. Each chart is `{"chartId", "data"}`; a chart whose value is null, empty, zero or all-skipped is left out, and one that throws is left out and logged when `log-errors` is on.
- The schedule is fixed: one submission after 3 to 6 minutes at random, then one every 30 minutes starting that delay plus 0 to 30 minutes at random. Nothing else may change it.
- The config is five `#` comment lines and the settings `enabled`, `server-uuid`, `log-errors`, `log-sent-data`, `log-response-status-text`. A missing file is written with a fresh UUID, `enabled` set to the default the caller gave, and the rest `false`. An `enabled` that is absent reads as `true`, the other flags default `false`, and a file with no `server-uuid` is written again. `didExistBefore` is false when the file had to be created.
- The JSON is the same document bstats-base builds: the same fields in the same order, with integers written without a fraction.
- Errors are logged only when `logErrors` is on; the sent data only when `logSentData` is on; the response body, with its line breaks removed, only when `logResponseStatusText` is on. A non-2xx answer is an error, as `HttpURLConnection::getInputStream` throws on one.

Deviations from bstats-base:

- `ScheduledThreadPoolExecutor` (the `bStats-Metrics` thread) becomes an `EventLoop` the caller passes in through the `HttpClient`, scheduled with `schedule` (`threading.md`). `scheduleAtFixedRate` is a `schedule` that arms the next run first. The send is a coroutine on that loop, so nothing blocks it. The thread is not a daemon, so `shutdown` must run before the loop does.
- `HttpsURLConnection` becomes endgate's `HttpClient`, which trusts the system's store. It adds `Host` and `Connection: close` and sets `Content-Length` from the body itself, so `MetricsBase` sends only the other four headers. `HttpRequest` gains a `method` and a `body` for it.
- `GZIPOutputStream` becomes libdeflate's gzip at level 6, Java's default.
- `Consumer`, `Supplier`, `BiConsumer` and `Callable` become `std::function`. A chart's callable returns an empty string or map where Java returns null, and `std::map` where it returns a `Map`. The error logger takes the message and the `std::exception`. `Throwable` becomes `std::exception`. `IOException` becomes `std::ios_base::failure`.
- `getRequestJsonObject` and `getChartData` return `std::optional<JsonObject>` for Java's null. `getChartData` is protected in every chart; Java's `DrilldownPie` widens it to public by accident.
- `customCharts` is a `std::vector<std::unique_ptr<CustomChart>>` behind a mutex, in insertion order, where Java has a `HashSet` and no lock. `addCustomChart` takes ownership.
- `skipRelocateCheck` is dropped: it guards against a shaded Java package and has no counterpart in C++. Its place is taken by `Overrides`, the last constructor parameter, which holds the report URL and the first delay and which only a test sets.
- `MetricsConfig` reads and writes Endstone's `config.toml` with glaze's TOML instead of bstats-base's `config.txt`, for the reason that it matches Endstone's (`Velocity's Metrics` below). Its constructor takes a `std::filesystem::path` for Java's `File`.
- `MetricsBase` may be destroyed while a scheduled run is queued: `shutdown` runs a barrier task on the loop that drops the liveness token every queued run checks first. It blocks, so it is called from a thread that is not an event loop. `~MetricsBase` calls it.
- `JsonObjectBuilder` is glaze (`dependencies.md`). `JsonObject` is `glz::generic`, which keeps its fields in the order they were added, and a consumer sets a field with `object["osName"] = ...` where Java calls `appendField`. The fixed parts, a chart's request and its `value` or `values`, are structs with glaze metadata; the platform and service data, which the consumers fill, and the pie, bar and drilldown maps are generic values.
- glaze escapes a control character with its short form (`
`) where bstats-base writes `
`. The JSON is the same, and bStats reads either.

## Velocity's Metrics

`com.velocitypowered.proxy.Metrics` is `Metrics` in `src/endgate/metrics.h`, with its nested `VelocityMetrics` as `Metrics::EndgateMetrics`, the way `VelocityCommand` is `EndgateCommand`.

- The constructor reads `plugins/bstats/config.toml` through `MetricsConfig`, builds the `MetricsBase` with the platform `server-implementation` and `appendPlatformData` (`osName`, `osArch`, `osVersion`, `coreCount`), and logs the first-run notice when `didExistBefore` is false. A config that cannot be written logs `Failed to create bStats config` at error and leaves the metrics off.
- `EndgateMetrics::startMetrics(server, metricsConfig)` adds the charts and `ProxyServer::start` calls it where `VelocityServer` does, once the configuration is read and the servers are registered.
- The charts are Velocity's: `players` (`SingleLineChart`, `getPlayerCount`), `managed_servers` (`SingleLineChart`, `getAllServers().size()`), `online_mode` (`SimplePie`, `online` or `offline`), `endgate_version` (`SimplePie`, for `velocity_version`) and `operating_system` (`DrilldownPie`, for `java_version`, `os::name` over `os::version`: `{"Windows 11": {"10.0": 1}}`).
- The toggle is Velocity's: a `[metrics]` table with `enabled`, read through `ProxyConfig::isMetricsEnabled`, which only seeds `enabled` in a new config as `defaultEnabled`. Velocity's `default-velocity.toml` doesn't list the table and neither does `endgate.default.toml`.
- The config is Endstone's, not bstats-base's `config.txt`: `plugins/bstats/config.toml` next to `endgate.toml`, TOML read and written with glaze, the same five comment lines and keys as Endstone's `MetricsConfig` (`endstone/metrics/_config.py`). The reason is that it matches Endstone's, so a server owner finds the same file in both. glaze's TOML writer emits the settings but no comments, so the five comment lines are written before its output. A key it doesn't know is ignored. Endgate has no plugins, but the folder is created as Endstone creates it. The first-run notice names `the 'plugins/bstats' folder` for `the '/plugins/bStats/' folder` and `config.toml` for `config.txt`, and says Endgate for Velocity.
- Velocity checks that bstats-base was not left unrelocated and logs `debug environment, metrics is disabled!` otherwise; there is no such thing in C++, so the check is gone.
- Velocity never stops its metrics, because the scheduler's thread is a daemon. Endgate's loop is not, so `ProxyServer::shutdown` calls `Metrics::shutdown` first, before the loops stop.

## Bedrock and endgate adaptations

- Endgate has its own bStats service on the `server-implementation` platform, as Velocity has service 4752: `EndgateMetrics::SERVICE_ID` is 30353 (https://bstats.org/plugin/server-implementation/Endgate/30353).
- The charts that must exist on the service, with these ids and types: `players` and `managed_servers` (Single Line Chart), `online_mode` and `endgate_version` (Simple Pie), `operating_system` (Drilldown Pie).
- The platform data is the JDK's, so bStats' own OS charts line up with Velocity's. Platform facts live in `os.h` as free functions in `namespace os` that mimic Java's system properties and follow the JDK's own code, not an approximation: `os::name` (`os.name`), `os::version` (`os.version`) and `os::arch` (`os.arch`) port `src/java.base/windows/native/libjava/java_props_md.c` and `src/java.base/unix/native/libjava/java_props_md.c` (on Windows the name and version come from kernel32.dll's file version and the product type from `RtlGetVersion`, where the JDK uses `GetVersionEx`, which lies to a program with no manifest), and `os::active_processor_count` (`Runtime.availableProcessors`) ports HotSpot's `os::active_processor_count` in `os_windows.cpp` (job object, process group, affinity mask, with `UseAllWindowsProcessorGroups` at its default of off) and `os_linux.cpp` (`sched_getaffinity` limited by the cgroup v1 or v2 CPU quota, as `OSContainer` does with container support on). The sources are JDK 25 (`jdk-25-ga`). A new platform fact goes in `os.h` the same way.
- `ENDGATE_BSTATS_URI`, like `ENDGATE_DISCOVERY_URI`, replaces the report URL and the first delay for the integration test, which points it at a fake endpoint. It is not a user setting and `endgate.toml` has no key for it.
- The player count reads the same registry `glist` does; a Bedrock player is a Bedrock connection and counts once.
- The integration harness writes `[metrics] enabled = false` unless a test sets the table or `ENDGATE_BSTATS_URI`, so no test proxy writes a `plugins` folder or reaches bStats.
- The integration test reads the request the fake endpoint received, so a change to the payload changes `tests/integration/test_metrics.py` in the same commit.
