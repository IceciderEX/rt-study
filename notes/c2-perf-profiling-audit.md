# C2：轻量性能剖析报告 (notes/c2-perf-profiling-audit.md)

**实验时间**：2026-08-20  
**实验节点**：`s14.servers.hustpdsl.cn` (80 vCPUs, 78GB RAM)  
**底层介质**：`/dev/nvme0n1p2` (KIOXIA EXCERIA PRO NVMe SSD)  
**RocksDB 版本**：官方 Tag `v11.8.0`（commit `abeebd9630f11bd08c28b7bd43c7bdfc62050654`）  
**剖析方法**：利用 RocksDB 原生内置 `PerfContext` 与 `IOStatsContext`（以 `PerfLevel::kEnableTimeAndCPUTimeExceptForMutex` 级别运行），采集 10,000 次 Scan 操作在 $T=0$ 与 $T=256$ 下的执行耗时构成。

---

## 一、实测细粒度剖析数据对比表

| 性能剖析指标 | **T=0 (墓碑滞留 MemTable)** | **T=256 (阈值下推 SST)** | 变化幅度 (T=256 vs T=0) |
| :--- | :---: | :---: | :---: |
| **10,000 次 Scan 总耗时** | **$32,886.6\text{ ms}$** | **$1,853.4\text{ ms}$** | **加速 17.74 倍 (-94.4%)** |
| **用户键比对次数 (`user_key_comparison_count`)** | **$282,235,436$** | **$17,882,639$** | **减少 15.78 倍 (-93.7%)** |
| **迭代器内部寻道耗时 (`seek_internal_seek_time`)** | **$32,832.2\text{ ms}$** | **$1,826.3\text{ ms}$** | **下降 17.98 倍 (-94.4%)** |
| **子迭代器寻道耗时 (`seek_child_seek_time`)** | $24,575.6\text{ ms}$ | $1,348.1\text{ ms}$ | 下降 18.23 倍 |
| **最小堆排序耗时 (`seek_min_heap_time`)** | $1,416.7\text{ ms}$ | $83.7\text{ ms}$ | 下降 16.93 倍 |
| **SST 数据块读取耗时 (`block_read_time`)** | $14.3\text{ ms}$ | $16.7\text{ ms}$ | 维持在 $14 \sim 17\text{ ms}$ |
| **存储物理 I/O 读取耗时 (`read_nanos`)** | **$11.0\text{ ms}$** | **$12.9\text{ ms}$** | 存储读取等待耗时较低 |
| **存储读取总字节数 (`bytes_read`)** | $20.3\text{ MB}$ | $20.9\text{ MB}$ | 基本一致 |

---

## 二、三层论断体系

### 1. 可确认事实 (Confirmed Facts)

1. **在 PerfContext 覆盖的计时口径中，内部 seek 相关时间占绝对主导**：
   - 在 $T=0$ 下，10,000 次 Scan 的总耗时为 32.89 秒，其中 `seek_internal_seek_time` 为 **32.83 秒**，共记录了 **2.82 亿次** 用户键比对；
   - 在 $T=256$ 下，键比对次数下降至 1,788 万次，`seek_internal_seek_time` 下降至 1.83 秒，总耗时随之降低至 1.85 秒；
   - 相比之下，IOStats 记录的底层存储读取等待耗时（`read_nanos`）在两种配置下均较低（约为 $11 \sim 13\text{ ms}$），物理读取字节数基本一致（约 $20.3 \sim 20.9\text{ MB}$）。

---

### 2. 合理但仍需验证的解释 (Plausible Interpretations)

1. **实测表现与 CPU 侧内部迭代器寻道与 Key 比较开销主导的解释一致**：
   - 当大量范围墓碑滞留在活跃 MemTable 时，迭代器在推进过程中需要频繁比对内存中的墓碑集合，这解释了为何 `user_key_comparison_count` 会出现数亿次激增；
   - 阈值触发 Flush 将墓碑下推后，内存比对次数大幅缩减，从而大幅减轻了 CPU 寻道开销。

---

### 3. 当前实验不能推出的结论 (Unsupported Conclusions)

1. **由于未获得源码级/指令级热点采样（perf 工具在当前系统未就绪），未完成细粒度 CPU 内部函数级归因**：
   - 严禁从端到端统计数据中直接断言某一个特定 C++ 函数（如 RangeDelAggregator）是唯一瓶颈，只能表述为“与 CPU 侧内部迭代器和 Key 比较开销主导的解释一致”。
2. **不能断言底层 I/O 在所有负载场景下均可忽略**：当前测试在数据全部在缓存/内存中时主要反映 CPU 比对压力，在大数据集缓存穿透场景下 I/O 影响仍需独立评估。
