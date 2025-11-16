# REFACTOR.md

| Refactor / Improvement | Tech Debt (1-5) | Performance (1-5) | Stability (1-5) | Notes |
|------------------------|-----------------|--------------------|-----------------|-------|
| Replace hand-rolled HTTP parsing with a robust parser (e.g., llhttp or http_parser) and add proper request pipelining/backpressure | 5 | 4 | 5 | Current parser is minimal and blocking; a proven parser would remove subtle bugs and allow better concurrency. |
| Add structured logging + metrics exporter (Prometheus/OpenTelemetry) to observe worker throughput, queue depths, and compression timings | 3 | 3 | 4 | Improves debuggability and production readiness without touching the hot path. |
| Move PNG/WebP/AVIF compression into reusable worker modules with batched memory pools to reduce alloc/free churn | 4 | 5 | 3 | Would lower GC pressure and speed up throughput on multi-core systems. |
| Implement graceful shutdown + health probes (SIGINT drains queues, readiness checks) | 2 | 2 | 5 | Ensures safe restarts and orchestrator friendliness. |
| Support multipart/form-data uploads and parallel response streaming to handle browsers without JS bundling / large files | 4 | 3 | 4 | Broadens compatibility and reduces memory spikes for large payloads. |
