# ferretptimize

An ultra-fast PNG compression microservice written in C with a minimalist HTML/CSS/JS frontend. The backend accepts PNG uploads and produces four optimized variants (PNG lossless, PNG medium, WebP high, AVIF medium) using a lock-free queue and worker pool.

## Requirements

- GCC or Clang with C11 support
- `libpng`, `libwebp`, `libavif`, `pthread`
- GNU Make

On Debian/Ubuntu based systems you can install the libraries with:

```bash
sudo apt install build-essential libpng-dev libwebp-dev libavif-dev
```

## Build

```bash
make
```

The binary `ferretptimize` will be created in the project root.

## Install

```bash
make install PREFIX=/usr/local
```

Override `PREFIX`/`DESTDIR` as needed to install the `ferretptimize` binary into your desired tree.

## Run

```bash
make run
```

Running `make run` from WSL automatically launches Chrome on your Windows host (via `wsl.localhost`) pointing to the local server URL. Override with `FERRET_HOST` / `FERRET_PORT` if needed.

Environment variables:

- `FERRET_HOST` – address to bind (default `0.0.0.0`)
- `FERRET_PORT` – HTTP port (default `4317`)
- `FERRET_WORKERS` – number of worker threads (default `4`)
- `FERRET_QUEUE_SIZE` – capacity for job/result queues (default `128`)

Open `http://localhost:4317/` (from Windows you can also use `http://wsl.localhost:4317/`) in a browser, drag a PNG onto the drop zone, and the frontend will display four compressed variants with download links and size information.

## Testing

Always execute both tiers before shipping changes:

```bash
make test       # lock-free queue stress tests
make autotest   # boots the server and POSTs a generated PNG (fixture kept at tests/assets/test.png for manual use)
```

`make autotest` requires `curl` and leaves nothing running—it spawns the server, POSTs a generated fixture PNG (or tests/assets/test.png manually), validates the JSON payload, then cleans up.

## API

- `GET /` – serves the frontend from `public/`
- `POST /api/compress` – accepts raw PNG bytes (set `Content-Type: application/octet-stream` and `X-Filename` header). Returns JSON containing the compressed payloads encoded as base64.

Example `curl` usage:

```bash
curl -X POST \
     -H "Content-Type: application/octet-stream" \
     -H "X-Filename: sample.png" \
     --data-binary @sample.png \
     http://localhost:8080/api/compress
```

## Project layout

```
include/      – headers for queues, worker pool, compression modules
src/          – backend implementation
public/       – frontend assets
Makefile      – build instructions
PLAN.md       – project overview
```
