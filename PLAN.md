# pg_zvec 实施计划

## 项目目标

将阿里巴巴开源的 zvec 向量数据库实现为 PostgreSQL 扩展（pg_zvec），提供比 pgvector 更高性能的 ANN 检索能力。

## 总体架构

```
PostgreSQL Backend Process(es)
  │  SQL 函数调用 / 触发器
  │  共享内存队列 (DSM + LWLock)
  ▼
pg_zvec Background Worker
  │  持有 zvec Collection 单例
  │  处理读写请求
  ▼
zvec Collection (外部文件, $PGDATA/pg_zvec/<name>/)
  ├── Segment 0/          # 向量 + 标量数据
  ├── Segment 1/
  ├── id_map/             # RocksDB (PK → doc_id)
  └── meta/               # Collection 元数据 + PG LSN 水位线
```

## 实施路线

### Phase 1 — MVP：函数式 API + Background Worker

**目标**：可用的最小实现，不涉及 Index AM。

#### 1.1 项目脚手架 ✅
- [x] 创建扩展目录结构（`pg_zvec.c/h`, `pg_zvec.control`, `Makefile`, SQL 文件）
- [x] C++ bridge 通过 `#ifdef USE_ZVEC` 守卫，无 zvec 库也能编译
- [x] PGXS JIT bitcode 步骤：用 `clang++-14` 正确生成 `zvec_bridge.bc`
- [x] 验证：`CREATE EXTENSION pg_zvec` 成功，`zvec_worker_status()` 返回正确数据

**关键构建笔记：**
- `module_pathname = 'pg_zvec'`（不要用 `$libdir/pg_zvec`），让 `dynamic_library_path` 生效
- `PG_WAIT_EXTENSION`（PG 14）而非 `WAIT_EVENT_EXTENSION`
- `BackendPidGetProc` 在 `storage/procarray.h`
- `MyProcPid/MyLatch/MyDatabaseId` 在 `miscadmin.h`
- JIT bitcode 规则：用 `clang++-14 -flto=thin -emit-llvm` 编译 `.cc → .bc`
- 本地测试集群：`initdb + unix_socket_directories=/tmp/pg_zvec_socket + extension_destdir`

#### 1.2 Background Worker ✅
- [x] `pg_zvec_worker.c`：实现 worker 全生命周期（启动/主循环/优雅关闭）
- [x] IPC 协议：单槽位 `ZvecRequest/ZvecResponse` 共享内存 + `LWLock` + `SetLatch/WaitLatch`
- [x] Worker 本地 Collection 注册表：`WorkerCollection worker_colls[64]`（进程私有）
- [x] 共享内存 Collection 注册表：`PgZvecSharedState.collections[]`（供所有 backend 读取 data_dir）
- [x] `process_request()` 实现所有 case：PING / CREATE / DROP / INSERT / DELETE / OPTIMIZE

#### 1.3 管理函数 ✅
- [x] `zvec_create_collection(name, table_name, vector_col, dimension, metric, index_type, params, data_dir)` — 打包 IPC payload 发送给 worker
- [x] `zvec_drop_collection(name)` — IPC DROP
- [x] `zvec_optimize(name)` — IPC OPTIMIZE（30 秒超时）
- [x] `zvec_stats(name)` — backend 直接打开只读 handle，返回 doc_count

**API 变更说明（相对 PLAN.md 初版）：**
- `zvec_create_collection` 增加了显式的 `dimension integer` 和 `metric text` 参数，去掉了从 params JSON 隐式解析的逻辑
- 最终签名：`(collection_name, table_name, vector_column, dimension, metric DEFAULT 'cosine', index_type DEFAULT 'hnsw', params DEFAULT '{}', data_dir DEFAULT '')`

#### 1.4 数据同步（触发器）✅
- [x] `zvec_sync_trigger()` 触发器：从 `TriggerData` 提取 pk（any type → 转 text via output function）+ float4[] 向量，发送 ZVEC_REQ_INSERT/ZVEC_REQ_DELETE
- [x] `zvec_attach_table(collection, table, pk_col, vec_col)` — 通过 SPI 执行 `CREATE TRIGGER` 安装触发器
- [x] NULL pk / NULL vector 行安全跳过
- [x] 平铺二进制 pack/unpack 辅助函数定义在 `pg_zvec_shmem.h` 中（`zvec_pack_str/int/floats`, `zvec_unpack_str/int/floats`）

**已知限制（记录于此供 Phase 2 改进）：**
- UPDATE 时若 PK 发生变化，旧 PK 不会从 zvec 中删除（文档化限制；运行 `zvec_optimize()` 可触发后台 GC）
- 触发器在行级立即发送 IPC（未缓冲至事务 COMMIT），rollback 后 zvec 可能有孤立向量
- 单槽 IPC：同一时刻只有一个 backend 可以向 worker 发请求，并发写会得到"worker is busy"

#### 1.5 检索函数 ✅
- [x] `zvec_search(collection, query float4[], topk)` — SRF，backend 直接打开只读 handle 执行搜索（不走 worker IPC，无并发瓶颈）
- [x] `zvec_search_filtered(collection, query float4[], topk, filter text)` — 同上 + 标量过滤表达式

#### 1.6 崩溃恢复
- [ ] 在 collection 元数据中记录 PG LSN 水位线
- [ ] worker 启动时从 LSN 水位线之后重放 PG 的变更（或标记 collection 需要重建）

#### Phase 1 验收标准（IPC 框架已全链路验证 ✅，需 USE_ZVEC=1 重建后进行完整功能验证）

```sql
-- 当前 API（dimension/metric 为显式参数）
CREATE EXTENSION pg_zvec;
CREATE TABLE items (id text PRIMARY KEY, embedding float4[]);
SELECT zvec_create_collection('items_idx', 'items', 'embedding', 1536,
       'cosine', 'hnsw', '{"m":16,"ef_construction":200}');
SELECT zvec_attach_table('items_idx', 'items', 'id', 'embedding');

INSERT INTO items VALUES ('doc1', array_fill(0.1::float4, ARRAY[1536]));
-- 触发器自动同步到 zvec（IPC → worker → zvec_collection_upsert）

SELECT * FROM zvec_search('items_idx', array_fill(0.1::float4, ARRAY[1536]), 10);
-- → (pk text, score float4) 行集
```

**Stub 模式下已验证行为：**
- `CREATE EXTENSION / zvec_worker_status()` ✅
- `zvec_create_collection()` IPC payload 全链路正确传递 ✅
- `zvec_attach_table()` 通过 SPI 安装 AFTER 触发器 ✅
- 触发器 fire (INSERT/DELETE)：提取 pk + float4[]，发送 IPC，worker 响应 WARNING ✅
- `zvec_search()` 在 collection 不存在时返回明确错误 ✅

---

### Phase 2 — 自定义向量类型 + 算符

**目标**：提供更符合 SQL 习惯的 API，与 pgvector 接口兼容。

- [ ] 定义 `zvec` 类型（或复用 pgvector `vector` 类型）
- [ ] 实现输入/输出函数：`'[0.1, 0.2, 0.3]'::zvec`
- [ ] 定义距离算符：
  - `<->` L2 距离
  - `<=>` 余弦距离
  - `<#>` 内积距离
- [ ] 实现 `zvec_distance(a zvec, b zvec, metric text) → float4`
- [ ] 支持 `ORDER BY embedding <=> query_vec LIMIT 10` 语法（通过 operator class）

---

### Phase 3 — Index Access Method（可选，长期目标）

**目标**：深度集成到 PG 查询优化器，支持 `CREATE INDEX USING zvec`。

- [ ] 实现 `IndexAmRoutine`（ambuild, aminsert, amgettuple, amendscan 等）
- [ ] 向量索引数据存储策略（外部文件 or PG relation pages）
- [ ] 与 PG 查询规划器集成（`amcostestimate`）
- [ ] 支持 `SET zvec.ef_search = 64` 等 session 级参数

---

## GUC 参数规划

```ini
# postgresql.conf
pg_zvec.data_dir = ''           # 默认 $PGDATA/pg_zvec
pg_zvec.query_threads = 4
pg_zvec.optimize_threads = 2
pg_zvec.max_buffer_size = '64MB'
pg_zvec.log_level = 'warn'
```

---

## 目录结构规划

```
pg_zvec/
├── PLAN.md                  # 本文件
├── zvec/                    # zvec 源码（git submodule 或直接引用）
├── src/
│   ├── pg_zvec.c            # 扩展入口，SQL 函数注册
│   ├── pg_zvec_worker.c     # Background Worker
│   ├── pg_zvec_ipc.c        # 共享内存 / IPC 协议
│   ├── pg_zvec_trigger.c    # 触发器实现
│   ├── pg_zvec_search.c     # 检索函数
│   ├── pg_zvec_type.c       # zvec 向量类型（Phase 2）
│   └── pg_zvec_index.c      # Index AM（Phase 3）
├── sql/
│   ├── pg_zvec--1.0.sql     # 扩展 SQL 定义
│   └── pg_zvec--1.0--1.1.sql
├── pg_zvec.control
├── Makefile                 # PGXS
└── CMakeLists.txt           # 编译 zvec 静态库
```

---

## 已知风险与应对

| 风险 | 应对 |
|------|------|
| RocksDB 符号与 PG 或其他扩展冲突 | 静态链接 + `-fvisibility=hidden` |
| worker 崩溃导致 collection 状态不一致 | LSN 水位线 + 重启重放 |
| 大量数据 rollback 后 zvec 有孤立向量 | 后台 GC 任务定期清理不在 PG heap 中的 doc_id |
| zvec 未内置 PG MVCC 可见性 | Phase 1 接受最终一致性；Phase 3 通过 TAM 解决 |
| 编译依赖复杂（RocksDB / Arrow / Protobuf） | Docker 构建环境，CI 固定依赖版本 |

---

## 变更日志

| 日期 | 变更 |
|------|------|
| 2026-02-22 | 初始调研完成，创建 PLAN.md，确定三阶段路线 |
| 2026-02-22 | Phase 1.1 脚手架完成，验证 CREATE EXTENSION 和 zvec_worker_status() |
| 2026-02-22 | Phase 1.2–1.5 完成：IPC 序列化、worker Collection 注册表、管理函数、触发器、搜索函数。全链路 IPC 验证通过（stub 模式） |
