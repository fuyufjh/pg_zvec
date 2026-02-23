# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Project Is

`pg_zvec` is a PostgreSQL Foreign Data Wrapper (FDW) extension that integrates Alibaba's **zvec** vector database library, enabling ANN (Approximate Nearest Neighbor) search via SQL (`ORDER BY embedding <=> query LIMIT k`).

The architecture has three tiers:
1. **PostgreSQL backend** — FDW callbacks, `ProcessUtility_hook` for DDL, IPC sender
2. **Background worker** — Single writable zvec Collection handle per collection; serializes all writes
3. **zvec C++ bridge** — C-callable wrapper around the zvec C++ library (`zvec_bridge.cc`)

## Build Commands

```bash
# Build and install (stub mode — no real zvec library needed)
make install

# Build with the real zvec library
USE_ZVEC=1 ZVEC_BUILD=/path/to/zvec/build make install
```

The build uses PGXS. C files compile normally; `src/zvec_bridge/zvec_bridge.cc` is compiled separately with C++17 and linked in. When `USE_ZVEC` is not set, all `zvec_bridge.h` functions return error stubs so development and testing work without the library.

PostgreSQL must be configured with `shared_preload_libraries = 'pg_zvec'` before loading the extension.

## Running Tests

```bash
# Set env vars for the test instance
export PGHOST=/tmp/pg_zvec_socket
export PGPORT=5499

# Run all regression tests
make installcheck

# Run a single test file (e.g., 05_scan)
pg_regress --inputdir=test --outputdir=test --dbname=pg_zvec_regress 05_scan
```

Test SQL lives in `test/sql/` and expected output in `test/expected/`. Diffs on failure appear in `test/regression.diffs`.

## Code Architecture

### Key Files

| File | Role |
|------|------|
| `src/pg_zvec_fdw.c` | Core FDW: OPTIONS parsing, ProcessUtility hook, all FDW callbacks, IPC sender |
| `src/pg_zvec_fdw.h` | Internal structs (`ZvecFdwScanState`, `ZvecFdwModifyState`), GUC declarations |
| `src/pg_zvec_worker.c` | Background worker main loop, per-collection handle registry |
| `src/pg_zvec_shmem.c/h` | Shared memory layout, `PgZvecSharedState`, pack/unpack helpers |
| `src/zvec_bridge/zvec_bridge.h` | C-callable API surface for the C++ bridge |
| `src/zvec_bridge/zvec_bridge.cc` | Wraps zvec `Collection` C++ objects; real impl guarded by `USE_ZVEC` |
| `sql/pg_zvec--2.0.sql` | Extension SQL (registers FDW handler and validator) |
| `test/sql/` | pg_regress test scripts (01–06) |

### IPC Pattern

Backend → worker communication uses a **single shared request/response slot** protected by a named LWLock tranche (`pg_zvec`):

1. Backend acquires exclusive lock, copies `ZvecRequest` into shared slot, sets `request_pending = true`, signals worker latch, releases lock.
2. Worker wakes, processes request (calls zvec APIs), copies `ZvecResponse` into slot, sets `response_ready = true`, signals sender latch.
3. Backend polls (100 ms intervals, 10 s timeout) under shared lock until `response_ready`.

Only one request is in flight at a time — this serializes all writes through the background worker.

### Column Conventions

Foreign tables must follow this layout:
- **Column 1 (attno 1):** Primary key — type `text`
- **First `float4[]` column:** Vector data

These are hard-coded conventions, not validated by type; `BeginForeignModify` locates them by attno and type OID.

### DDL Hook Behavior

`ProcessUtility_hook` intercepts `CREATE FOREIGN TABLE` (fires *after* `standard_ProcessUtility` so the catalog row exists) and `DROP FOREIGN TABLE` (fires *before* to capture relation info). Failures on CREATE/DROP emit `WARNING` (non-blocking); failures on INSERT/DELETE emit `ERROR`.

### Phase Status

- **Phase 1 (complete):** FDW framework, DDL hooks, IPC, stub scan callbacks, regression tests
- **Phase 2 (pending):** Distance operators (`<->`, `<=>`, `<#>`), ANN path pushdown, real SELECT
- **Phase 3 (pending):** Real INSERT/DELETE through zvec APIs
- **Phase 4 (pending):** Scalar filter pushdown, GUC integration, custom vector type

The `zvec/` directory is a git submodule pointing to the upstream zvec library.
