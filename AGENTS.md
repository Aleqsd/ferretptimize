# AGENTS.md

## Workflow Guardrails
- Always rebuild the C backend with `make` after modifying any source under `src/` or `include/`.
- Execute **both** automation tiers (`make test` and `make autotest`) on every run; the latter boots the server and verifies a full PNG upload/response cycle.
- Treat a failing build or test as blocking; fix before proceeding.
- Log the commands you ran and their outcomes in PR discussions when relevant so other agents/operators can audit the run.

## Test Inventory
| Command       | Description |
|---------------|-------------|
| `make`        | Compiles the ferretptimize server and compression pipeline with all dependencies. |
| `make test`   | Builds and runs `tests/run_tests`, stress-testing the lock-free queue implementation. |
| `make autotest` | Launches the server on localhost, uploads a generated PNG via `curl`, and validates the JSON payload (keeps `tests/assets/test.png` for manual use). |
