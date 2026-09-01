# A0：只读一致性与统计口径审计报告 (notes/a0-final-integrity-audit.md)

**审计时间**：2026-08-20  
**审计目标**：全面复核现有实验代码、统计指标口径、驱动加载机制与表述边界，建立严谨的学术论述规范，消除潜在表述歧义与口径混淆。

---

## 一、E1 终态状态审计与溯源

### 1. 实测终态数据清单

| 实验组别 | 运行模式 | 存活 Key 数量 | 逻辑 Payload 大小 | 全库 SHA-256 校验和 | 重复一致性 (PASS) |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **Span 10** | **Dynamic** | 511,983 | 132.81 MB | `d7369ba89c5ab04235bc242dc0f774d6f58ddf3fdca76ef447e64a1c861b2762` | **PASS (3/3 严格一致)** |
| **Span 10** | **Static** | 511,983 | 132.81 MB | `d7369ba89c5ab04235bc242dc0f774d6f58ddf3fdca76ef447e64a1c861b2762` | **PASS (3/3 严格一致)** |
| **Span 100** | **Dynamic** | 511,983 | 132.81 MB | `d7369ba89c5ab04235bc242dc0f774d6f58ddf3fdca76ef447e64a1c861b2762` | **PASS (3/3 严格一致)** |
| **Span 100** | **Static** | 511,983 | 132.81 MB | `d7369ba89c5ab04235bc242dc0f774d6f58ddf3fdca76ef447e64a1c861b2762` | **PASS (3/3 严格一致)** |
| **Span 400** | **Dynamic** | 511,983 | 132.81 MB | `d7369ba89c5ab04235bc242dc0f774d6f58ddf3fdca76ef447e64a1c861b2762` | **PASS (3/3 严格一致)** |
| **Span 400** | **Static** | 511,983 | 132.81 MB | `d7369ba89c5ab04235bc242dc0f774d6f58ddf3fdca76ef447e64a1c861b2762` | **PASS (3/3 严格一致)** |

### 2. 原因溯源与机制解释
- **SHA 计算范围**：驱动中的全库 SHA-256 校验和是在实验完成后，通过全库无缓存正向迭代遍历，将所有可见存活 Key 与对应 Value 拼接后计算的完整 256 位哈希。
- **原因溯源**：查验 `tools/thesis_validation/tv_config.h` 第 21 行与 `tv_driver.cc` 第 36 行可知，`tv_config.h` 中 `trace_path` 默认赋值为 `./traces/p7/p7_trace_ratio_100.bin`。由于 `configs/e1/*.ini` 中未显式指定 `trace_path=""`，驱动在启动时复用了 P7 标准静态轨迹，导致 Span 10/100/400 实际执行了相同的轨迹。
- **一致性定义澄清**：
  - “PASS”判定严格表示**同一实验配置在 3 次独立重复运行之间达成逐比特状态一致**；
  - 严禁使用“跨配置逐比特一致”等过度推论；跨配置的差异性必须基于独立生成的 trace 轨迹进行评估。

---

## 二、写放大（Write Amplification）统计口径修订

为避免对写放大概念的理解混淆，正式将统计口径拆分为物理层与 Compaction 层的双重度量：

### 1. 后台 Compaction 写放大（CWA）
$$\text{CWA} = \frac{\text{Compaction 写入总字节数}}{\text{前台逻辑写入总字节数}}$$
- **物理含义**：度量后台为了维护 LSM 分层结构有序性与回收过期空间所额外付出的数据重写倍数。
- **说明**：在 Default 组（$T=0$）中，由于未触发任何 Compaction，$\text{CWA} = 0.0$。这仅代表**没有发生后台 Compaction 写入**，不能表述为“整个系统的物理写放大为 0”。

### 2. 总物理写放大（PWA）
$$\text{PWA} = \frac{\text{Flush 写入总字节数} + \text{Compaction 写入总字节数}}{\text{前台逻辑写入总字节数}}$$
- **物理含义**：度量从前台写入到达 LSM-tree 到最终持久化至存储介质的全链路真实物理写入放大倍数。

---

## 三、E1、E2、E4 核心表述修订

为保证学术论证的严密性，对前期报告中的部分过强表述进行规范化修订：

1. **E2 结论表述修订**：
   - **原表述（不准确）**：“小 Value 场景特有的范围删除退化”。
   - **修订后表述**：“无论 Value 大小为 64B 还是 4KiB，当普通 Put 的累计物理写入量不足以跨越活跃 MemTable 的 64MB 自然 Flush 阈值时，范围墓碑将持续滞留于内存，从而引发高密度墓碑下的读尾延迟严重恶化。”
2. **E4 结论表述修订**：
   - **原表述（过度因果推论）**：“Flush 即时消除了 SkipList 墓碑线性比对”。
   - **修订后表述**：“Flush 触发后读性能恢复与活跃 MemTable 墓碑清空、后续自动 Compaction 触发以及失效 Key 的物理丢弃在时间演化上具有强相关性。”
3. **E1 结论表述修订**：
   - **原表述（不准确）**：“固定计数阈值无法表示读压力”。
   - **修订后表述**：“固定墓碑计数（Count）无法反映范围删除所覆盖的逻辑删除规模和潜在空间回收价值。”

---

## 四、已用统计项与 RocksDB 源码/Ticker 完整映射表

| 统计指标名称 | 驱动变量名 | RocksDB 采集源 (Ticker / Property) | 源码位置 / 属性含义 |
| :--- | :--- | :--- | :--- |
| **Flush 次数** | `flush_count_total` | `rocksdb.num-flushes-running` + Ticker `FLUSH_WRITE_BYTES` 增量检测 | `db/db_impl/db_impl_compaction_flush.cc` |
| **Flush 原因** | `flush_reason_*` | Tickers: <br>- `FLUSH_REASON_MEMTABLE_FULL`<br>- `FLUSH_REASON_MEMTABLE_MAX_RANGE_DELETIONS`<br>- `FLUSH_REASON_MANUAL` | `include/rocksdb/statistics.h:630-655` |
| **Flush 写入字节** | `flush_write_mb` | Ticker: `FLUSH_WRITE_BYTES` | 累计写入 SST 的物理 Flush 字节 |
| **Compaction 读字节** | `compaction_read_mb`| Ticker: `COMPACTION_READ_BYTES` | Compaction 阶段读取的 SST 字节 |
| **Compaction 写字节** | `compaction_write_mb`| Ticker: `COMPACTION_WRITE_BYTES` | Compaction 阶段重写的 SST 字节 |
| **墓碑丢弃键数** | `compaction_drop_keys` | Ticker: `COMPACTION_KEY_DROP_RANGE_DEL` | 在 Compaction 中被范围墓碑裁剪丢弃的键总数 |
| **活跃 MemTable 大小** | `active_memtable_bytes`| Property: `rocksdb.cur-size-active-mem-table` | `db/internal_stats.cc:113` |
| **范围墓碑数量** | `tombstones_count` | `MemTable::num_range_deletions()` / Ticker `NUMBER_OF_RESCUED_FOR_REMOVE_RANGE_TOMBSTONE` | `db/memtable.cc:301` |
| **L0 文件数** | `l0_files_final` | Property: `rocksdb.num-files-at-level0` | Level 0 当前包含的 SST 文件数 |
| **SST 总大小** | `total_sst_mb` | Property: `rocksdb.total-sst-files-size` | 全库所有 Level SST 文件物理占用大小 |
| **写停顿耗时** | `stall_micros` | Ticker: `STALL_MICROS` | 前台写入因 MemTable/L0 积压而阻塞等待的微秒数 |
| **块缓存命中率** | `block_cache_hit_rate` | Tickers: `BLOCK_CACHE_HIT` / (`BLOCK_CACHE_HIT` + `BLOCK_CACHE_MISS`) | 块缓存命中概率 |
