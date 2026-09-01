# 阶段 A：环境与接口审计报告 (notes/environment-audit.md)

**生成时间**：2026-08-20  
**实验机器节点**：`s14.servers.hustpdsl.cn` (简称 s14)  
**实验项目根目录**：`/home/wam/grad/s14-range-delete-study/`  
**官方 RocksDB 源码树**：`/home/wam/grad/rocksdb-v11.8.0/`  
**审计人员与执行者**：Antigravity Agent

---

## 1. 硬件平台与操作系统

| 类别 | 规格项 | 实际检测值 | 验证命令/证据源 |
| :--- | :--- | :--- | :--- |
| **CPU** | 架构 / 型号 | x86_64 / Intel(R) Xeon(R) Gold 5218R CPU @ 2.10GHz | `lscpu` |
| | 物理 Socket / 核心数 | 2 Sockets / 40 Cores (20 Cores/Socket) | `lscpu` |
| | 逻辑 CPU (vCPU) | 80 vCPUs (超线程开启, 2 Threads/Core) | `lscpu` |
| | NUMA 节点 | 2 节点 (Node0: 0-19,40-59; Node1: 20-39,60-79) | `lscpu` |
| | L1 / L2 / L3 Cache | L1d 1.3MiB, L1i 1.3MiB, L2 40MiB, L3 55MiB | `lscpu` |
| **内存** | 物理内存总量 (RAM) | 78.0 GiB (83,775,946,752 Bytes) | `free -b` |
| | 当前可用内存 (Available) | 63.0 GiB (67,658,678,272 Bytes) | `free -b` |
| | Swap 空间 | 8.0 GiB (8,589,930,496 Bytes, 使用 < 13MiB) | `free -b` |
| **操作系统** | Linux 内核版本 | `6.8.0-136-generic` (#136~22.04.1-Ubuntu SMP) | `uname -a` |
| | 发行版 | Ubuntu 22.04.5 LTS (Jammy Jellyfish) | `/etc/os-release` |
| **编译工具链** | GCC / G++ | `g++ (Ubuntu 11.4.0-1ubuntu1~22.04.3) 11.4.0` | `g++ --version` |
| | Clang / Clang++ | `Ubuntu clang version 20.1.8` | `clang++ --version` |
| | CMake | `cmake version 3.22.1` | `cmake --version` |

---

## 2. 存储设备、挂载点与空间预算审计

### 2.1 存储设备与挂载证明
- **块设备名称**：`nvme0n1`
- **设备硬件型号**：`KIOXIA-EXCERIA PRO SSD` (1.8TB PCIe NVMe SSD)
- **底层特性**：`rotational=0` (非机械盘/SSD), I/O 调度器 `[none] mq-deadline`
- **文件系统与挂载分区**：`/dev/nvme0n1p2` 格式化为 `ext4`，挂载在根目录 `/` (`rw,relatime`)
- **路径挂载定位证明**：
  ```text
  $ findmnt -T /home/wam/grad/s14-range-delete-study
  TARGET SOURCE         FSTYPE OPTIONS
  /      /dev/nvme0n1p2 ext4   rw,relatime
  ```
  **结论**：确认 `/home/wam/grad/s14-range-delete-study/` 及 `/home/wam/grad/rocksdb-v11.8.0/` 严格位于高速 NVMe SSD (`/dev/nvme0n1p2`) 上，未挂载于 `/dev/shm`、SATA SSD、机械盘或网络文件系统。

### 2.2 容量与 Inode 余量
- **总容量**：1.8 TiB (1,966,736,678,912 Bytes)
- **已用空间**：1.2 TiB (1,239,536,762,880 Bytes, 67%)
- **当前可用空间**：**585 GiB** (627,219,664,896 Bytes, 33%)
- **Inode 总量**：122,028,032
- **已用 Inode**：5,346,273 (5%)
- **空闲 Inode**：**116,681,759** (95%)

### 2.3 实验空间预算评估
- **单次实验最大预算**：
  - 数据集规模：500,000 ~ 2,000,000 keys (Key 16B, Value 100B~1KB)，逻辑数据量约 100MB ~ 2GB。
  - SST 与 WAL 物理占用峰值估算：< 5 GiB。
- **全套预实验 (P1~P5 包含重复运行) 最大预算**：
  - 预计总运行轮次：P1(6组*3次=18次) + P2(4组*3次=12次) + P3(2组*3次=6次) + P4(3组*3次=9次) + P5(3组*3次=9次) = 54 轮。
  - 每次运行采用独立新目录并在验证分析后清理 raw db（保留原始 metrics/log），同时保留的峰值磁盘占用不超过 30 GiB。
  - **空间裕量比**：可用空间 585 GiB / 峰值预算 30 GiB = **19.5 倍**，存储与 Inode 裕量完全充足。

---

## 3. 系统 I/O 竞争与背景负载审计

- **I/O 利用率基线**：
  - `iostat -xz 1 2` 实测：`%util = 0.00%`，`r/s = 0.00`，`w/s = 0.00`，`iowait = 0.00%`。
- **背景活跃进程排查**：
  - 后台主要常驻服务为 TiKV/TiDB 演示实例 (`tikv-server`, `tidb-server`) 及 IDE 进程，均处于极低 CPU/无 I/O 闲置状态。
- **结论**：当前系统不存在活跃高 I/O 进程，背景无共享 I/O 干扰，具备严谨实验准入条件。

---

## 4. RocksDB 源码与构建审计 (严格遵循第二条)

### 4.1 源码与 Git 状态
- **源码路径**：`/home/wam/grad/rocksdb-v11.8.0/`
- **上游地址**：`https://github.com/facebook/rocksdb.git`
- **Git Tag**：`v11.8.0` (commit `abeebd9630f11bd08c28b7bd43c7bdfc62050654`)
- **工作树状态**：纯净无修改 (`working tree clean`)
- **版本宏定义** (`include/rocksdb/version.h`)：
  ```cpp
  #define ROCKSDB_MAJOR 11
  #define ROCKSDB_MINOR 8
  #define ROCKSDB_PATCH 1
  ```
- **构建方式与构建参数**：
  - 构建目录：`/home/wam/grad/rocksdb-v11.8.0/build`
  - 构建指令：`cmake -DCMAKE_BUILD_TYPE=Release -DWITH_TESTS=OFF -DWITH_GFLAGS=OFF -DWITH_BENCHMARK_TOOLS=OFF -DWITH_TOOLS=OFF -DWITH_CORE_TOOLS=OFF .. && make -j32 rocksdb`
  - 产物：`librocksdb.a` (39MB 静态库，Release 优化)

---

## 5. RocksDB 观测指标与接口语义核验清单

严格核验 RocksDB v11.8.0 C++ API 下各指标的实际存在性、获取方式与确切含义：

| 指标需求项 | 实际 API / Property / Ticker 标识符 | 可用性 | 指标含义与提取方式说明 |
| :--- | :--- | :--- | :--- |
| **Compaction 读字节** | `ROCKSDB_NAMESPACE::Tickers::COMPACT_READ_BYTES` | **可用** | Compaction 过程中从磁盘读取的总字节数 (Ticker 累加值) |
| **Compaction 写字节** | `ROCKSDB_NAMESPACE::Tickers::COMPACT_WRITE_BYTES` | **可用** | Compaction 过程中写入新 SST 文件的总字节数 (Ticker 累加值) |
| **Flush 写字节** | `ROCKSDB_NAMESPACE::Tickers::FLUSH_WRITE_BYTES` | **可用** | MemTable Flush 写入 L0 SST 文件的总字节数 (Ticker 累加值) |
| **WriteStall 累计时间** | `ROCKSDB_NAMESPACE::Tickers::STALL_MICROS` | **可用** | 前台写操作因触发 Stall 限制而累计被阻塞等待的微秒数 |
| **WriteStall 触发次数** | `STALL_L0_SLOWDOWN_COUNT`<br>`STALL_MEMTABLE_COMPACTION_COUNT`<br>`STALL_L0_NUM_FILES_COUNT` | **可用** | 分别记录因 L0 文件过多减速、Memtable 刷写等待、L0 文件达到 Stop 阈值导致的 Stall 次数 |
| **WriteStall 详细状态** | `DB::Properties::kDBWriteStallStats`<br>`("rocksdb.db-write-stall-stats")` | **可用** | 获取 WriteStall 格式化统计报告字符串 |
| **当前延迟写速率** | `DB::Properties::kActualDelayedWriteRate`<br>`("rocksdb.actual-delayed-write-rate")` | **可用** | 发生写入减速时当前的写入速率限额 (Bytes/s) |
| **L0 文件数** | `DB::Properties::kNumFilesAtLevelPrefix + "0"`<br>`("rocksdb.num-files-at-level0")` | **可用** | Level 0 当前包含的活跃 SST 文件总数 (Uint64 属性) |
| **Pending Compaction Bytes** | `DB::Properties::kEstimatePendingCompactionBytes`<br>`("rocksdb.estimate-pending-compaction-bytes")` | **可用** | 估算当前积压待执行 Compaction 的数据字节量，核心衡量写放大与后台堆积 |
| **当前运行 Flush 数量** | `DB::Properties::kNumRunningFlushes`<br>`("rocksdb.num-running-flushes")` | **可用** | 当前正在执行的后台 MemTable Flush 任务并发数 |
| **当前运行 Compaction 数量**| `DB::Properties::kNumRunningCompactions`<br>`("rocksdb.num-running-compactions")` | **可用** | 当前正在执行的后台 Compaction 任务并发数 |
| **BlockCache Hit / Miss** | `BLOCK_CACHE_DATA_HIT` / `BLOCK_CACHE_DATA_MISS`<br>`BLOCK_CACHE_INDEX_HIT` / `BLOCK_CACHE_INDEX_MISS`<br>`BLOCK_CACHE_FILTER_HIT` / `BLOCK_CACHE_FILTER_MISS` | **可用** | 分类统计 Data block, Index block, Filter block 在 Block Cache 中的命中与未命中次数 |
| **BlockCache 实际占用/容量**| `DB::Properties::kBlockCacheUsage`<br>`DB::Properties::kBlockCacheCapacity` | **可用** | BlockCache 实时内存使用量与设定容量 (Bytes) |
| **范围墓碑丢弃 Key 统计** | `ROCKSDB_NAMESPACE::Tickers::COMPACTION_KEY_DROP_RANGE_DEL` | **可用** | Compaction 过程中因为被 Range Tombstone 覆盖而物理丢弃的点 Key 数量 |
| **范围墓碑自身丢弃统计** | *原生无独立 Ticker* | **不可用/待核验** | 原生 RocksDB 仅提供被覆盖 Key 的丢弃计数，未暴露墓碑合并清理自身的丢弃计数 |
| **Range Tombstone 数量属性**| `TableProperties::num_range_deletions`<br>通过 `DB::Properties::kAggregatedTableProperties` 汇总 | **可用** | SST 元数据中记录的 Range Deletion Tombstones 数量，可通过聚合属性提取 |
| **活跃 MemTable 墓碑数** | `DB::Properties::kNumDeletesActiveMemTable` | **部分可用/需区分**| 原生仅提供 MemTable 点删除条目数，RangeDelete 不在此独立计数 |
| **数据库物理 SST 大小** | `DB::Properties::kTotalSstFilesSize`<br>`DB::Properties::kLiveSstFilesSize` | **可用** | 全库 SST 文件物理占用字节数 (Total 包含待删，Live 为当前有效版本占用) |
| **有效数据预估大小** | `DB::Properties::kEstimateLiveDataSize` | **可用** | 去除过期版本与冗余数据后的活跃数据估算字节量 |

---

## 6. 阶段 A 审计结论
1. 环境目录 `/home/wam/grad/s14-range-delete-study/` 与源码树 `/home/wam/grad/rocksdb-v11.8.0/` 严格位于 NVMe SSD 分区 (`/dev/nvme0n1p2`)。
2. 纯净官方 RocksDB `v11.8.0`（commit `abeebd9630f11bd08c28b7bd43c7bdfc62050654`）已成功克隆并以 Release 模式编译产出 `librocksdb.a`。
3. 磁盘空间 (585GB) 与 Inode (1.16亿) 完全满足预实验全部开销。
4. 系统 I/O 无外部竞争，硬件基线稳定。
5. 准入条件全部满足，进入阶段 B 驱动编译与阶段 C SmokeTest 阶段。
