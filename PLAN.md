# pg_zvec 实施计划

## 项目目标

将阿里巴巴开源的 zvec 向量数据库实现为 PostgreSQL FDW 扩展（pg_zvec），提供比 pgvector 更高性能的 ANN 检索能力，并以 `ORDER BY embedding <-> query LIMIT k` 的原生 SQL 语法暴露搜索能力。

---

## 总体架构

```
用户 SQL
  │
  │  SELECT * FROM my_vectors
  │  ORDER BY embedding <-> '[0.1, 0.2, ...]'
  │  LIMIT 10;
  │
  ▼
PostgreSQL 查询规划器
  │  GetForeignPaths() 识别 ORDER BY <-> LIMIT 模式
  │  生成 ForeignScan 路径，fdw_private 携带 {query_vec, k, filter}
  ▼
FDW Scan 执行器（backend 进程内）
  │  BeginForeignScan → 打开 zvec Collection 只读句柄
  │  IterateForeignScan → 调用 zvec KNN search，逐行返回
  │  EndForeignScan → 释放句柄
  ▼
zvec Collection（文件系统，$PGDATA/pg_zvec/<name>/）

INSERT INTO my_vectors VALUES (...);
  │
  ▼
FDW Modify 执行器
  │  ExecForeignInsert → worker IPC（单写者）
  ▼
pg_zvec Background Worker（持有 Collection 写句柄）
```

**核心设计决策：**
- **读（ANN搜索/顺序扫描）**：backend 直接持有只读句柄，无需 worker，无并发瓶颈
- **写（INSERT/DELETE）**：通过 background worker IPC 串行化，保证 zvec WAL 一致性
- **DDL（CREATE/DROP FOREIGN TABLE）**：通过 worker IPC 创建/销毁 Collection
- **ANN 下推**：`GetForeignPaths` 识别 `ORDER BY <op>(col, $param) LIMIT k` 模式

---

## DDL 用法（目标）

```sql
-- 安装扩展（注册 FDW handler）
CREATE EXTENSION pg_zvec;

-- FDW 已由扩展自动创建（无需手动 CREATE FOREIGN DATA WRAPPER）

-- 创建服务器（全局配置）
CREATE SERVER zvec_server FOREIGN DATA WRAPPER zvec_fdw
  OPTIONS (data_dir '/var/lib/pg_zvec');

-- 创建外部表 = 创建一个 zvec Collection
CREATE FOREIGN TABLE my_vectors (
    id      text,
    content text,
    embedding float4[]
)
SERVER zvec_server
OPTIONS (
    dimension    '1536',
    metric       'cosine',    -- cosine | l2 | ip
    index_type   'hnsw',      -- hnsw | ivf | flat
    m            '16',
    ef_construction '200'
);

-- 自然的 ANN 搜索
SELECT id, content, embedding <=> '[0.1, 0.2, ...]'::float4[] AS score
FROM my_vectors
ORDER BY embedding <=> '[0.1, 0.2, ...]'::float4[]
LIMIT 10;

-- 带标量过滤（pushdown 到 zvec filter engine）
SELECT id, content
FROM my_vectors
WHERE content LIKE '%postgres%'
ORDER BY embedding <=> '[...]'::float4[]
LIMIT 10;

-- 写入
INSERT INTO my_vectors VALUES ('doc1', 'hello world', '{0.1, 0.2, ...}');
DELETE FROM my_vectors WHERE id = 'doc1';
```

---

## 实施路线

### Phase 1 — FDW 框架 + DDL 集成

**目标**：注册 FDW，`CREATE FOREIGN TABLE` 能创建 zvec Collection，`DROP FOREIGN TABLE` 能销毁它。

#### 1.1 项目脚手架重构

丢弃旧的函数式 API，重构为 FDW 框架：

- [ ] `pg_zvec_fdw.c` — 核心 FDW handler，注册 `FdwRoutine`
- [ ] `pg_zvec_fdw.h` — FDW 内部共享结构定义
- [ ] `pg_zvec_worker.c` — Background Worker（保留，负责写路径 + Collection 生命周期）
- [ ] `pg_zvec_shmem.c/h` — 共享内存 + IPC（保留，结构不变）
- [ ] `zvec_bridge.cc/h` — C++ bridge（保留，`#ifdef USE_ZVEC` 守卫）
- [ ] `pg_zvec--2.0.sql` — 注册 FDW handler、FDW、距离算符、算符类

**SQL 扩展注册：**
```sql
CREATE FUNCTION zvec_fdw_handler() RETURNS fdw_handler ...;
CREATE FUNCTION zvec_fdw_validator(text[], oid) RETURNS void ...;
CREATE FOREIGN DATA WRAPPER zvec_fdw
    HANDLER zvec_fdw_handler
    VALIDATOR zvec_fdw_validator;
```

#### 1.2 FDW Handler 骨架

实现 `FdwRoutine` 的全部回调（大多数先返回 ereport/空实现）：

**DDL 钩子（通过 ProcessUtility_hook 或 object_access_hook）：**
- `CREATE FOREIGN TABLE` → 解析 OPTIONS，发送 `ZVEC_REQ_CREATE` 给 worker
- `DROP FOREIGN TABLE` → 发送 `ZVEC_REQ_DROP` 给 worker

**OPTIONS 验证（`zvec_fdw_validator`）：**
```
必需：dimension（正整数）
可选：metric（cosine/l2/ip，默认 cosine）
      index_type（hnsw/ivf/flat，默认 hnsw）
      m、ef_construction、nlist（索引参数）
```

#### 1.3 Background Worker（复用 Phase 1 实现）

- [ ] 复用已有 worker IPC 框架（`ZVEC_REQ_CREATE/DROP/INSERT/DELETE/OPTIMIZE`）
- [ ] Worker 在启动时从 `pg_foreign_table` 系统表读取所有外部表，恢复 Collection 句柄

---

### Phase 2 — 距离算符 + ANN 下推

**目标**：`ORDER BY embedding <=> query LIMIT k` 自动走 zvec ANN 搜索路径。

#### 2.1 自定义距离算符

为 `float4[]` 定义三个距离算符（参考 pgvector 约定）：

| 算符 | 语义 | zvec metric |
|------|------|-------------|
| `<->` | L2 距离 | `l2` |
| `<=>` | 余弦距离 | `cosine` |
| `<#>` | 负内积 | `ip` |

```sql
CREATE FUNCTION zvec_l2_distance(float4[], float4[]) RETURNS float4 ...;
CREATE OPERATOR <-> (LEFTARG = float4[], RIGHTARG = float4[], FUNCTION = zvec_l2_distance, COMMUTATOR = <->);
-- 同理 <=> 和 <#>
```

算符函数实现：
- 有 zvec 时（`USE_ZVEC=1`）：调用 zvec 精确距离计算
- 无 zvec 时：纯 C 实现（用于测试）

#### 2.2 GetForeignPaths — ANN 路径识别

`GetForeignPaths` 是 ANN 下推的核心。检测条件：

```
1. query pathkeys 包含 ORDER BY f(col, Param/Const) ASC
2. f 是我们注册的距离算符之一
3. 存在 LIMIT（通过 root->limit_tuples > 0 判断）
4. col 是本外部表的向量列（对照 Options 中的 dimension 确认）
```

满足条件时，生成 `ForeignPath`，`fdw_private` 携带：
```c
typedef struct ZvecANNInfo {
    int         vec_attno;      /* 向量列的属性编号 */
    Oid         dist_op_oid;    /* 使用的距离算符 OID */
    Node       *query_expr;     /* query vector 表达式（Const 或 Param）*/
    double      limit_tuples;   /* LIMIT 值 */
    List       *remote_conds;   /* 可下推的标量过滤条件 */
    List       *local_conds;    /* 不可下推的条件（在 PG 层过滤）*/
} ZvecANNInfo;
```

代价估算：
- ANN 路径 startup_cost ≈ 0，total_cost ≈ k * per_tuple_cost（极低）
- 顺序扫描路径按 doc_count 估算（通过 `zvec_stats()` 获取）

#### 2.3 GetForeignPlan / BeginForeignScan / IterateForeignScan

```c
/* BeginForeignScan */
void zvec_begin_foreign_scan(ForeignScanState *node, int eflags) {
    // 从 fdw_private 反序列化 ZvecANNInfo
    // 打开 zvec Collection 只读句柄（backend 内，无需 IPC）
    // 若是 ANN 路径：执行 collection->search(query_vec, k, filter)
    //   把结果暂存到 FdwScanState.results
    // 若是顺序扫描路径：初始化 collection->full_scan() 游标
}

/* IterateForeignScan：逐行从 results 弹出 */
TupleTableSlot *zvec_iterate_foreign_scan(ForeignScanState *node) {
    // 从 results 取下一条，填充 slot
}
```

---

### Phase 3 — ForeignModify（INSERT / DELETE）

**目标**：`INSERT INTO` 和 `DELETE FROM` 正常工作。

#### 3.1 INSERT

```
ExecForeignInsert
  → 提取向量列 + 所有标量列
  → 序列化为 IPC payload
  → 发送 ZVEC_REQ_INSERT 给 worker
  → 等待响应
```

**事务语义说明（已知限制）：**
- 触发时机为语句执行时（非事务 COMMIT），rollback 后 zvec 可能有孤立向量
- Phase 3 接受最终一致性；通过定期 `OPTIMIZE` GC 清理（类似 Elasticsearch 的 soft delete）

#### 3.2 DELETE

```
ExecForeignDelete
  → 从 plan 的 junk attribute 取出 zvec doc_id（或 PK）
  → 发送 ZVEC_REQ_DELETE 给 worker
```

为支持 DELETE，`AddForeignUpdateTargets` 需将 PK 列标记为 junk target。

#### 3.3 UPDATE（暂不支持）

- FdwRoutine.ExecForeignUpdate = NULL（PG 会报 "foreign table does not support UPDATE"）
- 用户可通过 DELETE + INSERT 实现

---

### Phase 4 — 标量过滤下推 + 高级特性

**目标**：将 WHERE 条件下推到 zvec 的 filter engine（ANTLR SQL parser）。

#### 4.1 条件分类（GetForeignPaths 中完成）

```
可下推：
  - col = const（标量等值）
  - col > / < / >= / <= const（标量范围）
  - col LIKE 'prefix%'（string prefix，如 zvec 支持）

不可下推：
  - 跨表 JOIN 条件
  - 复杂表达式
  - 涉及非下推函数的条件
```

#### 4.2 条件转换为 zvec filter string

实现 `deparse_expr()` 将 PG `Expr` 转换为 zvec filter SQL 字符串：
```
(pg) col = 'foo'::text  →  (zvec) "col" = 'foo'
(pg) score > 0.5        →  (zvec) "score" > 0.5
```

#### 4.3 IMPORT FOREIGN SCHEMA

实现 `ImportForeignSchema`，将 zvec data_dir 下已有的 Collection 批量导入为外部表。

#### 4.4 GUC 参数

```ini
# postgresql.conf
pg_zvec.ef_search = 64        # 全局 ef_search 覆盖（可被 SET LOCAL 覆盖）
pg_zvec.query_threads = 4
pg_zvec.optimize_threads = 2
pg_zvec.log_level = 'warn'
```

---

## 目录结构

```
pg_zvec/
├── PLAN.md
├── zvec/                        # git submodule
├── src/
│   ├── pg_zvec_fdw.c            # FDwRoutine 全部回调（主文件）
│   ├── pg_zvec_fdw.h            # FdwScanState、ZvecANNInfo 等内部结构
│   ├── pg_zvec_pathkeys.c       # GetForeignPaths：ANN 路径识别逻辑
│   ├── pg_zvec_deparse.c        # 条件表达式 → zvec filter string
│   ├── pg_zvec_worker.c         # Background Worker（写路径 + 生命周期）
│   ├── pg_zvec_shmem.c/h        # 共享内存 + IPC 协议
│   └── zvec_bridge.cc/h         # C++ → C bridge（USE_ZVEC 守卫）
├── sql/
│   ├── pg_zvec--2.0.sql         # FDW + 算符 + 算符类 SQL 定义
│   └── pg_zvec--1.x--2.0.sql   # 升级脚本（如需）
├── pg_zvec.control
└── Makefile                     # PGXS
```

---

## 关键技术难点

### T1：GetForeignPaths 中识别 ORDER BY 算符

PG 规划器以 `PathKey` 列表表示排序需求。需要：
1. 遍历 `root->query_pathkeys`
2. 对每个 PathKey，检查其 `pk_eclass` 中的等价成员
3. 识别形如 `OpExpr(our_op, Var(our_rel, vec_attno), param_or_const)` 的表达式
4. 确认 `pk_strategy = BTLessStrategyNumber`（ASC，最近邻排前面）

参考实现：`postgres_fdw` 的 `find_em_for_rel()`，`multicorn` 的 pathkeys 处理。

### T2：query vector 的运行时求值

query vector 可能是参数（Prepared Statement）或常量：
- 常量：在 `GetForeignPlan` 时直接序列化进 `fdw_private`
- 参数（Param）：在 `BeginForeignScan` 时通过 `ExecEvalExpr` 求值

### T3：zvec Collection 生命周期与 `CREATE FOREIGN TABLE` 的绑定

`CREATE FOREIGN TABLE` 是标准 DDL，PG 不提供 FDW 回调。需要使用：
- `object_access_hook`：`OAT_POST_CREATE` 对 `ForeignTableRelationId` 触发创建
- `object_access_hook`：`OAT_DROP` 触发销毁

或者：在 `BeginForeignScan` / `BeginForeignModify` 时懒惰创建 Collection（首次访问时创建）。

### T4：多进程只读句柄安全性

zvec 的只读句柄是否线程/进程安全需验证（见 zvec `collection.h`）。
如果不安全，需每次 BeginForeignScan 打开、EndForeignScan 关闭（开销可接受，因为 ANN 搜索耗时远大于句柄开销）。

---

## 已知限制（Phase 2-3 接受）

| 限制 | 说明 | 未来改进 |
|------|------|----------|
| 不支持 UPDATE | 需 DELETE + INSERT | Phase 4 |
| 写不随事务回滚 | zvec WAL ≠ PG WAL | 长期：Logical Replication 槽 |
| 单写者串行化 | worker IPC 瓶颈 | 评估 zvec 多进程写安全性后改进 |
| ORDER BY 必须包含 LIMIT | 无 LIMIT 时退化为顺序扫描 | 可接受 |
| 向量列类型为 float4[] | 非 zvec 专属类型，无类型安全 | Phase 4：定义 zvec 类型 |

---

## 已知风险

| 风险 | 应对 |
|------|------|
| RocksDB 符号与 PG 冲突 | 静态链接 + `-fvisibility=hidden` |
| GetForeignPaths 识别失败（退化顺序扫描） | 增加 EXPLAIN 输出调试信息，文档化触发条件 |
| zvec 只读句柄多进程安全性未知 | 验证后决定是否加 lwlock 保护 |
| 编译依赖复杂（RocksDB / Arrow / Protobuf） | Docker 构建环境，CI 固定依赖版本 |

---

## 验收标准

### Phase 1
```sql
CREATE EXTENSION pg_zvec;
CREATE SERVER zvec_server FOREIGN DATA WRAPPER zvec_fdw OPTIONS (data_dir '/tmp/zvec_data');
CREATE FOREIGN TABLE vecs (id text, emb float4[]) SERVER zvec_server OPTIONS (dimension '4', metric 'cosine');
-- zvec Collection 被创建
DROP FOREIGN TABLE vecs;
-- zvec Collection 被销毁
```

### Phase 2
```sql
INSERT INTO vecs VALUES ('a', '{1,0,0,0}'), ('b', '{0,1,0,0}'), ('c', '{0,0,1,0}');
EXPLAIN SELECT id FROM vecs ORDER BY emb <=> '{1,0.1,0,0}' LIMIT 3;
-- 输出中出现 "Foreign Scan on vecs" + "ZvecANN: k=3 metric=cosine"
SELECT id FROM vecs ORDER BY emb <=> '{1,0.1,0,0}' LIMIT 3;
-- → 返回 a, b, c（按余弦距离排序）
```

### Phase 3
```sql
INSERT INTO vecs VALUES ('d', '{0,0,0,1}');
-- INSERT 通过 ForeignModify → worker IPC 成功
DELETE FROM vecs WHERE id = 'a';
-- DELETE 通过 ForeignModify → worker IPC 成功
```

---

## 变更日志

| 日期 | 变更 |
|------|------|
| 2026-02-22 | 初始函数式 API 方案（Phase 1 完成） |
| 2026-02-23 | 架构重新设计：放弃函数式 API，改为 FDW + ANN 下推方案 |
