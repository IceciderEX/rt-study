# P9：RocksDB v11.8.0 范围删除与 Flush 机制只读源码核验报告 (notes/p9-native-feature-audit.md)

**核验生成时间**：2026-08-20  
**核验目标源码树**：`/home/wam/grad/rocksdb-v11.8.0`  
**Git Tag 与 Commit**：纯净官方 Tag `v11.8.0`（commit `abeebd9630f11bd08c28b7bd43c7bdfc62050654`）  
**核验目的**：严格确认 RocksDB v11.8.0 中与范围墓碑触发 Flush、读路径墓碑感知、Compaction 补偿及相关统计 Ticker 的原生支持能力与参数边界，严禁假设不存在的特性。

---

## 一、核心选项与机制只读核验清单

### 1. `memtable_max_range_deletions`（原生固定墓碑数量 Flush 阈值）

| 属性项 | 源码核验结果 |
| :--- | :--- |
| **是否存在** | **存在** (完全原生支持) |
| **源码声明路径** | `include/rocksdb/options.h` (第 357 行)<br>`options/cf_options.h` (第 379 行)<br>`db/memtable.h` (第 981 行) |
| **默认值** | `0`（表示**关闭 / disabled**） |
| **核心执行逻辑路径** | `db/memtable.cc` (第 231、301~305、370~372 行) |
| **执行语义与触发机理** | 当写入 `DeleteRange` 时，MemTable 内部的 `range_del_table_` 计数器递增。若 `memtable_max_range_deletions_ > 0` 且 `range_del_table_->Count() > memtable_max_range_deletions_`，直接触发 `MarkFlushPending()`，将活跃 MemTable 冻结为 Immutable 并触发异步/同步 Flush，Flush 原因是 `FlushReason::kMemTableMaxRangeDeletions`。 |
| **动态 SetOptions 支持** | **支持** (`options/cf_options.cc` 第 762~763 行将其注册于 `MutableCFOptions`，支持在运行时通过 `db->SetOptions` 修改)。 |
| **在 E2/E3/E4 中的角色** | **核心可运行基准选项**。通过配置 `memtable_max_range_deletions = 64/128/256/512/1024/2048`，可完全通过原生参数进行固定数量阈值压测，无需任何外围脚本或人工注入。 |

---

### 2. `memtable_op_scan_flush_trigger` 与 `memtable_avg_op_scan_flush_trigger`（读路径隐式扫描触发 Flush）

| 属性项 | 源码核验结果 |
| :--- | :--- |
| **是否存在** | **存在** (原生支持) |
| **源码声明路径** | `include/rocksdb/advanced_options.h` (第 1383、1393 行)<br>`db/db_iter.h` (第 624~643 行)<br>`db/db_iter.cc` (第 103、154~162 行) |
| **默认值** | `0`（关闭） |
| **执行语义与触发机理** | 在 Iterator 读路径（Seek / Next）扫描 MemTable 时，若跳过/过滤掉的隐藏操作（如被删除或覆盖的键）单次达到 `memtable_op_scan_flush_trigger`，或在滑动窗口内的平均跳过数达到 `memtable_avg_op_scan_flush_trigger`，迭代器会主动调用 `MarkFlushPending()`，以 `FlushReason::kMemTableOpScanFlushTrigger` 触发刷盘。 |
| **动态 SetOptions 支持** | **支持** (`options/cf_options.cc` 第 766~771 行已注册于 `MutableCFOptions`)。 |
| **在当前研究中的角色** | 属于读路径墓碑累积触发的启发式机制，可作为后续讨论与对比参考。 |

---

### 3. `compensated_range_deletion_size`（Compaction 范围删除空间补偿）

| 属性项 | 源码核验结果 |
| :--- | :--- |
| **是否存在** | **存在** (内部机制，非用户直接配置项) |
| **源码声明路径** | `db/version_edit.h` (第 272、356 行)<br>`db/builder.cc` (第 355 行)<br>`db/compaction/compaction_outputs.cc` (第 756 行)<br>`db/version_set.cc` (第 3328 行) |
| **默认值** | `0`（随 SST 文件元数据动态计算） |
| **执行语义与触发机理** | 当 Flush 或 Compaction 生成包含范围墓碑的 SST 文件时，RocksDB 会调用 `ApproximateSize` 估算该墓碑在底层（下层 Level）覆盖的数据体积，并累加到该 SST 文件的 `compensated_range_deletion_size` 与 `compensated_file_size` 中。这会人为放大该文件在 LevelCompaction 中的得分（Score），从而促使后台更早调度包含范围墓碑的 SST 与底层进行合并，加速物理空间回收。 |
| **动态 SetOptions 支持** | **不适用 (N/A)**：这是 SST 内部元数据与 Compaction 打分机制，非 Options 参数。 |
| **在当前研究中的角色** | 解释了为什么范围墓碑被 Flush 刷入 SST 后能够自动触发 Compaction 并有效缩减总 SST 大小。 |

---

### 4. 统计 Ticker 与 Property 审计

| 观测项 | 对应原生标识符 / Ticker | 源码位置与语义 |
| :--- | :--- | :--- |
| **墓碑 Flush 计数** | `MEMTABLE_MAX_RANGE_DELETION_FLUSH_COUNT`<br>(`rocksdb.flush.reason.memtable_max_range_deletions`) | `monitoring/statistics.cc:328`<br>精确统计因达到 `memtable_max_range_deletions` 阈值而触发的 Flush 次数。 |
| **墓碑物理丢弃键数** | `COMPACTION_KEY_DROP_RANGE_DEL`<br>(`rocksdb.compaction.key.drop.range_del`) | `monitoring/statistics.cc:184`<br>精确统计在 Compaction 过程中被范围墓碑覆盖并被物理丢弃的过期 Key 数量。 |
| **过期墓碑丢弃数** | `COMPACTION_RANGE_DEL_DROP_OBSOLETE`<br>(`rocksdb.compaction.range_del.drop.obsolete`) | `monitoring/statistics.cc:186`<br>统计在 Compaction 合并到最底层（Bottommost Level）后被彻底丢弃的无效范围墓碑数。 |
| **活跃 MemTable 墓碑数** | 原生 Property 无直接墓碑暴露接口 | 经查验，RocksDB 原生 Property 暴露了 `rocksdb.num-deletes-active-mem-table` 与 `rocksdb.cur-size-active-mem-table`，但范围墓碑数量未单独提供 String Property。**驱动层必须自维护精确的 `cumulative_range_deletions` 计数器**。 |

---

## 二、准入结论与能力边界判定

1. **核心选项准入确认**：
   - 官方 RocksDB `v11.8.0` 原生完全支持 `memtable_max_range_deletions`，默认值为 `0`（关闭）；
   - 该选项完全由 C++ 原生引擎在 `db/memtable.cc` 中基于 `range_del_table_->Count()` 判定触发，与本研究计划完全契合；
   - **无需任何源码修改，无需脚本外部模拟，实验可 100% 运行在纯净官方 RocksDB v11.8.0 上**。

2. **边界与纪律声明**：
   - 本轮 E1 ~ E4 实验均使用纯原生 `memtable_max_range_deletions` 进行参数扫描；
   - 活跃 MemTable 内部的墓碑数由驱动程序做确定性统计；
   - 所有性能数据与 Ticker 均有确切源码语义映射。

---

> [!NOTE]
> **当前状态**：P0 只读源码与环境核验已全部完成，证实 `memtable_max_range_deletions` 等核心原生参数完全可用。已准备好进入下一阶段（E2 Value 大小失配 Sanity 试跑）。
