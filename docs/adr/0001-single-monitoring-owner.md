# One background process owns monitoring

The desktop GUI and MCP adapter must observe the same monitoring history without starting duplicate collectors. One background Sentinel process per Windows user will own collection and persistence, continuing when the GUI closes or an MCP client disconnects; both applications will act as clients. This introduces communication between processes and separate lifecycle management in exchange for continuous monitoring independent of viewers.

## Interface and ownership

The owner uses a reusable C++ monitoring module extracted from the CLI orchestration. A shared C++ client library communicates with the owner over a local Windows named pipe restricted to the same user. The WebView2 native host uses that library and bridges plain serializable data to React/TypeScript; the MCP adapter uses the same client library. The first slice is explicitly started with `sentinel start`; a second owner reports already running. Ctrl+C stops the owner cleanly. Clients neither launch nor stop the owner in this slice.

Reads return independent, consistent snapshots of one published sampling pass, after persistence succeeds. Missing process data is explicit rather than replaced with older process data. Timestamps, source availability, a snapshot sequence number, owner-instance identity, and monitoring generation distinguish startup, repeated data, stale data, and restarts. Current health remains readable independently of the last published telemetry snapshot, including during recovery or terminal failure. Connection failures are distinct from monitoring failures.

React polls asynchronously through the native host once per second, keeping communication off the UI thread. Push notifications, automatic startup, Windows service installation, cross-user access, historical queries, archives, and integration into the real GUI and MCP adapter are subsequent work.

## First slice

Use `sentinel start` as the monitoring owner and add `sentinel status` as a separate client of the shared C++ interface. The slice includes latest system and selected process-group telemetry, current health, simultaneous clients, clean owner shutdown, and the recovery contract in ADR 0002. It proves both the reusable monitoring module and the process-to-process client interface before GUI and MCP integration.
