## Problem Statement

Sentinel's telemetry engine is mostly implemented, but monitoring orchestration belongs to the CLI. A desktop GUI and MCP adapter need a shared way to inspect the same continuously collected telemetry without starting duplicate collectors, coordinating storage themselves, or hiding evidence when monitoring fails. The GUI runs in React/TypeScript inside a WebView2 host. An investigating agent needs to distinguish an idle machine from unavailable, stale, partially collected, or interrupted telemetry.

## Solution

Provide one explicitly started monitoring owner per Windows user and a shared typed C++ client interface for reading latest saved telemetry and current monitoring health. The owner continues independently of connected clients. The WebView2 native host and MCP adapter will consume the same client library; the host will bridge serializable results to React/TypeScript.

Prove the first slice using sentinel start as the owner and sentinel status as a separate client over a user-restricted local named pipe. Include concurrent reads, clean shutdown, detailed diagnostics, safe persistence retries, and bounded engine recovery before connecting the real GUI or MCP adapter.

## User Stories

1. As a Sentinel user, I want to start one monitoring owner explicitly, so that collection has a predictable lifecycle.
2. As a Sentinel user, I want a second owner to report already running, so that telemetry is not collected twice.
3. As a Sentinel user, I want monitoring to continue after a viewer closes, so that incidents are still recorded.
4. As a CLI user, I want sentinel status to connect to the running owner, so that I can verify the shared interface before GUI integration.
5. As a GUI developer, I want the native WebView2 host to use a shared C++ client, so that React does not implement Windows communication.
6. As an MCP developer, I want to reuse that client, so that agents and the GUI observe the same monitoring owner.
7. As a Windows user, I want owner access restricted to my user, so that local monitoring is not exposed across user accounts.
8. As a telemetry reader, I want latest system and selected process-group telemetry, so that I can inspect current resource pressure.
9. As a telemetry reader, I want data from one sampling pass, so that unrelated source timestamps are not silently combined.
10. As a telemetry reader, I want missing sources to be explicit, so that unavailable process data is not mistaken for zero activity.
11. As a telemetry reader, I want an explicit no-sample-yet result, so that startup is distinguishable from failure.
12. As a telemetry reader, I want only successfully saved telemetry published, so that displayed data has a persistence guarantee.
13. As a concurrent client, I want an independent consistent snapshot, so that another client or collection cannot mutate my result.
14. As a GUI developer, I want sequence numbers and timestamps, so that repeated or stale snapshots can be identified.
15. As an investigating agent, I want owner-instance identity and monitoring generation, so that process restarts and engine recreations are distinguishable.
16. As a GUI developer, I want asynchronous polling support, so that once-per-second reads do not block the UI thread.
17. As a client, I want connection errors distinguished from monitoring errors, so that I can explain whether Sentinel is unreachable or reporting failure.
18. As an investigating agent, I want separate system and process collection health, so that partial failures remain visible.
19. As an investigating agent, I want each source's last success and consecutive failure count, so that I can assess the duration of missing evidence.
20. As an investigating agent, I want initial CPU baseline collection distinguished from failure, so that expected warmup is not misdiagnosed.
21. As an investigating agent, I want failing operations, available error codes, and timestamps, so that failures are actionable.
22. As an investigating agent, I want process enumeration, read, and skip counts, so that partial process visibility is apparent.
23. As a client, I want expected process-access failures summarized, so that routine exclusions do not overwhelm diagnostic signals.
24. As a Sentinel user, I want collection to continue when an individual source fails, so that other evidence is retained.
25. As an investigating agent, I want bounded recent diagnostic history and a rotating log independent of SQLite, so that persistence failures do not erase their own explanation.
26. As a client, I want current health available during recovery, so that a stale telemetry snapshot does not imply a healthy engine.
27. As a Sentinel user, I want transient persistence failures to trigger bounded automatic engine recovery, so that manual intervention is not always required.
28. As a client, I want recovery attempts and exhaustion reported, so that repeated restarts are visible.
29. As a Sentinel user, I want retries to avoid duplicate samples and anomaly events, so that my investigation history remains trustworthy.
30. As an investigating agent, I want committed writes distinguished from subsequent maintenance failures, so that saved evidence is not incorrectly reported as lost.
31. As an investigating agent, I want recovery gaps and interrupted anomaly continuity identified, so that separate monitoring generations are not interpreted as continuous observation.
32. As a client, I want previous failure details retained after recovery, so that transient incidents remain explainable.
33. As a client, I want the owner reachable after retry exhaustion, so that terminal failure remains inspectable.
34. As a Sentinel user, I want Ctrl+C to stop the owner cleanly, so that shutdown releases monitoring and communication resources.

## Implementation Decisions

- Extract monitoring orchestration into a reusable C++ monitoring module. Retain the existing collectors, aggregation, detection, attribution, storage, configuration, and retention behavior except where the agreed health and recovery contract requires changes.
- Keep a single monitoring owner per Windows user, independent of GUI and MCP lifetimes. Explicit startup and Ctrl+C shutdown belong to the owner; clients neither launch nor stop it in this slice.
- Add a shared C++ client module using a local Windows named pipe restricted to the same user. The eventual WebView2 bridge and MCP adapter consume this module.
- Offer a typed read contract for latest published telemetry, current health, and bounded recent diagnostics. Results must be serializable for the eventual TypeScript bridge without leaking native handles or implementation-owned references.
- Publish system and selected process-group telemetry from the same sampling pass only after persistence succeeds. Represent missing sources explicitly; do not substitute values from older passes.
- Identify snapshots using sequence numbers, source timestamps, monitoring generation, and owner-instance identity. Health can change without publishing a new telemetry snapshot.
- Return independent consistent snapshots to simultaneous readers. Collection and storage operations must not prevent health reads throughout recovery.
- Define explicit startup/no-sample, operating, Recovering, and Failed conditions, with separate per-source availability and errors. Keep connection failure separate from a reachable owner's monitoring failure.
- Enrich collection outcomes so baseline initialization, successful collection, top-level source failure, and summarized per-process exclusions can be distinguished. Include operation, available error code, timestamps, last success, consecutive failure counts, and process coverage counts.
- Keep bounded diagnostic history and a rotating log independent of SQLite. Surface logging failure when possible; diagnostics cannot depend solely on successful telemetry writes. Keep per-pass process coverage in health, but limit routine exclusion diagnostics to one summary per minute so they do not crowd out actionable errors.
- Make persistence outcomes explicit: a committed tick and a post-commit maintenance failure are distinguishable. Provide stable tick identity and duplicate prevention covering samples and anomalies, including retry after reopening the database.
- Preserve existing databases and stored history when evolving persistence for tick identity. Reopening a connection alone is not sufficient duplicate prevention.
- Persistence failure pauses collection and starts bounded engine/database recreation while the owner remains reachable. Attempt recovery after delays of 1, 2, and 4 seconds. Saving a new tick is required for success; opening the database alone is insufficient.
- A recreated engine may take one baseline-only sampling pass before saving a new pass. If collection still produces no savable data on the next pass, or fails without baseline warmup, that recovery attempt fails and the remaining retry schedule applies. Replaying a pending pass from an earlier generation does not complete recovery.
- Replenish the recovery budget after 60 seconds of successful persistence. Repeated brief recoveries must not create unlimited restarts. Exhaustion leaves the owner reachable in Failed until explicitly restarted.
- Every engine recreation creates a new monitoring generation, re-establishes CPU baselines, and resets anomaly detection. Retain the previous published snapshot with its original identity and timestamps. Report the gap and interrupted episode continuity without fabricating stopped events.
- The eventual React consumer polls asynchronously once per second through the native host. Implement the reusable client contract now; actual GUI and MCP integration is subsequent work.
- Preserve existing CLI monitoring configuration and archive behavior while introducing the owner and status client.

## Testing Decisions

- Primary test seam: the shared C++ client read interface connected to a real monitoring owner over a real local named pipe. Assert caller-observable telemetry, health, diagnostics, errors, and lifecycle behavior. Avoid inspecting worker counts, lock placement, private state, or exact internal call sequences.
- Exercise separate-process start/status behavior, duplicate-owner rejection, client disconnect without owner shutdown, simultaneous independent reads, startup without data, partial collection, and clean owner shutdown.
- Use controlled collection, persistence failure, and elapsed-time inputs behind the monitoring module where deterministic recovery tests require them. Keep these internal to the test setup rather than creating public testing operations or separate interfaces for every helper.
- Through the primary seam, verify that recovery keeps health readable, retains stale snapshot identity, reports gaps and new generations, follows bounded retry timing, exhausts the budget, and resets the budget only after the stable-persistence interval. Use controlled time rather than real minute-long sleeps.
- Reuse the existing public SQLiteStore write/read/reopen seam for focused persistence integration tests: stable tick replay does not duplicate samples or anomalies, uncommitted ticks are not published, committed ticks remain recognizable when maintenance fails, and database evolution preserves existing history.
- Reuse existing GoogleTest and CTest infrastructure. Prior art includes temporary-database reopen and retention tests, ring-buffer concurrent snapshot/copy tests, process baseline and unavailable-CPU tests, anomaly confirmation tests, and missing-process-context attribution tests.
- Keep a small real-Windows collection smoke test alongside deterministic tests; CPU load and real machine timing must not determine the recovery test results.
- Verify bounded diagnostics and process-access summaries, connection-versus-monitoring error classification, and the named pipe's same-user access restriction. A client must receive health failures as data rather than confusing them with a broken transport.
- Final acceptance: one owner is independently readable by multiple status clients, continues after they exit, exposes latest saved telemetry plus current health, recovers safely from injected persistence failures within the agreed budget, and stops cleanly on Ctrl+C.
- The user confirmed these test seams before publication.

## Out of Scope

- Implementing the desktop GUI, its WebView2 bridge, or the actual MCP adapter.
- Historical telemetry queries, archive operations through the shared client, and new analytical summaries.
- Client-triggered owner startup, client-triggered stop/restart, automatic startup, and Windows service installation.
- Cross-user or remote access.
- Push notifications and streaming subscriptions.
- Preserving anomaly episode continuity across an engine recreation or inventing stopped events for unobserved endings.
- Unlimited restart loops, whole-process self-relaunch, and an external crash watchdog.

## Further Notes

The user approved the design after a documented interview. The project's single monitoring owner and bounded recovery ADRs provide the decision rationale; the glossary defines monitoring owner, sampling pass, and monitoring generation.

The current SQLite write path can commit before storage maintenance throws, and pending anomaly events are retained when the write call throws. Safe persistence outcomes and duplicate prevention are therefore prerequisites for automatic retries, not optional follow-up hardening.

This is the first end-to-end interface slice. It includes recovery and diagnostic behavior because callers need to trust both the telemetry and explanations for missing telemetry. Implementation tickets can split the work while retaining one coherent acceptance contract.
