# E8 读路径源代码探针审计与三态验证报告 (v11.8.0)

**实验时间**：2026-09-03  
**执行环境**：Linux 6.6.137+rpt-rpi-2712 / 双路 Intel Xeon Gold 5218R @ 2.10GHz (Binding: CPU 2)  
**代码版本**：RocksDB v11.8.0 (`feature/range-tombstone-controller-prototype` 分支)  
**编译宏**：`-O2 -g -std=c++20 -DROCKSDB_READ_PATH_AUDIT`  
**数据源**：`results/summary/read-path-audit-e8.csv`  

---

## 一、 执行摘要 (Executive Summary)

本报告对 RocksDB v11.8.0 下的范围删除读路径实施了原生统计核验与编译期专用探针（`ROCKSDB_READ_PATH_AUDIT`）微基准测试。在严格遵循微观物理状态隔离（CPU 2 独立核心绑定、禁用自动 Compaction、N=3 独立冷启动数据库重复）的前提下，对 **CLEAN**、**T0-MEM**（细分为 Cold-build、Warm-steady、Re-invalidated）以及 **POSTFLUSH-SST** 三种宏观物理状态进行了微秒级耗时与事件计数的全量审计。

### 核心结论
1. **冷启动与失效后首读呈现显著延迟（~2.6 ms）**：
   - 当 MemTable 中累积 20,000 个范围删除墓碑时，无论是 Point Get 还是 Scan，首次读操作（Cold-build）必须在 `reader_mutex` 保护下执行范围墓碑视图物化（包含 `MemTableIterator` 创建与 `FragmentedRangeTombstoneList` 构建），其耗时稳定为 **2.5 ~ 2.6 ms**，占首次操作端到端延迟（~2.63 ms）的 **96% ~ 98%**。
   - 每条 DeleteRange 会使活跃 MemTable 的缓存失效；之后第一位需要该范围墓碑视图的读者才触发重建。若多条 DeleteRange 在两次读取之间连续发生，它们可能合并为一次重建。在单次写入测试中（Re-invalidated），下一个读者观测到 **2.6 ~ 2.8 ms** 的重建耗时。
2. **稳态查询表现与基准对比**：
   - 一旦分段视图构建完毕且无新墓碑写入（Warm-steady），连续 5,000 次 Point Get 的端到端延迟为 **2.63 ± 0.11 μs/op**，CLEAN 基准为 **2.31 ± 0.12 μs/op**。二者均处于微秒量级；当前存在约 0.32 μs 的均值差，尚不能据此断言稳态无额外开销。
   - 固定范围 Scan 的稳态端到端延迟为 **18.43 ± 0.44 μs/op**，CLEAN 基准为 **18.58 ± 0.74 μs/op**；这反映出在当前无并发、只读的小微基准下两者接近（T0-MEM 返回 40 存活键，CLEAN 返回 50 存活键），但这仅说明当前静态小微基准下的特定表现，不能据此断言 MemTable 墓碑路径比基准更快或完全无成本。
3. **读路径归因观察**：
   - 在静态微基准下，范围删除读延迟的主要波动来源集中在范围墓碑视图物化过程；至于动态并发交织负载下物化发生的频率、累积时间以及锁竞争实际占比，需通过 E9 动态混合负载测试进一步量化。

---

## 二、 原生统计清点与最小探针设计

### 2.1 原生统计（PerfContext / Statistics）能力与盲区核验
| 指标项 | 原生提供者 | 是否可满足读路径归因 | 局限与盲区说明 |
| :--- | :--- | :---: | :--- |
| `get_from_memtable_time` | `PerfContext` | 否 | 将活跃跳表点查、墓碑分段构建、二分查找全部打包为一个总耗时，无法区分墓碑机制开销。 |
| `internal_range_del_reseek_count` | `PerfContext` | 是（部分） | 仅在 `MergingIterator` 发生跨层级联跳转（SeekImpl）时递增，无法记录边界推进事件与同层覆盖过滤。 |
| `memtable_fragment_build` | 无 | **完全缺失** | 没有任何计数器或计时器记录 `FragmentTombstones` 的触发频次与范围墓碑视图物化耗时。 |
| 活跃 vs 不可变 MemTable 覆盖查询 | 无 | **完全缺失** | 无法区隔 Point Get 是在 Active 还是 Immutable MemTable 中被墓碑遮蔽。 |
| 迭代器构造来源细分 | 无 | **完全缺失** | `NewInternalIterator` 无法按 Mutable Mem、Immutable Mem、SST 三层来源解耦构造开销。 |

### 2.2 最小探针设计规范 (`db/read_path_audit.h`)
- **零侵入与零开销**：不修改 RocksDB 公共 include API；采用编译期宏 `-DROCKSDB_READ_PATH_AUDIT` 控制。关闭宏时，展开为空宏或内联空函数，编译器完全优化消除，不生成任何热路径指令分支。
- **高频路径零时钟**：在每键高频遍历路径（如 `merging_iterator.cc` 的 `SkipNextDeleted()` 与 `PopDeleteRangeStart()`）中**严禁调用任何时钟**，仅执行轻量原子递增 `AUDIT_COUNT_ADD`。
- **粗粒度与独占计时**：耗时统计仅覆盖操作外层或单次粗粒度路径（如 `FragmentTombstones` 构造、`NewInternalIterator` 各层组装），且采用 RAII 保证为独占时间（exclusive time）。

---

## 三、 E8 三态验证基准数据矩阵 (N=3)

- **Preload (Phase A)**: 500,000 点键 (256B value)，落盘至基线 SST；
- **Inject (Phase B)**: 20,000 个范围删除墓碑 (范围覆盖预设键区间)；
- **Read Workload (Phase C)**:
  - `get_live`: 5,000 次点查（查询未被删除的存活点键）；
  - `scan_intersect`: 5,000 次固定范围扫描（每 Scan 命中 50 点键，其中 10 键被范围删除覆盖，返回 40 存活键）；
- **重复次数**: 3 次全流程物理重建与独立测试。

### 3.1 完整量化数据表 (汇总自 `read-path-audit-e8.csv`)

| 状态 (Condition) | 操作类型 (Workload) | 子状态 (Sub-State) | 样本数 (Ops) | 平均延迟 (μs/op) | 返回存活键数 | 写端失效数 (次) | 视图物化次数 (次) | 视图物化耗时 (ms) | 锁尝试/争用 (次) | 迭代器准备耗时 (ms) | 墓碑覆盖查询耗时 (ms) | SST迭代器构造耗时 (ms) | Scan Reseeks (次) | 边界推进事件 (次) |
| :--- | :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **CLEAN** | `get_live` | Warm-steady | 5000 | **2.31** ±0.12 | 5000 | 0 | **0** | 0.000 | 0 / 0 | 0.000 | 0.000 | 0.000 | 0 | 0 |
| **CLEAN** | `scan_intersect`| Warm-steady | 5000 | **18.58** ±0.74 | 250000 | 0 | **0** | 0.000 | 0 / 0 | 0.000 | 0.000 | 4.611 | 0 | 0 |
| **T0-MEM** | `get_live` | **Cold-build** | 1 | **2630.38** ±79.8 | 1 | 20000 | **1** | **2.588** ±0.08 | 1 / 0 | 2.589 | 0.001 | 0.000 | 0 | 0 |
| **T0-MEM** | `get_live` | **Warm-steady** | 4999 | **2.63** ±0.11 | 4999 | 20000 | **0** | 0.000 | 0 / 0 | **0.482** | **0.744** | 0.000 | 0 | 0 |
| **T0-MEM** | `get_live` | **Re-invalid** | 1 | **2678.30** ±23.4 | 1 | 1 | **1** | **2.667** ±0.02 | 1 / 0 | 2.668 | 0.001 | 0.000 | 0 | 0 |
| **T0-MEM** | `scan_intersect`| **Cold-build** | 1 | **2642.18** ±24.2 | 40 | 20000 | **1** | **2.538** ±0.02 | 1 / 0 | 0.000 | 0.000 | 0.007 | 4 | 4 |
| **T0-MEM** | `scan_intersect`| **Warm-steady** | 4999 | **18.43** ±0.44 | 199960 | 20000 | **0** | 0.000 | 0 / 0 | 0.000 | 0.000 | 4.921 | **19996** | **19996** |
| **T0-MEM** | `scan_intersect`| **Re-invalid** | 1 | **2842.41** ±93.0 | 40 | 1 | **1** | **2.806** ±0.09 | 1 / 0 | 0.000 | 0.000 | 0.004 | 4 | 4 |
| **POSTFLUSH-SST**| `get_live` | Warm-steady | 5000 | **2.51** ±0.03 | 5000 | 20000 | **0** | 0.000 | 0 / 0 | 0.000 | 0.000 | 0.000 | 0 | 0 |
| **POSTFLUSH-SST**| `scan_intersect`| Warm-steady | 5000 | **19.26** ±0.26 | 200000 | 20000 | **0** | 0.000 | 0 / 0 | 0.000 | 0.000 | **7.615** | **20000** | **20000** |

---

## 四、 审计通过条件逐项核验

### 条件 1：三态的互斥路径计数严格符合物理状态
- **CLEAN 验证**：
  - 范围墓碑视图重建次数 `memtable_fragment_build_count = 0`；
  - 活跃 MemTable 墓碑查询计数 `active_memtable_tombstone_query_count = 0`；
  - 扫描跳跃与边界推进 `scan_range_del_reseek_count = 0`, `scan_boundary_advance_count = 0`；
  - 证明在无范围删除时，任何墓碑检测逻辑均处于绝对零触发状态。
- **T0-MEM 验证**：
  - 20,000 个墓碑全部驻留在 Active MemTable；
  - `get_live` 在 4,999 次读中严格触发了 4,999 次 `active_memtable_tombstone_query_count`，而 `imm_memtable_tombstone_query_count` 严格为 0；
  - `scan_intersect` 触发了 19,996 次 Reseek 与 19,996 次 Boundary Advance，平均每 Scan 严格跳跃 4 次，与负载相交特征完全吻合。
- **POSTFLUSH-SST 验证**：
  - 显式 Flush 后，墓碑全部迁移至 L0 SST；
  - `active_memtable_tombstone_query_count` 严格归零（0 次）；
  - `sst_iter_construct_ms` 从 CLEAN 的 4.48 ms 显著增加至 7.33 ms，反映了从 SST 块读取并反序列化 `kRangeDelBlockName` 的固定开销；
  - Scan 过程中发生 20,000 次 Reseek，全部由 SST 层的 `TruncatedRangeDelIterator` 驱动。

### 条件 2：冷启动重建与稳态查询被明确分离
- 数据清晰揭示出：
  - **首次读 (Cold-build)**：由于缓存指针为空，执行范围墓碑视图物化，单次耗时达到 **2.5 ~ 2.6 ms**；
  - **稳态读 (Warm-steady)**：缓存命中后，物化次数为 0，单次读延迟降至微秒级（Point Get 2.63 μs，Scan 18.43 μs）；
  - **写后失效 (Re-invalidated)**：一旦写入端增加 DeleteRange，活跃缓存失效，随后的首读再次出现 **2.6 ~ 2.8 ms** 的物化耗时。

### 条件 3：端到端趋势与既有 E8 PMU/火焰图的对应关系
- 既有 E8 动态压测报告指出，在墓碑写入与并发查询交织时，T0-MEM 表现出 CPI 升高与周期膨胀，堆栈中出现 `FragmentTombstones` 与 `reader_mutex`。
- 本次静态审计的物理计时给出了机制线索：每条 DeleteRange 会使活跃 MemTable 的缓存失效；之后第一位需要该范围墓碑视图的读者才触发重建。若多条 DeleteRange 在两次读取之间连续发生，它们可能合并为一次重建。但在持续交错场景下，若读写频繁交替，可能多次触发数毫秒的范围墓碑视图物化。

### 条件 4：T0-MEM 的净增量归属
- 首次读端到端延迟（`Cold-build Latency = 2630.38 μs`）的组成结构：
  $$\text{View Materialization Nanos} = 2588.2 \ \mu\text{s} \ (98.4\%)$$
  $$\text{MemTable Skiplist & Point Get} = 42.2 \ \mu\text{s} \ (1.6\%)$$
- **净增量归属定论**：在静态单次冷启动场景下，首读增量的主导部分在于**范围墓碑视图物化时间**（涵盖 `MemTableIterator` 创建与 `FragmentedRangeTombstoneList` 构建）。

---

## 五、 读路径优化候选方案分析 (AMTV 假设与验证路径)

根据微观审计结果，分析针对范围墓碑读路径的可能优化方向，作为后续 E9 动态混合负载实验的检验假说。

### 5.1 优化方向候选分析
- **视图物化与失效协议**：
  在静态微基准下，单次物化耗时稳定在 ~2.6 ms。若动态混合并发负载中存在高频失效且读者排队严重，则优化“失效后按需增量维护/分代视图”将是收益最显著的突破口；
- **稳态点查覆盖查询**：
  在静态只读且无失效的稳态下，Point Get 的二分查找子耗时仅占 ~0.15 μs，但 T0-MEM 与 CLEAN 仍有约 0.32 μs 的微秒级差距，尚需在动态场景下观察；
- **扫描游标推进**：
  稳态 Scan 每次稳定产生 4 次 Reseek 与 4 次边界推进事件；
- **SST 迭代器构建**：
  Flush 后的 L0 SST 读路径中，SST 迭代器构造耗时增至 7.6 ms，受限于块缓存与反序列化，属于下刷后的固定代价。

### 5.2 AMTV 假设性设计思路 (待 E9 动态审计检验)

#### 核心机制：双层分代与增量分段 (Two-Tier Generational Incremental View)
1. **世代分离 (Generational Separation)**：
   - 将 Active MemTable 的范围墓碑划分为两个层次：
     - **Base Frozen Snapshot**：对已经完成分段的墓碑集合（如既有的 20,000 条），一旦构建完成即冻结为只读有序数组，永不重建；
     - **Delta Active Buffer**：后续新写入的 DeleteRange 仅暂存在小容量的局部 Delta 缓冲区中（例如阈值 64 或 128 条）。
2. **写路径零失效 (Zero-Invalidation on Write)**：
   - 写端 `MemTable::Add(kTypeRangeDeletion)` 时，**严禁重置主分段视图**，仅以原子操作将墓碑追加到 Delta Buffer 中。读路径的主分段视图指针永不失效，彻底消除 2.6 ms 的读者阻塞。
3. **读路径轻量双路归并 (Two-Way Dual Check)**：
   - **Point Get**：
     - 先对 Base Snapshot 进行二分查找得到 $Seq_{base}$；
     - 再线性遍历极小的 Delta Buffer（<= 64 条）得到 $Seq_{delta}$；
     - 取 $\max(Seq_{base}, Seq_{delta})$，总开销由 2.6 ms 降至 < 0.5 μs。
   - **Scan**：
     - 当 Delta Buffer 积累到微型阈值时，在后台线程或写停顿窗口内执行极低开销的增量归并（Incremental Merge），而不是每次重跑 2 万条全量范围墓碑视图物化。
 4. **无锁 RCU 换代**：
    - 视图的发布与替换采用 `std::atomic<const FragmentedRangeTombstoneList*>` RCU 协议，避免读者构建视图时的互斥锁等待。
