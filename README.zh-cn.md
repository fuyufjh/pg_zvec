# pg_zvec

基于 [zvec](https://github.com/alibaba/zvec) 的 PostgreSQL 高性能向量相似度搜索扩展。

pg_zvec 是一个 PostgreSQL 外部数据包装器 (FDW) 扩展，将 zvec 向量数据库引擎直接集成到 PostgreSQL 中。它支持使用标准 SQL 语法进行近似最近邻 (ANN) 搜索，非常适合需要低延迟向量检索与关系数据协同工作的 AI/ML 场景。

## 特性

- **原生 SQL 语法** — `ORDER BY embedding <=> query LIMIT k` 自动触发 ANN 搜索
- **多种距离度量** — L2 (`<->`)、余弦 (`<=>`)、负内积 (`<#>`)
- **多种索引类型** — HNSW、IVF、flat（暴力搜索）
- **完整 CRUD** — 支持外部表的 INSERT、DELETE 和 SELECT
- **标量列支持** — 向量旁可存储和查询 text、boolean、integer、bigint、real、double precision 类型
- **FP16 向量** — 通过列级 `OPTIONS (type 'vector_fp16')` 支持半精度存储
- **ANN 下推** — 查询规划器检测 `ORDER BY <距离算符> LIMIT k` 模式并下推到 zvec 执行
- **后台工作进程架构** — 写操作通过后台工作进程串行化以保证 WAL 一致性；读操作直接使用只读句柄，无 IPC 开销

## 环境要求

- PostgreSQL 16
- zvec（作为 git 子模块包含）
- C++17 编译器
- CMake（用于构建 zvec）

## 安装

### 1. 克隆仓库

```bash
git clone --recurse-submodules https://github.com/user/pg_zvec.git
cd pg_zvec
```

### 2. 构建 zvec

```bash
cd zvec
mkdir -p build && cd build
cmake .. -GNinja -DCMAKE_BUILD_TYPE=Release
ninja
cd ../..
```

### 3. 编译并安装扩展

```bash
make -j
sudo make install
```

### 4. 启用扩展

```sql
CREATE EXTENSION pg_zvec;
```

## 快速开始

```sql
-- 1. 创建扩展
CREATE EXTENSION pg_zvec;

-- 2. 创建服务器（配置数据目录）
CREATE SERVER zvec_server FOREIGN DATA WRAPPER zvec_fdw
  OPTIONS (data_dir '/var/lib/pg_zvec');

-- 3. 创建外部表（= 创建一个 zvec Collection）
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

-- 4. 插入数据
INSERT INTO my_vectors VALUES
  ('doc1', 'hello world', '{0.1, 0.2, ...}');

-- 5. ANN 搜索
SELECT id, content, embedding <=> '{0.1, 0.2, ...}'::float4[] AS distance
FROM my_vectors
ORDER BY embedding <=> '{0.1, 0.2, ...}'::float4[]
LIMIT 10;

-- 6. 删除
DELETE FROM my_vectors WHERE id = 'doc1';
```

## 距离算符

| 算符 | 度量 | 说明 |
|------|------|------|
| `<->` | L2 | 欧氏距离 |
| `<=>` | Cosine | 余弦距离（1 - 余弦相似度）|
| `<#>` | IP | 负内积 |

这些算符作用于 `float4[]` 数组，可用于任意表达式：

```sql
-- 直接计算
SELECT '{1,0,0}'::float4[] <=> '{0,1,0}'::float4[] AS distance;

-- ANN 搜索（配合 ORDER BY ... LIMIT 时自动下推）
SELECT id FROM my_vectors
ORDER BY embedding <-> '{0.1, 0.2, ...}'::float4[]
LIMIT 10;
```

## 表选项

通过 `CREATE FOREIGN TABLE ... OPTIONS (...)` 指定：

| 选项 | 必填 | 默认值 | 说明 |
|------|------|--------|------|
| `dimension` | 是 | — | 向量维度（正整数）|
| `metric` | 否 | `cosine` | 距离度量：`cosine`、`l2` 或 `ip` |
| `index_type` | 否 | `hnsw` | 索引类型：`hnsw`、`ivf` 或 `flat` |
| `m` | 否 | — | HNSW 每节点最大连接数 |
| `ef_construction` | 否 | — | HNSW 构建时搜索宽度 |
| `nlist` | 否 | — | IVF 聚类数量 |

### 列选项

| 选项 | 取值 | 说明 |
|------|------|------|
| `type` | `vector_fp32`（默认）、`vector_fp16` | 向量存储精度 |

半精度向量示例：

```sql
CREATE FOREIGN TABLE my_vectors (
    id  text,
    emb float4[] OPTIONS (type 'vector_fp16')
) SERVER zvec_server OPTIONS (dimension '768');
```

## 支持的列类型

| PostgreSQL 类型 | zvec 字段类型 |
|-----------------|---------------|
| `text` | STRING |
| `boolean` | BOOL |
| `integer` | INT32 |
| `bigint` | INT64 |
| `real` | FLOAT |
| `double precision` | DOUBLE |
| `float4[]` | VECTOR_FP32 |
| `float4[]`（设置 `type = 'vector_fp16'`）| VECTOR_FP16 |

第一列始终作为主键使用。

## GUC 参数

```ini
# postgresql.conf
pg_zvec.data_dir = '/var/lib/pg_zvec'   # 默认数据目录
pg_zvec.query_threads = 4                # 查询执行线程数
pg_zvec.optimize_threads = 2             # 后台优化线程数
pg_zvec.max_buffer_mb = 64               # 最大缓冲区大小（MB）
```

## 架构

```
用户 SQL 查询
  │
  ▼
PostgreSQL 查询规划器
  │  GetForeignPaths() 识别 ORDER BY <op> LIMIT 模式
  │  生成 ForeignScan 路径，携带 ANN 参数
  ▼
FDW 扫描执行器（后端进程内）
  │  BeginForeignScan → 打开 zvec Collection 只读句柄
  │  IterateForeignScan → 调用 zvec KNN 搜索，逐行返回
  │  EndForeignScan → 释放句柄
  ▼
zvec Collection（磁盘存储，$PGDATA/pg_zvec/<name>/）

INSERT / DELETE
  │
  ▼
FDW Modify 执行器 → IPC → 后台工作进程（单写者）
  │                        持有 Collection 写句柄
  ▼
zvec Collection
```

**核心设计决策：**
- **读（ANN 搜索 / 顺序扫描）：** 后端直接持有只读句柄——无需工作进程，无 IPC 开销
- **写（INSERT / DELETE）：** 通过共享内存 IPC 发送到后台工作进程串行化执行，保证 WAL 一致性
- **DDL（CREATE / DROP FOREIGN TABLE）：** 通过 `ProcessUtility_hook` 拦截并转发到工作进程

## 测试

运行回归测试套件：

```bash
# 启动测试用 PostgreSQL 实例后执行：
PGHOST=/tmp/pg_zvec_socket PGPORT=5499 make installcheck
```

测试套件覆盖：
- 扩展安装与 FDW 注册
- 服务器创建与验证
- 表选项验证（dimension、metric、index_type、列类型）
- DDL 操作（CREATE/DROP FOREIGN TABLE）
- 顺序扫描与列投影
- INSERT 和 DELETE 操作
- 主键类型支持
- FP16 向量存储
- 所有支持的标量列类型
- 距离算符计算
- ANN 查询下推及 EXPLAIN 验证

## 已知限制

| 限制 | 说明 | 解决方案 |
|------|------|----------|
| 不支持 UPDATE | 不支持 UPDATE 操作 | 使用 DELETE + INSERT |
| 写操作不跟随事务 | zvec WAL ≠ PostgreSQL WAL，回滚不会撤销写入 | 接受最终一致性 |
| 单写者串行化 | 所有写操作经由单个后台工作进程 | 对于典型写入模式可接受 |
| ANN 搜索需要 LIMIT | 没有 LIMIT 的 `ORDER BY <op>` 会退化为顺序扫描 | 始终包含 LIMIT |
| 向量类型为 float4[] | 没有专用向量类型，使用标准 PostgreSQL 数组 | 未来规划中 |

## 许可证

详见 [LICENSE](LICENSE)。

## 致谢

- [zvec](https://github.com/alibaba/zvec) — 阿里巴巴开源的向量数据库引擎
- [PostgreSQL](https://www.postgresql.org/) — 全球最先进的开源关系型数据库
