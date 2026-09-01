# P7：P1高退化现象强度核验与可观测状态复现实验准入报告 (notes/p7-p1-replay/run-readiness.md)

**生成时间**：2026-08-20  
**实验机器节点**：`s14.servers.hustpdsl.cn` (80 vCPUs, 78GB RAM)  
**底层存储介质**：`/dev/nvme0n1p2` (KIOXIA EXCERIA PRO 1.8TB NVMe SSD, ext4)  
**RocksDB 版本**：纯净官方 Tag `v11.8.0` (commit `abeebd9630f11bd08c28b7bd43c7bdfc62050654`, Release `-O2`)  
**当前状态**：**阶段A审计已完成，所有 Trace 请求流、P7 专属双时间轴驱动、冻结配置文件与调度脚本均已就绪，处于完全冻结状态，等待您的确认指令后再行启动 18 轮正式执行**。

---

## 一、阶段A 审计结论与协议修正

阶段A完整审计报告已生成至：[notes/p7-p1-replay/p1-p6-protocol-audit.md](file:///home/wam/grad/s14-range-delete-study/notes/p7-p1-replay/p1-p6-protocol-audit.md) 及 [results/summary/p7-p1-replay/p1-p6-protocol-audit.csv](file:///home/wam/grad/s14-range-delete-study/results/summary/p7-p1-replay/p1-p6-protocol-audit.csv)。

### 1.1 核心审计事实
1. **P1 的“DeleteRange 比例”**：严格定义为**逻辑请求序列中的操作概率/比例（Logical Operation Ratio）**。在 10% 组中，200,000 次操作中严格包含了 **19,873 次 DeleteRange（占 9.94%）**。
2. **P6 D2 的真实占比**：4,000 次 DeleteRange 是以固定 100 ops/s 注入，在 988 万次总操作中真实占比仅为 **0.040%**。P1 10% 组的删除密度是 P6 D2 的近 **250 倍**。
3. **P1 与 P6 的不可比性**：P1 无 Compaction 吸收（0 MB Compaction Write，墓碑直接在活跃 MemTable/L0 堆积）；P6 有 1,170 MB Compaction 吸收（墓碑被合并清理）。二者在数据量（128MB vs 1GB）、注入机制（前台紧凑循环 vs 独立限速注入）上存在本质差异。

---

## 二、原 P1 与 P7 实验参数逐项一致性对照表

| 配置参数项 | 原 P1 冻结参数 | P7 实验参数 | 一致性状态 |
| :--- | :--- | :--- | :---: |
| **初始 Key 数量** | 500,000 | 500,000 | **100% 完全一致** |
| **Value 大小** | 256 Bytes | 256 Bytes | **100% 完全一致** |
| **总逻辑操作数** | 200,000 | 200,000 | **100% 完全一致** |
| **并发工作线程数** | 8 | 8 | **100% 完全一致** |
| **Key 编码格式** | `key_%012llu` (16 Bytes) | `key_%012llu` (16 Bytes) | **100% 完全一致** |
| **Key 访问分布** | Uniform (0 ~ 499,999) | Uniform (0 ~ 499,999) | **100% 完全一致** |
| **前台 Put 比例** | 5.0% (10,000 次) | 5.0% (10,000 次) | **100% 完全一致** |
| **前台 Scan 比例与跨度** | 20.0% (40,000 次), Span=100 | 20.0% (40,000 次), Span=100 | **100% 完全一致** |
| **DeleteRange 单次长度** | 100 Keys (`[b, b+100)`) | 100 Keys (`[b, b+100)`) | **100% 完全一致** |
| **BlockCache 大小** | 128 MB (134,217,728 B) | 128 MB (134,217,728 B) | **100% 完全一致** |
| **MemTable 大小与数量** | 64 MB, Max Buffer Number = 4 | 64 MB, Max Buffer Number = 4 | **100% 完全一致** |
| **L0 Compaction 触发阈值** | 4 Files | 4 Files | **100% 完全一致** |
| **后台线程总数** | 8 Threads | 8 Threads | **100% 完全一致** |
| **数据库预置流程** | 插入 500k 键 $\rightarrow$ Flush 固化 | 插入 500k 键 $\rightarrow$ Flush 固化 | **100% 完全一致** |

---

## 三、请求流 Trace 文件与驱动二进制 SHA-256 固化清单

| 文件名称 | 路径 | 规格与操作构成 | SHA-256 Checksum |
| :--- | :--- | :--- | :--- |
| **0.0% Trace** | `traces/p7/p7_trace_ratio_000.bin` | 200k Ops (Del: 0, Get: 150k, Scan: 40k, Put: 10k) | `6ed355378b06ffc9c13561668471b2765863b5bf66913f4047f63f72d94c7a54` |
| **0.5% Trace** | `traces/p7/p7_trace_ratio_005.bin` | 200k Ops (Del: 1k, Get: 149k, Scan: 40k, Put: 10k) | `b434b7e1662d83d0e20b868ed094c2f1b2d0265ee7e17895eb8abc585420336a` |
| **1.0% Trace** | `traces/p7/p7_trace_ratio_010.bin` | 200k Ops (Del: 2k, Get: 148k, Scan: 40k, Put: 10k) | `5d72d86226c858195da24d6f202bfbb04f43000fcf2cc02d3b474ce3094a7e17` |
| **2.0% Trace** | `traces/p7/p7_trace_ratio_020.bin` | 200k Ops (Del: 4k, Get: 146k, Scan: 40k, Put: 10k) | `fb85c2685b0115c1743f8aec57ba122722a9a105cede8f0f58c28e7b242d046f` |
| **5.0% Trace** | `traces/p7/p7_trace_ratio_050.bin` | 200k Ops (Del: 10k, Get: 140k, Scan: 40k, Put: 10k) | `78507e52afa6bc96b6e8facf73ccd8640fb2361ce226ad85b8031665ed3269d5` |
| **10.0% Trace** | `traces/p7/p7_trace_ratio_100.bin` | 200k Ops (Del: 20k, Get: 130k, Scan: 40k, Put: 10k) | `8768b01911933478aba0da92a30d205ec179c46cd1fc4f509223b84f71c9d1e0` |
| **P7 驱动程序** | `bin/p7_driver` | C++20 Release 二进制程序 (`-O2 -std=c++20`) | `92b2342b9deff3e753d733f17cc69fd99ec11f6f7a8e169402c1abcd478dccc8` |

---

## 四、双时间轴采样架构与关键指标清单

P7 驱动在运行期间同时向两套 CSV 管道实时流式写入数据：

### 4.1 采样通道设计
1. **墙钟时间序列通道**（`results/summary/p7-p1-replay/timeseries-wallclock.csv`）：
   每 1 秒捕获 1 次瞬时快照，用于分析长周期组（5%、10%）的真实时间演进。
2. **逻辑进度时间序列通道**（`results/summary/p7-p1-replay/timeseries-progress.csv`）：
   每完成总请求数的 **1%（即每 2,000 次操作）** 捕获 1 次快照（每轮固定 100 个精准快照），彻底消除运行耗时跨度极大（0.5s vs 200s）导致的高速组样本缺失问题，实现相同请求推进程度下的 LSM 状态绝对公平对齐。

### 4.2 采集指标清单
- **前台读写细分度量**：
  - `overall_iops`, `total_ops_interval`
  - `get_ctrl_ops`, `get_ctrl_p50/p95/p99` (基准点查)
  - `get_aff_ops`, `get_aff_p50/p95/p99` (受损区存活点查)
  - `get_del_ops`, `get_del_p50/p95/p99` (已删墓碑命中点查)
  - `scan_ops`, `scan_p50/p95/p99`, `scan_avg_span`, `scan_avg_keys_returned`, `scan_us_per_key` (**核心扫描开销**)
  - `put_ops`, `put_p50/p95/p99`
  - `del_range_ops`, `del_range_p50/p95/p99`
- **RocksDB 内部状态与 Ticker**：
  - `active_memtable_mb`, `immutable_memtable_count`, `memtable_flush_pending`
  - `l0_files`, `l1_files`, `l2_files` (各层 SST 文件数)
  - `pending_compaction_mb`, `running_flushes`, `running_compactions`, `total_sst_mb`
  - `compaction_read_mb_delta`, `compaction_write_mb_delta`, `flush_write_mb_delta`
  - `write_stall_micros_delta`
  - `block_cache_hits_delta`, `block_cache_misses_delta`, `block_cache_hit_rate`
  - `tombstones_dropped_delta` (`COMPACTION_KEY_DROP_RANGE_DEL`)
- **系统与存储状态**：
  - `db_dir_mb` 物理目录落盘大小
  - `iostat -xz 1` 系统磁盘利用率与队列深度

---

## 五、资源预算、执行时序与耗时估算

### 5.1 写入量与磁盘空间预算
- **单轮逻辑写入量**：Preload 128 MB + 前台 Put ~2.5 MB + DeleteRange 元数据 $\approx \mathbf{135\text{ MB}}$
- **18 轮总逻辑写入量**：$18 \times 135\text{ MB} \approx \mathbf{2.43\text{ GB}}$
- **磁盘占用峰值**：单轮运行占用 $\le \mathbf{1.0\text{ GB}}$（单轮运行后立即全量对账并清理，当前 NVMe 可用空间 **585 GB**，余量充裕率 > 99.8%）。

### 5.2 18 轮运行顺序与预计耗时
| 批次与组别 | 比例 | 重复轮次 | 单轮预计耗时 | 批次小计耗时 |
| :--- | :---: | :---: | :---: | :---: |
| **第 1 批：p7_ratio_000** | 0.0% | rep1, rep2, rep3 | ~0.5s ~ 1.0s | ~3 秒 |
| **第 2 批：p7_ratio_005** | 0.5% | rep1, rep2, rep3 | ~1.0s | ~3 秒 |
| **第 3 批：p7_ratio_010** | 1.0% | rep1, rep2, rep3 | ~2.0s ~ 2.5s | ~7 秒 |
| **第 4 批：p7_ratio_020** | 2.0% | rep1, rep2, rep3 | ~7.5s ~ 8.0s | ~24 秒 |
| **第 5 批：p7_ratio_050** | 5.0% | rep1, rep2, rep3 | ~52s ~ 55s | ~2.7 分钟 |
| **第 6 批：p7_ratio_100** | 10.0% | rep1, rep2, rep3 | ~200s ~ 210s | ~10.5 分钟 |
| **总计 18 轮正式运行** | - | **共 18 轮** | - | $\mathbf{\approx 13.6\text{ 分钟}}$ |

---

## 六、执行准入声明

- [x] 阶段A 协议审计报告已生成并归档（`p1-p6-protocol-audit.md`）
- [x] 6 组 Trace 脚本与固化 Checksum 已生成（`traces/p7/`）
- [x] P7 双时间轴专用驱动程序已编译完成并通过校验（`bin/p7_driver`）
- [x] 全部 6 个冻结配置文件已就绪（`configs/p7-p1-replay/`）
- [x] 自动化测试、数据聚合与矢量绘图脚本已就绪（`scripts/p7-p1-replay/`）
- [x] **所有 P7 实验代码、配置与环境已处于冻结状态，等待您的确认指令后再行启动 18 轮正式执行**。
