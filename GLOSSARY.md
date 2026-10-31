# Sentinel

Sentinel records Windows telemetry so resource pressure and incidents can be investigated afterward.

## Language

**Monitoring owner**:
The single running Sentinel instance responsible for collecting and retaining telemetry for a Windows user, independently of connected viewers.
_Avoid_: GUI-owned monitoring, per-client collector

**Sampling pass**:
One attempt to collect system telemetry and process-group telemetry together. Either source can be unavailable for that pass.
_Avoid_: merged latest values

**Monitoring generation**:
A continuous run of the monitoring engine between creations, across which collection baselines and anomaly detection state can be maintained. Recovery begins a new generation and interrupts anomaly episode continuity.
_Avoid_: owner instance, monitoring session
