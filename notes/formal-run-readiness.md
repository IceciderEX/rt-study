# 阶段 D：正式实验准入与冻结方案报告 (notes/formal-run-readiness.md)

**生成时间**：2026-08-20  
**实验机器节点**：`s14.servers.hustpdsl.cn`  
**实验项目根目录**：`/home/wam/grad/s14-range-delete-study/`  
**源码与驱动基准**：RocksDB `v11.8.0` (commit `abeebd9630f11bd08c28b7bd43c7bdfc62050654`, Release `-O2`)  
**当前状态**：**已冻结全部配置与脚本，严禁在未获明确授权前触发正式写入运行**。

---

## 1. 实验目标与研究假设

### 1.1 核心目标
本阶段目标为证明：**“原生 RocksDB DeleteRange 在混合查询负载下的性能退化与写入/空间放大问题是否真实存在且具有研究价值”**。  
本阶段**坚决不实现任何优化机制**（包括但不限于新索引、墓碑过滤器、Compaction 调度修改或缓存优化）。

### 1.2 观测的核心机制假设
1. **范围墓碑对 Get 的隐式放大**：即使点查命中存活 Key，遍历墓碑边界与多层合并迭代器是否引入不可忽视的长尾延迟？
2. **范围墓碑对 RangeScan 的虚假加速与真实开销**：随着范围删除增加，Scan 实际返回键数急剧减少；若以总耗时衡量会造成“删除后 Scan 变快”的虚假假象，必须以**单位有效返回 Key 延迟**衡量真实的遍历开销。
3. **墓碑碎片化与重叠度的性能影响**：离散短墓碑与连续长墓碑在相同逻辑删除量下对 MemTable / SST 查找性能的影响差异。
4. **冷热局部性与回收干扰**：墓碑空间与查询热点的高度重合是否加剧前台读阻塞；受控 `CompactRange()` 物理清理墓碑时对前台 P99 造成的瞬时冲击与 WriteStall 放大。

---

## 2. 冻结实验矩阵与配置明细

全套正式实验由五组预实验构成（P1 ~ P5），每组每个条件**严格执行 3 次独立重复（Repetitions = 3）**，每次均使用独立新创建的数据库目录：

```text
/home/wam/grad/s14-range-delete-study/
├── configs/
│   ├── p1_range_del_ratio_0pct.ini ~ 10pct.ini (6个配置)
│   ├── p2_tombstone_short_fragmented.ini ~ long_overlapped.ini (4个配置)
│   ├── p3_point_heavy.ini & p3_scan_heavy.ini (2个配置)
│   ├── p4_locality_cold.ini ~ hot.ini (3个配置)
│   └── p5_compaction_high_heat.ini ~ control_none.ini (3个配置)
```

| 组别 | 实验名称 | 自变量与测试梯度 | 控制变量与固定参数 | 运行轮次 |
| :--- | :--- | :--- | :--- | :--- |
| **P1** | RangeDelete 比例敏感性 | DeleteRange 比例：<br>`0%`, `0.5%`, `1.0%`, `2.0%`, `5.0%`, `10.0%` | Key=500k, Val=256B, Ops=200k, 8线程<br>Put=5%, Uniform分布, DelLen=200, ScanLen=100 | 6 组 × 3 次 = **18 轮** |
| **P2** | 范围墓碑组织特征 | 4 种形态模式：<br>1. `short-fragmented` (Len=50, Overlap=0.0)<br>2. `long-contiguous` (Len=1000, Overlap=0.0)<br>3. `short-overlapped` (Len=50, Overlap=0.8)<br>4. `long-overlapped` (Len=1000, Overlap=0.8) | 累计删除 Key 预算一致 (~20% 空间)<br>Key=500k, Ops=200k, 8线程, Put=5%, DelRatio=2% | 4 组 × 3 次 = **12 轮** |
| **P3** | 读工作负载构成 | 读取构成对比（总读预算 80%）：<br>1. `point-heavy`: Get 75%, Scan 5%<br>2. `scan-heavy`: Get 10%, Scan 70% | Key=500k, Ops=200k, 8线程<br>Put=10%, DeleteRange=10%, DelLen=200 | 2 组 × 3 次 = **6 轮** |
| **P4** | 删除区间冷热局部性 | 查询热点与删除区间的空间重合度：<br>1. `cold`: 重合率 0.0 (删除区间完全位于冷区)<br>2. `medium`: 重合率 0.5 (部分重合)<br>3. `hot`: 重合率 1.0 (完全位于前 20% Zipf 热点) | Key=500k, Ops=200k, 8线程, Zipfian (θ=0.99)<br>Put=5%, Get=73%, Scan=20%, DelRatio=2% | 3 组 × 3 次 = **9 轮** |
| **P5** | 受控回收干扰压力测试 | 墓碑回收时机与区间：<br>1. `high-heat`: 50% 进度对热区触发一次 `CompactRange()`<br>2. `low-heat`: 50% 进度对冷区触发一次 `CompactRange()`<br>3. `control-none`: 纯原生后台，不触发 CompactRange | Key=500k, Ops=200k, 8线程, Zipfian (θ=0.99)<br>持续固定前台混合负载，记录前后瞬态冲击 | 3 组 × 3 次 = **9 轮** |

**全套实验总运行轮次**：$18 + 12 + 6 + 9 + 9 = \mathbf{54\text{ 轮}}$。

---

## 3. 资源开销、磁盘预算与耗时预估

### 3.1 单次运行开销
- **预置数据 (Preload)**：500,000 Keys × 256B = **128 MB** 原始数据。
- **前台工作负载**：200,000 Ops，其中写操作产生 WAL 约 10~30 MB。
- **单个数据库物理峰值占用**：< **500 MB**。
- **单次运行耗时估算**：Preload (~1.5s) + 200k Ops (~4~8s) + 离线全量对账 (~1.5s) $\approx$ **7 ~ 12 秒 / 轮**。

### 3.2 全套实验 (54 轮) 总预算
- **总逻辑数据写入量**：$54 \times (128\text{MB} + 20\text{MB}) \approx \mathbf{8.0\text{ GB}}$。
- **磁盘占用策略**：每轮运行在独立目录 `run-db/db_${exp_id}_rep${rep}` 中完成，归档原始 `LOG` 与度量数据后，脚本自动清理当前临时 DB，**活动磁盘峰值占用 < 2 GB**。
- **即使保留全部 54 轮 DB**：峰值物理占用 $\approx 54 \times 500\text{MB} = \mathbf{27\text{ GB}}$（当前可用 NVMe 空间为 **585 GB**，占用率 < 4.6%）。
- **全套实验总耗时预估**：$54 \times 10\text{s} + 间隔安全检查 \approx \mathbf{10 \sim 15\text{ 分钟}}$。

---

## 4. 统计指标与监控项全景映射

| 度量维度 | 具体统计指标名称 | 采集源 / API | 核心分析作用 |
| :--- | :--- | :--- | :--- |
| **Get 细分性能** | `get_affected_live_p50/p95/p99` | 驱动高精计时 (P50/P95/P99) | 验证受墓碑影响的有效键点查长尾退化 |
| | `get_deleted_p50/p95/p99` | 驱动高精计时 | 记录 NotFound 路径耗时 |
| | `get_control_p50/p95/p99` | 驱动高精计时 | 作为无墓碑干扰的纯净基线对照 |
| **RangeScan 性能**| `scan_avg_keys_returned` | 驱动迭代器返回计数 | 记录每次扫描实际返回键数，消除有效数据量差异 |
| | `scan_avg_span` | 驱动参数 | 记录设定的扫描键空间跨度 |
| | `scan_p50/p95/p99/mean` | 驱动高精计时 | 总体扫描延迟分布 |
| | `scan_us_per_key` | 归一化计算: $\frac{\text{Mean Latency}}{\text{Avg Returned Keys}}$ | **核心指标**：单位有效 Key 遍历代价，杜绝“空扫变快”误判 |
| | `scan_ops_sec`, `scan_keys_sec` | 驱动吞吐统计 | 扫描 QPS 与 键吞吐率 |
| **范围墓碑特征** | `tombstones_count` | `ReferenceModel` | 记录累计 DeleteRange 调用次数 |
| | `union_deleted_keys` | `ReferenceModel` | 墓碑区间并集覆盖的键总数 |
| | `union_coverage_ratio` | $\frac{\text{Union Deleted}}{\text{Total Keys}}$ | 键空间并集覆盖率 (%) |
| | `overlap_factor` | $\frac{\sum \text{Del Length}}{\text{Union Deleted}}$ | 墓碑重叠/冗余因子 |
| **RocksDB 引擎状态**| `compaction_read_mb` / `write_mb` | `Tickers::COMPACT_READ/WRITE_BYTES` | Compaction 读写放大度量 |
| | `flush_write_mb` | `Tickers::FLUSH_WRITE_BYTES` | MemTable Flush 写入量 |
| | `stall_micros` | `Tickers::STALL_MICROS` | 前台因 WriteStall 累计受阻微秒数 |
| | `compaction_drop_keys` | `Tickers::COMPACTION_KEY_DROP_RANGE_DEL` | Compaction 因范围墓碑物理丢弃的点键数 |
| | `l0_files_final` | `rocksdb.num-files-at-level0` | 最终 L0 SST 文件堆积数量 |
| | `pending_compact_mb` | `rocksdb.estimate-pending-compaction-bytes`| 积压待 Compaction 字节量 |
| | `total_sst_mb` | `rocksdb.total-sst-files-size` | SST 物理文件总占用大小 |
| **系统底层监控** | `%util`, `r/s`, `w/s`, `rkB/s`, `wkB/s` | `iostat -xz 1` 后台连续采样 | 记录 NVMe 设备级实时读写吞吐与 I/O 饱和度 |
| **正确性对账** | `sample_verified_ok/notfound` | 驱动随机抽样 10,000 Keys | 抽样点查 100% 吻合检验 |
| | `full_scan_ok` | 驱动全库 Iterator 对账 | 全库存活键数、逻辑字节数与有序性全量核验 |

---

## 5. 路径隔离与清理保护策略

1. **严格路径白名单**：
   - 数据库操作路径限定为：`/home/wam/grad/s14-range-delete-study/run-db/db_*`
   - 结果数据路径限定为：`/home/wam/grad/s14-range-delete-study/results/`
2. **清理隔离性原则**：
   - 清理指令仅匹配以 `${BASE_DIR}/run-db/db_` 开头的绝对路径；
   - 脚本执行前强制断言路径前缀；**严禁执行任何宽泛的 `rm -rf *` 或涉及 `/home/wam/grad/` 下其他目录的操作**；
   - 即使实验发生异常中断，清理动作仅限于当前未完成的单个 `db_*` 目录，绝对不影响历史数据与其他用户文件。

---

## 6. 八大严格停止条件 (熔断机制)

在执行过程中，若出现以下任一异常，调度脚本将立即终止并报错退出，严禁私自修改参数或重试掩盖问题：

1. **正确性校验失败**：抽样 10,000 Keys 或全量 Iterator 对账出现任何状态不符；
2. **参考模型不一致**：驱动内存参考状态与 RocksDB 内部行为出现偏离；
3. **指标缺失或异常**：关键统计项出现不可读、未更新或全 0 异常；
4. **存储位置违规**：实验路径脱离 NVMe 分区 (`/dev/nvme0n1p2`)；
5. **空间预算不足**：可用空间低于 50 GB 安全红线；
6. **外部高 I/O 干扰**：检测到底层背景 `%util > 10%` 或其他活跃写进程干扰；
7. **NotFound 严重倾斜造成不可比**：未分离 Get 类别导致混淆；
8. **RangeScan 键数误判**：未记录实际返回有效 Key 数导致将空扫误判为性能提升；
9. **参数污染**：需要私自调整固定配置或剔除数据才能得出差异。

---

## 7. 执行就绪声明

- [x] 源码环境与纯净 RocksDB v11.8.0 审计完成并生成报告
- [x] 原生轻量 C++ 负载驱动编写完成，支持三类 Get 与 RangeScan 归一化统计
- [x] 最小规模 SmokeTest 验证通过（100% 正确性对账，0 错误）
- [x] P1 ~ P5 共 18 个配置文件全部就绪并冻结
- [x] 单独运行脚本 `run_p1.sh` ~ `run_p5.sh` 与主调度脚本 `run_all_frozen.sh` 全部生成
- [x] 汇总分析与统计脚本 `aggregate_results.py` 就绪
- [x] **所有实验代码与正式运行处于冻结状态，等待您的最终批准指示**。
