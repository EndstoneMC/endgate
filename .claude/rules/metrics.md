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
| `org.bstats.json.JsonObjectBuilder`, `JsonObject` | `JsonObjectBuilder`, `JsonObjectBuilder::JsonObject` | `bstats/json.h` |
| `org.bstats.charts.CustomChart` and its seven subclasses | the same names | `bstats/charts.h` |

Method names and semantics are bstats-base's: `addCustomChart`, `shutdown`, `getServerUUID`, `isEnabled`, `isLogErrorsEnabled`, `isLogSentDataEnabled`, `isLogResponseStatusTextEnabled`, `didExistBefore`, `appendField`, `appendNull`, `build`, `getRequestJsonObject`, `getChartData`. The constructor takes `MetricsBase`'s parameters in the same order.

What it does, unchanged:

- The request is `POST https://bStats.org/api/v2/data/<platform>`, gzip-compressed JSON, with `Accept: application/json`, `Connection: close`, `Content-Encoding: gzip`, `Content-Length`, `Content-Type: application/json` and `User-Agent: Metrics-Service/1`.
- The payload is the platform data, then `service` (the service data, `id`, `customCharts`), `serverUUID` and `metricsVersion` (`3.0.3`), in that order. Each chart is `{"chartId", "data"}`; a chart whose value is null, empty, zero or all-skipped is left out, and one that throws is left out and logged when `log-errors` is on.
- The schedule is fixed: one submission after 3 to 6 minutes at random, then one every 30 minutes starting that delay plus 0 to 30 minutes at random. Nothing else may change it.
- `config.txt` is `key=value` lines under five `#` comment lines: `enabled`, `server-uuid`, `log-errors`, `log-sent-data`, `log-response-status-text`. A missing file is written with a fresh UUID, `enabled` set to the default the caller gave, and the rest `false`. An `enabled` that is absent reads as `true` and one that is not exactly `true` as `false`; the other flags default `false`; a file with no `server-uuid` is written again. `didExistBefore` is false when the file had to be created.
- `JsonObjectBuilder` escapes `"`, `\` and every control character below `0x20` as `\u00XX` with lower-case hex, and nothing else. It is the builder itself, not glaze, so the payload is byte for byte bstats-base's.
- Errors are logged only when `logErrors` is on; the sent data only when `logSentData` is on; the response body, with its line breaks removed, only when `logResponseStatusText` is on. A non-2xx answer is an error, as `HttpURLConnection::getInputStream` throws on one.

Deviations from bstats-base:

- `ScheduledThreadPoolExecutor` (the `bStats-Metrics` thread) becomes an `EventLoop` the caller passes in through the `HttpClient`, scheduled with `schedule` (`threading.md`). `scheduleAtFixedRate` is a `schedule` that arms the next run first. The send is a coroutine on that loop, so nothing blocks it. The thread is not a daemon, so `shutdown` must run before the loop does.
- `HttpsURLConnection` becomes endgate's `HttpClient`, which trusts the system's store. It adds `Host` and `Connection: close` and sets `Content-Length` from the body itself, so `MetricsBase` sends only the other four headers. `HttpRequest` gains a `method` and a `body` for it.
- `GZIPOutputStream` becomes libdeflate's gzip at level 6, Java's default.
- `Consumer`, `Supplier`, `BiConsumer` and `Callable` become `std::function`. A chart's callable returns `std::optional` where Java returns null and `std::map` where it returns a `Map`. The error logger takes the message and the `std::exception`. `Throwable` becomes `std::exception`. `IOException` becomes `std::ios_base::failure`.
- `getRequestJsonObject` and `getChartData` return `std::optional<JsonObject>` for Java's null. `getChartData` is protected in every chart; Java's `DrilldownPie` widens it to public by accident.
- `customCharts` is a `std::vector<std::unique_ptr<CustomChart>>` behind a mutex, in insertion order, where Java has a `HashSet` and no lock. `addCustomChart` takes ownership.
- `skipRelocateCheck` is dropped: it guards against a shaded Java package and has no counterpart in C++. Its place is taken by `Overrides`, the last constructor parameter, which holds the report URL and the first delay and which only a test sets.
- `MetricsBase` may be destroyed while a scheduled run is queued: `shutdown` runs a barrier task on the loop that drops the liveness token every queued run checks first. It blocks, so it is called from a thread that is not an event loop. `~MetricsBase` calls it.
- `JsonObjectBuilder::appendField` takes `std::string_view`, `int`, `JsonObject`, `std::span<const std::string>`, `std::span<const int>` and `std::span<const JsonObject>`; `build` on a built builder throws `std::logic_error` for `IllegalStateException`. The null checks are gone, since none of those types is null.

## Velocity's Metrics

`com.velocitypowered.proxy.Metrics` is `Metrics` in `src/endgate/metrics.h`, with its nested `VelocityMetrics` as `Metrics::EndgateMetrics`, the way `VelocityCommand` is `EndgateCommand`.

- The constructor reads `bStats/config.txt` through `MetricsConfig`, builds the `MetricsBase` with the platform `server-implementation` and `appendPlatformData` (`osName`, `osArch`, `osVersion`, `coreCount`), and logs the first-run notice when `didExistBefore` is false. A config that cannot be written logs `Failed to create bStats config` at error and leaves the metrics off.
- `EndgateMetrics::startMetrics(server, metricsConfig)` adds the charts and `ProxyServer::start` calls it where `VelocityServer` does, once the configuration is read and the servers are registered.
- The charts are Velocity's: `players` (`SingleLineChart`, `getPlayerCount`), `managed_servers` (`SingleLineChart`, `getAllServers().size()`), `online_mode` (`SimplePie`, `online` or `offline`), `endgate_version` (`SimplePie`, for `velocity_version`) and `operating_system` (`DrilldownPie`, for `java_version`, the OS name over its version: `{"Windows": {"Windows 11 (10.0.26200)": 1}}`).
- The toggle is Velocity's: a `[metrics]` table with `enabled`, read through `ProxyConfig::isMetricsEnabled`, which only seeds `enabled` in a new `config.txt` as `defaultEnabled`. Velocity's `default-velocity.toml` doesn't list the table and neither does `endgate.default.toml`.
- `config.txt` lives in `bStats/` next to `endgate.toml`, where Velocity's lives in `plugins/bStats/`. Endgate has no plugin folder, so no `plugins/` is created for it. The first-run notice names `the 'bStats' folder` for `the '/plugins/bStats/' folder`, and says Endgate for Velocity.
- Velocity checks that bstats-base was not left unrelocated and logs `debug environment, metrics is disabled!` otherwise; there is no such thing in C++, so the check is gone.
- Velocity never stops its metrics, because the scheduler's thread is a daemon. Endgate's loop is not, so `ProxyServer::shutdown` calls `Metrics::shutdown` first, before the loops stop.

## Bedrock and endgate adaptations

- Endgate has its own bStats service on the `server-implementation` platform, as Velocity has service 4752. Its id is `EndgateMetrics::UNREGISTERED_SERVICE_ID` (0) until it is registered, and `startMetrics` starts nothing while it is that: no config file, no notice, no request.
- The charts that must exist on the service, with these ids and types: `players` and `managed_servers` (Single Line Chart), `online_mode` and `endgate_version` (Simple Pie), `operating_system` (Drilldown Pie).
- The platform data is Java's, so bStats' own OS charts line up with Velocity's: `osArch` is `amd64` or `aarch64` (what `os.arch` reports), `osName` is `Windows 10`, `Windows 11` or `Linux`, `osVersion` is the NT version (`10.0`) or the kernel release, and `coreCount` is `std::thread::hardware_concurrency`.
- `ENDGATE_BSTATS_URI`, like `ENDGATE_DISCOVERY_URI`, replaces the report URL and the first delay for the integration test, which points it at a fake endpoint. It also lets an unregistered service id send, since the fake is not bStats. It is not a user setting and `endgate.toml` has no key for it.
- The player count reads the same registry `glist` does; a Bedrock player is a Bedrock connection and counts once.
- The integration test reads the request the fake endpoint received, so a change to the payload changes `tests/integration/test_metrics.py` in the same commit.
