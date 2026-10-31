# Sentinel architecture and implementation plan

## Component ownership

Sentinel stays in one repository. The C++ engine owns collection, process
grouping, anomaly detection, attribution, persistence, and query semantics.
The CLI, desktop dashboard, and MCP adapter are clients of that engine.

Implemented locations:

- `engine/include/sentinel/`: public C++ types and interfaces.
- `engine/src/`: implementations, including `runtime/monitor_runtime.cpp`.
- `engine/tests/`: engine tests and runtime integration tests.
- `apps/cli/`: command parsing and console shutdown handling.

Planned locations, created when their implementations begin:

- `engine/src/query/`: current status, history, process activity, and event queries.
- `engine/src/transport/`: the engine's local communication endpoint.
- `apps/desktop/host/`: native window, WebView2, and communication bridge.
- `apps/desktop/ui/`: React and TypeScript presentation.
- `integrations/mcp/`: local MCP tools translating agent calls into engine queries.
- `packages/protocol/`: versioned external request and response schemas.
- `packages/client-ts/`: shared client used by TypeScript integrations.
- `tests/integration/`: checks crossing application and engine boundaries.

The engine has no GUI or MCP dependency. The GUI and MCP adapter must not
independently interpret the SQLite schema or implement anomaly logic.
The desktop host owns its WebView2 dependency. Internal C++ snapshot types
are not yet an external wire format.

## Current monitoring runtime

`MonitorRuntime` owns one monitoring session and one worker thread. Construction
validates configuration. `start()` is one-shot; startup and collection run on
the worker. `status()` exposes Idle, Starting, Running, Stopping, Stopped, or
Failed. `requestStop()` interrupts the sampling wait, while `wait()` joins and
rethrows any startup or worker error. Destruction requests stop and joins.
Shutdown waits for any collection or storage operation already in progress.

The worker acquires a Windows file handle with sharing disabled at
`<database-path>.recording.lock`. This prevents cooperating Sentinel runtimes
from recording to the same database concurrently, including across processes.
The lock file may remain after shutdown; ownership comes from the open handle,
not from the file's existence. Archive commands do not acquire this recorder lock.

Each tick collects system and process readings, groups and selects processes,
detects anomalies, and attaches context from the same pass. Available readings
and events are persisted in one transaction before that tick is published.
A storage error fails the runtime; failed ticks are not published as successful
live samples. A pass with neither reading is published with both fields absent
and `persisted=false`, and is not written to SQLite.

Live history is a bounded deque of `shared_ptr<const TickSnapshot>`. Readers
receive immutable snapshots containing the sequence, capture times, readings,
events, and persistence flag. Retained history is bounded by configuration;
callers holding old shared pointers extend those snapshots' lifetimes. Sequence
numbers are local to a runtime session and are not database IDs or reconnect
cursors. Separate system/process history classes remain available to existing
library users, but the runtime publishes combined ticks.

Health currently includes completed ticks, unavailable system/process counts,
skipped deadlines, last tick duration, last persisted tick time, and the terminal
error. The collector's empty result does not distinguish initial CPU baseline
setup from a failed Win32 read. A process snapshot can also omit inaccessible
processes without reporting coverage. These limitations must remain explicit
until richer collection outcomes are implemented.

Sampling uses a steady clock. Missed deadlines are skipped rather than replayed
as immediate catch-up ticks. Existing CPU baseline and detector gap behavior is
otherwise unchanged. Runtime callbacks permit deterministic testing without
depending on the host's CPU load.

## Implementation order

1. **Complete: repository separation.** Engine library, CLI application, public
   header namespace, and engine tests have separate locations and CMake files.
   The `sentinel_telemetry` alias and executable output locations are preserved.
2. **Complete: runtime foundation.** Owned lifecycle, interruptible waits,
   recorder lock, consistent tick snapshots, initial health, failure propagation,
   and skipped-deadline scheduling are implemented.
3. **Before the shared query interface: finish engine readiness.**
   - Distinguish warming up, failed reads, partial process coverage, and stale data.
   - Define detector confirmation and CPU baseline handling across collection
     failures, sleep, and long gaps.
   - Introduce recording-session boundaries and anomaly episode linkage so a
     restart does not imply continuous observation or an uninterrupted episode.
   - Define database reader ownership and bounded concurrent reads. Evaluate
     journaling and total disk-cap accounting together; SQLite currently uses
     rollback journaling and synchronous writes.
   - Add periodic age-based retention; currently it runs at startup. Verify
     collection and persistence overhead under sustained operation before adding
     asynchronous storage or other complexity.
4. **Next: shared query layer and local transport.** Add bounded current,
   recent/history, event, and process queries. Define units, UTC timestamps,
   missing-data semantics, schema versions, errors, pagination, and history-to-live
   handoff/reconnect behavior. Keep these semantics shared across all clients.
5. **Then: desktop and MCP clients.** Implement one real vertical path in each.
   The GUI loads history and follows live samples; the agent requests focused,
   bounded evidence on demand. Both continue to work independently of each other.

Additional disk, network, GPU, and Linux-process metrics, a Windows service
installer, automatic archive rules, and richer investigation summaries can
follow. They are not prerequisites for the first query interface.

## Verification

The repository separation passed all 53 existing tests in a fresh x64 MSVC build.
The runtime foundation adds eight tests; all 61 pass. Runtime tests cover
consistent bounded snapshots, persistence and event context, interruptible stop,
duplicate recorder rejection and lock release, missing readings, startup and
worker failure, skipped deadlines, and destruction cleanup.

See `engine/tests/README.md` for the standard build and test commands.
