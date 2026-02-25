# pg_zvec

High-performance vector similarity search for PostgreSQL, powered by [zvec](https://github.com/alibaba/zvec).

pg_zvec is a PostgreSQL Foreign Data Wrapper (FDW) extension that integrates the zvec vector database engine directly into PostgreSQL. It enables approximate nearest neighbor (ANN) search using familiar SQL syntax, making it ideal for AI/ML workloads that need low-latency vector retrieval alongside relational data.

## Features

- **Native SQL syntax** — `ORDER BY embedding <=> query LIMIT k` triggers ANN search automatically
- **Multiple distance metrics** — L2 (`<->`), cosine (`<=>`), negative inner product (`<#>`)
- **Multiple index types** — HNSW, IVF, flat (brute-force)
- **Full CRUD** — INSERT, DELETE, and SELECT on foreign tables
- **Scalar column support** — Store and query text, boolean, integer, bigint, real, and double precision alongside vectors
- **FP16 vectors** — Half-precision storage via column-level `OPTIONS (type 'vector_fp16')`
- **ANN pushdown** — The query planner detects `ORDER BY <distance_op> LIMIT k` patterns and pushes them into zvec
- **Background worker architecture** — Writes are serialized through a background worker for WAL consistency; reads go directly through read-only handles with no IPC overhead

## Requirements

- PostgreSQL 16
- zvec (included as a git submodule)
- C++17 compiler
- CMake (for building zvec)

## Installation

### 1. Clone the repository

```bash
git clone --recurse-submodules https://github.com/user/pg_zvec.git
cd pg_zvec
```

### 2. Build zvec

```bash
cd zvec
mkdir -p build && cd build
cmake .. -GNinja -DCMAKE_BUILD_TYPE=Release
ninja
cd ../..
```

### 3. Build and install the extension

```bash
make -j
sudo make install
```

### 4. Enable the extension

```sql
CREATE EXTENSION pg_zvec;
```

## Quick Start

```sql
-- 1. Create the extension
CREATE EXTENSION pg_zvec;

-- 2. Create a server (configure data directory)
CREATE SERVER zvec_server FOREIGN DATA WRAPPER zvec_fdw
  OPTIONS (data_dir '/var/lib/pg_zvec');

-- 3. Create a foreign table (= a zvec collection)
CREATE FOREIGN TABLE my_vectors (
    id        text,
    content   text,
    embedding float4[]
) SERVER zvec_server
OPTIONS (
    dimension       '1536',
    metric          'cosine',
    index_type      'hnsw',
    m               '16',
    ef_construction '200'
);

-- 4. Insert data
INSERT INTO my_vectors VALUES
  ('doc1', 'hello world', '{0.1, 0.2, ...}');

-- 5. ANN search
SELECT id, content, embedding <=> '{0.1, 0.2, ...}'::float4[] AS distance
FROM my_vectors
ORDER BY embedding <=> '{0.1, 0.2, ...}'::float4[]
LIMIT 10;

-- 6. Delete
DELETE FROM my_vectors WHERE id = 'doc1';
```

## Distance Operators

| Operator | Metric | Description |
|----------|--------|-------------|
| `<->` | L2 | Euclidean distance |
| `<=>` | Cosine | Cosine distance (1 - cosine similarity) |
| `<#>` | IP | Negative inner product |

These operators work on `float4[]` arrays and can be used in any expression:

```sql
-- Direct computation
SELECT '{1,0,0}'::float4[] <=> '{0,1,0}'::float4[] AS distance;

-- ANN search (pushdown when used with ORDER BY ... LIMIT)
SELECT id FROM my_vectors
ORDER BY embedding <-> '{0.1, 0.2, ...}'::float4[]
LIMIT 10;
```

## Table Options

Options specified with `CREATE FOREIGN TABLE ... OPTIONS (...)`:

| Option | Required | Default | Description |
|--------|----------|---------|-------------|
| `dimension` | Yes | — | Vector dimension (positive integer) |
| `metric` | No | `cosine` | Distance metric: `cosine`, `l2`, or `ip` |
| `index_type` | No | `hnsw` | Index type: `hnsw`, `ivf`, or `flat` |
| `m` | No | — | HNSW max connections per node |
| `ef_construction` | No | — | HNSW construction-time search width |
| `nlist` | No | — | IVF number of clusters |

### Column Options

| Option | Values | Description |
|--------|--------|-------------|
| `type` | `vector_fp32` (default), `vector_fp16` | Vector storage precision |

Example with half-precision vectors:

```sql
CREATE FOREIGN TABLE my_vectors (
    id  text,
    emb float4[] OPTIONS (type 'vector_fp16')
) SERVER zvec_server OPTIONS (dimension '768');
```

## Supported Column Types

| PostgreSQL Type | zvec Field Type |
|-----------------|-----------------|
| `text` | STRING |
| `boolean` | BOOL |
| `integer` | INT32 |
| `bigint` | INT64 |
| `real` | FLOAT |
| `double precision` | DOUBLE |
| `float4[]` | VECTOR_FP32 |
| `float4[]` (with `type = 'vector_fp16'`) | VECTOR_FP16 |

The first column is always treated as the primary key.

## GUC Parameters

```ini
# postgresql.conf
pg_zvec.data_dir = '/var/lib/pg_zvec'   # Default data directory
pg_zvec.query_threads = 4                # Threads for query execution
pg_zvec.optimize_threads = 2             # Threads for background optimization
pg_zvec.max_buffer_mb = 64               # Max buffer size in MB
```

## Architecture

```
User SQL Query
  │
  ▼
PostgreSQL Query Planner
  │  GetForeignPaths() detects ORDER BY <op> LIMIT pattern
  │  Generates ForeignScan path with ANN parameters
  ▼
FDW Scan Executor (in backend process)
  │  BeginForeignScan → opens zvec collection read-only handle
  │  IterateForeignScan → calls zvec KNN search, returns rows
  │  EndForeignScan → releases handle
  ▼
zvec Collection (on-disk, $PGDATA/pg_zvec/<name>/)

INSERT / DELETE
  │
  ▼
FDW Modify Executor → IPC → Background Worker (single writer)
  │                          holds collection write handle
  ▼
zvec Collection
```

**Key design decisions:**
- **Reads (ANN search / sequential scan):** Backend holds a read-only handle directly — no worker, no IPC overhead
- **Writes (INSERT / DELETE):** Serialized through a background worker via shared memory IPC for WAL consistency
- **DDL (CREATE / DROP FOREIGN TABLE):** Intercepted via `ProcessUtility_hook` and forwarded to the worker

## Testing

Run the regression test suite:

```bash
# Start a test PostgreSQL instance, then:
PGHOST=/tmp/pg_zvec_socket PGPORT=5499 make installcheck
```

The test suite covers:
- Extension installation and FDW registration
- Server creation and validation
- Table option validation (dimension, metric, index_type, column types)
- DDL operations (CREATE/DROP FOREIGN TABLE)
- Sequential scan and column projection
- INSERT and DELETE operations
- Primary key type support
- FP16 vector storage
- All supported scalar column types
- Distance operator computation
- ANN query pushdown with EXPLAIN verification

## Known Limitations

| Limitation | Description | Workaround |
|------------|-------------|------------|
| No UPDATE support | UPDATE is not supported | Use DELETE + INSERT |
| Writes don't follow transactions | zvec WAL ≠ PostgreSQL WAL; rollback won't undo writes | Accept eventual consistency |
| Single-writer serialization | All writes go through one background worker | Acceptable for typical write patterns |
| LIMIT required for ANN | `ORDER BY <op>` without LIMIT falls back to sequential scan | Always include LIMIT |
| Vector type is `float4[]` | No dedicated vector type — uses standard PostgreSQL arrays | Planned for future |

## License

See [LICENSE](LICENSE) for details.

## Acknowledgments

- [zvec](https://github.com/alibaba/zvec) — The vector database engine by Alibaba
- [PostgreSQL](https://www.postgresql.org/) — The world's most advanced open source relational database
