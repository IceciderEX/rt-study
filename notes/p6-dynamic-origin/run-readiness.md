# P6：动态范围删除来源定位实验准入与冻结报告 (notes/p6-dynamic-origin/run-readiness.md)

**生成时间**：2026-08-20  
**实验机器节点**：`s14.servers.hustpdsl.cn` (80 vCPUs, 78GB RAM)  
**底层存储介质**：`/dev/nvme0n1p2` (KIOXIA EXCERIA PRO 1.8TB NVMe SSD, ext4)  
**引擎基准版本**：官方纯净 RocksDB `v11.8.0` (commit `abeebd9630f11bd08c28b7bd43c7bdfc62050654`, Release `-O2`)  
**当前状态**：**已完成全部请求流固化、驱动编译、配置生成与准入核验，处于严格冻结状态，等待您的确认指令后再行启动正式执行**。

---

## 一、核心研究问题与对比设计

### 1. 核心科学问题
> 在相近逻辑终态（全库仅剩 600,000 个存活键 $L$）和相同前台读写配置（Get 75%, Scan 20%, Put 5%）下，**持续执行 DeleteRange 是否比“预先完成范围删除、仅保留静态落盘墓碑”额外恶化性能**？若恶化，其时间上是否伴随 MemTable、L0 文件数、Flush、Compaction 或范围墓碑维护状态的变化？

### 2. 实验三组设计 (D0 vs D1 vs D2)
| 组别代号 | 实验组名称 | 预处理阶段 | 测量阶段工作负载 | 预期逻辑终态与校验 |
| :--- | :--- | :--- | :--- | :--- |
| **D0** | **CleanFinalState**<br>(无墓碑基准组) | 仅加载存活键集合 $L$ (600,000 Keys) $\rightarrow$ Flush 到 SST | 纯净重放固定前台请求流（Get 75%, Scan 20%, Put 5%），无 DeleteRange | 逻辑终态为 $L$ (600,000 Keys)，计算全库 Key-Value SHA-256 |
| **D1** | **StaticTombstoneState**<br>(静态墓碑对照组) | 加载全部 1,000,000 Keys $\rightarrow$ Flush $\rightarrow$ 执行 4,000 个 DeleteRange (覆盖 $U$) $\rightarrow$ Flush 固化墓碑 | 执行与 D0 完全相同的固定前台请求流，无新的 DeleteRange | 逻辑终态与 D0 逐键逐值完全一致，SHA-256 校验 100% 一致 |
| **D2** | **DynamicDeleteRangeState**<br>(动态删除测试组) | 加载全部 1,000,000 Keys $\rightarrow$ Flush (不预先执行删除) | 在前台请求流执行过程中，**在 Main 阶段前 40 秒按 100 ops/s 均匀注入 4,000 个 DeleteRange** | 删除完成后逻辑终态与 D0、D1 逐键逐值完全一致，SHA-256 100% 一致 |

---

## 二、请求流与文件 SHA-256 固化清单

为确保绝对的可重复性与学术严密性，所有删除操作与前台读写请求均预先生成为二进制 Trace 文件并固化校验码：

| 文件名称 | 路径 | 规格与规模 | SHA-256 Checksum |
| :--- | :--- | :--- | :--- |
| **DeleteRange 文本脚本** | `traces/p6/deleterange_trace.txt` | 4,000 行范围区间 (`[b, e)`)，覆盖 400,000 键 | `fb681cff4108dafe80177701d356d09cf188351eab8297637598bd33fd425747` |
| **DeleteRange 二进制脚本** | `traces/p6/deleterange_trace.bin` | 4,000 个紧凑结构体 (`<uint64_t, uint64_t>`) | `5dbaaf40167aabb23ccc689c991eeac9eee207518aafe16cb5cb8f359a95c5a7` |
| **前台请求流二进制脚本** | `traces/p6/frontend_ops_trace.bin` | 2,000,000 个确定性请求 (75% Get, 20% Scan, 5% Put) | `3353651fdc55af8b4c43ff6acf12fa20d3a0777440a98dbc8258412cdbc9a14e` |
| **专用驱动二进制程序** | `bin/p6_driver` | C++20 Release 编译 (`-O2 -std=c++20`) | `5949f3f404321f93b7ee063d734fdefca5553f90126330e488c4c8cce76db1cd` |

---

## 三、资源预算、执行时序与耗时估算

### 3.1 写入量与磁盘空间预算
- **D0 单轮逻辑写入**：Preload 600 MB + 前台 Put ~100 MB $\approx$ 0.7 GB
- **D1 单轮逻辑写入**：Preload 1.0 GB + 4k DeleteRange + 前台 Put ~100 MB $\approx$ 1.1 GB
- **D2 单轮逻辑写入**：Preload 1.0 GB + 4k DeleteRange + 前台 Put ~100 MB $\approx$ 1.1 GB
- **9 轮总逻辑写入量**：$3 \times 0.7 + 3 \times 1.1 + 3 \times 1.1 = \mathbf{8.7\text{ GB}}$
- **磁盘占用峰值**：每次单轮运行占用 $\le \mathbf{2.5\text{ GB}}$（单轮运行后立即全量对账并清理，磁盘可用空间 **585 GB**，余量充裕率 > 99%）。

### 3.2 时间结构与耗时预估
- **单轮运行结构**：
  - 数据预置（Preload 1GB + Flush）：~6 秒
  - **Warm-up（预热阶段）**：**10 秒**（预热 Cache 与线程，不计入主统计）
  - **Main（正式测量阶段）**：**60 秒**（D2 在前 40 秒均匀注入 4k DeleteRange）
  - **Cool-down（冷却收敛阶段）**：**10 秒**（记录后台收敛）
  - 离线全量一致性与 SHA-256 计算：~3 秒
  - 单轮总计：$\approx \mathbf{90\text{ 秒}}$ ($\approx 1.5\text{ 分钟}$)
- **9 轮正式运行总耗时**：$9 \times 1.5\text{ min} \approx \mathbf{13 \sim 15\text{ 分钟}}$。

---

## 四、1 秒级高精采样指标全景清单

驱动在 80 秒运行期间每秒同步捕获以下三大维度指标：

1. **前台读写细分度量**：
   - `get_ctrl_ops`, `get_ctrl_p50/p95/p99` (远离墓碑的基准点查)
   - `get_aff_ops`, `get_aff_p50/p95/p99` (靠近墓碑边界的受损区点查)
   - `scan_ops`, `scan_p50/p95/p99`, `scan_avg_keys`, `scan_us_per_key` (**核心归一化扫描开销**)
   - `put_ops`, `put_p50/p95/p99` (存活键就地更新)
   - `del_range_ops`, `del_range_p99` (动态范围删除耗时)
   - `overall_iops` (整机吞吐)
2. **RocksDB 内部状态与 Ticker**：
   - `active_memtable_mb` (`rocksdb.cur-size-active-mem-table`)
   - `immutable_memtable_count` (`rocksdb.num-immutable-mem-table`)
   - `memtable_flush_pending` (`rocksdb.mem-table-flush-pending`)
   - `l0_files` (`rocksdb.num-files-at-level0`)
   - `pending_compaction_mb` (`rocksdb.estimate-pending-compaction-bytes`)
   - `running_flushes` / `running_compactions`
   - `compaction_read_mb_delta` / `compaction_write_mb_delta` / `flush_write_mb_delta`
   - `write_stall_micros_delta` (写停顿微秒增量)
   - `block_cache_hit_rate` (BlockCache 命中率)
3. **系统级状态**：
   - `db_dir_mb` (数据库物理目录落盘大小)
   - `iostat -xz 1` 系统磁盘利用率与队列深度

---

## 五、严格停止与熔断条件

若在正式运行期间出现以下任一情况，脚本将立即中断并报警：
1. **状态摘要不一致**：D0、D1、D2 全库遍历的存活键数不等于 600,000，或终态 SHA-256 校验和在 D0/D1/D2 间不完全一致；
2. **Key/Value 损坏**：10,000 随机点查抽样发现任何 NotFound 或值错误；
3. **Trace 偏离**：D2 未能完整、按序执行全部 4,000 个预定 DeleteRange；
4. **路径异常**：数据库脱离 `/dev/nvme0n1p2` NVMe 分区；
5. **空间红线**：磁盘余量 $< 50\text{ GB}$。

---

## 六、执行准入声明

- [x] Trace 脚本与固化 Checksum 已生成（`traces/p6/`）
- [x] P6 专用驱动程序已编译完成并通过校验（`bin/p6_driver`）
- [x] D0、D1、D2 全部 3 个冻结配置文件已就绪（`configs/p6-dynamic-origin/`）
- [x] 自动化测试与聚合脚本已生成（`scripts/p6-dynamic-origin/`）
- [x] **所有 P6 实验代码、配置与环境已处于冻结状态，等待您的确认指令后再行启动正式执行**。
