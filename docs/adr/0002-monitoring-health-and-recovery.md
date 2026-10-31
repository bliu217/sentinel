# Preserve monitoring evidence through bounded recovery

Sentinel must explain its own collection and persistence failures to both GUI and MCP callers because missing telemetry can coincide with the incidents being investigated. Individual collection failures allow monitoring to continue; persistence failures pause collection and trigger bounded recreation of the monitoring engine and database connection while the owner remains reachable. This retains diagnostics and explicit data gaps in exchange for additional health and recovery behavior in the first shared-interface slice.

## Health and diagnostics

Report system and process collection health separately: last successful timestamps, consecutive failures, initial baseline availability, failing operations, error codes when available, and process enumeration/read/skip counts. Summarize expected process-access failures rather than flooding diagnostics. Both clients receive structured diagnostics, recovery progress, and bounded recent diagnostic history; a rotating diagnostic log is independent of SQLite. Logging failure must be visible when possible and cannot be assumed away by telemetry persistence.

Health explicitly distinguishes startup without a sample, normal operation with per-source availability, recovery, and terminal failure. Preserve previous failure details after successful recovery. The last successfully published snapshot remains available with its original timestamps and generation; current health does not imply that snapshot is fresh.

## Persistence outcomes

A thrown write currently does not prove that a tick was not committed: SQLiteStore::writeTick commits before enforceCap, which can itself fail. EventStore retains pending anomalies when writeTick throws, and existing generated row identifiers do not prevent duplicate replay. Before enabling retries, distinguish commit failure from post-commit maintenance failure and provide stable tick identity so retrying a tick cannot duplicate its samples or anomalies. Report maintenance failures separately even when a tick committed successfully.

## Recovery lifecycle

After a persistence failure, report Recovering and pause collection. Allow three automatic recovery attempts after delays of 1, 2, and 4 seconds. An attempt succeeds only after saving a new tick; replenishment of the retry budget requires 60 seconds of successful persistence, so repeated reopen/write failure cannot cause unlimited restarts. Once the budget is exhausted, remain reachable in Failed until the owner is explicitly restarted.

Each engine recreation begins a new monitoring generation, re-establishes collection baselines, and resets detection state. Record the recovery gap and interrupted anomaly episode continuity in diagnostics. Do not fabricate a stopped event for an episode whose ending was not observed. An owner-instance identifier distinguishes restarting the entire owner process from recreating its engine.
