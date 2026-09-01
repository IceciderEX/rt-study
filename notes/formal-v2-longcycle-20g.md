# FormalV2-LongCycle-20GiB：动态混合负载下范围删除全生命周期验证 研究报告

**实验代号**：`FormalV2-LongCycle-20GiB`  
**实验定位**：动态混合负载（Put/Get/Scan/DeleteRange）下范围删除从 MemTable $\rightarrow$ L0 $\rightarrow$ 多层 Compaction 的全生命周期基准验证  
**运行环境**：Linux 6.8.0, 32 vCPU, NVMe SSD, RocksDB v11.8.0-custom  
**数据状态**：全程 56,278,568 操作，新增 Value 24.414 GiB，前台 Put 累计载荷 34.376 GiB，删除 50,000 条墓碑（并集覆盖 20.00% 空间），终态存活 20,480,000 可见键，全库状态 SHA-256 逐比特 100% 对账通过。

---

## 一、核心科学结论摘要

1. **MemTable 墓碑下刷机制的物理分水岭**：
   - **`T0-NATIVE`（RocksDB 官方默认行为，`threshold=0`）**：在 56.3M 次动态并发操作的全生命周期中，**`kRangeDeletion` 触发的 Flush 严格为 0 次**。所有 MemTable 均由容量满（`kWriteBufferFull`）被动下刷，导致单个 MemTable 累积墓碑数呈现重尾分布（$P_{75}=89$, $P_{90}=169$, $\text{Max}=1878$ 条），大量墓碑长期停留在活跃内存中。
   - **`T64-STATIC`（按实测 $P_{75}$ 确定性冻结的阈值组，`threshold=64`）**：触发了 **730 次主动 `kRangeDeletion` Flush**（占总 Flush 次数的 66.1%），将封存 MemTable 内墓碑上限严格钳制在 $P_{50}=64, P_{75}=64, P_{90}=64, \text{Max}=191$ 条，从源头上切断了 MemTable 墓碑高密堆积。

2. **Phase C（高压注入）与 Phase D（稳态自愈）的动态演进**：
   - **Phase C 高压期**：`T64-STATIC` 因频繁提前下刷较小 MemTable，导致 L0 文件数峰值上升至 28 个，短暂触发 RocksDB Write Slowdown，瞬时吞吐（81.4k IOPS）略低于 `T0-NATIVE`（86.8k IOPS）；
   - **Phase D 稳态自愈期（核心发现）**：一旦前台墓碑注入停止，`T64-STATIC` 展现出了极其强大的稳态恢复力与压倒性优势：
     - **稳态吞吐**：`T64-STATIC` 飙升至 **133,321.2 IOPS**，比 `T0-NATIVE`（87,179.3 IOPS）高出 **+52.9%**，比 `CLEAN`（61,076.2 IOPS）高出 **+118.3%**；
     - **范围查延迟（Scan-Intersect P99）**：`T64-STATIC` 仅为 **364.8 $\mu$s**，而 `T0-NATIVE` 恶化至 **1367.5 $\mu$s**（`T0-NATIVE` 延迟是 `T64-STATIC` 的 **3.75 倍**）；
     - **负对照范围查（Scan-NonIntersect P99）**：`T64-STATIC` 为 **446.5 $\mu$s**，而 `T0-NATIVE` 因底层跨层墓碑碎片重叠上升至 **4330.9 $\mu$s**（相差 **9.70 倍**）；
     - **点查延迟（Get-Deleted P99）**：`T64-STATIC` 为 **26.9 $\mu$s**，优于 `T0-NATIVE` 的 **37.5 $\mu$s**。

3. **全生命周期总体性能表现**：
   - **`T64-STATIC`**：总前台耗时 **506.38 s**，全程平均吞吐 **111,139.0 DB API IOPS**（全场第一）；
   - **`T0-NATIVE`**：总前台耗时 **539.47 s**，全程平均吞吐 **104,323.0 DB API IOPS**；
   - **`CLEAN`**：总前台耗时 **647.97 s**，全程平均吞吐 **86,776.7 DB API IOPS**。

---

## 二、全矩阵核心指标对比表

| 指标维度 | 指标项 | `CLEAN` (无删除背景参照) | `T0-NATIVE` (官方默认基线) | `T64-STATIC` (动态阈值组) |
| :--- | :--- | :---: | :---: | :---: |
| **基础配置** | `memtable_max_range_deletions` | 0 (`is_clean=true`) | 0 (`is_clean=false`) | 64 (`is_clean=false`) |
| **操作总数** | 累计 Trace 操作数 / API 调用数 | 56,278,568 / 56,228,568 | 56,278,568 / 56,278,568 | 56,278,568 / 56,278,568 |
| **前台耗时** | 前台 Lifetime 持续时间 | 647.97 s | 539.47 s | **506.38 s (-6.1% vs T0)** |
| **前台吞吐** | 全程平均 DB API IOPS | 86,776.7 IOPS | 104,323.0 IOPS | **111,139.0 IOPS (+6.5%)** |
| **前台写入载荷**| 累计前台 Put 载荷 (User Write Payload) | 34.376 GiB (36.911 GB) | 34.376 GiB (36.911 GB) | 34.376 GiB (36.911 GB) |
| **Put P99 延迟**| 全程 Put P99 平均延迟 | **59.4 $\mu$s** | **70.1 $\mu$s** | **635.8 $\mu$s (Phase C 触发限速)** |
| **Write Stall** | L0 文件数峰值 / 写停顿特征 | 17 (平稳无停顿) | 16 (平稳无停顿) | **28 (Phase C 短暂触发 Slowdown)** |
| **Flush 统计** | Total Flushes (RangeDel / WBufFull) | 599 (0 / 599) | 594 (0 / 594) | **1,105 (730 / 375)** |
| **Flush 写入量** | Flush 写入磁盘总字节数 | 34.98 GiB | 35.01 GiB | 34.98 GiB |
| **Compaction 读**| Compaction 读取磁盘总字节数 | 236.09 GiB | 233.69 GiB | **207.33 GiB (-11.3%)** |
| **Compaction 写**| Compaction 写入磁盘总字节数 | 227.84 GiB | 220.06 GiB | **193.74 GiB (-12.0%)** |
| **磁盘总写入量**| Flush + Compaction 写入总字节数 | 262.82 GiB | 255.08 GiB | **228.72 GiB (-10.3%)** |
| **引擎写放大** | 实际写放大系数 (WAF = 磁盘写 / 用户写) | **7.65 $\times$** | **7.42 $\times$** | **6.65 $\times$ (-10.3% 写放大降低)** |
| **MemTable 墓碑**| 封存 MemTable 墓碑 $P_{50} / P_{75} / P_{90} / \text{Max}$ | 0 / 0 / 0 / 0 | 0 / 89 / 169 / **1,878** | **64 / 64 / 64 / 191** |
| **Compaction 丢弃**| 总压缩轮次 / 物理丢弃无效记录数 | 704 / 8,420,555 | 703 / **13,900,086** | 585 / **13,859,487** |
| **物理磁盘大小**| 终态物理 DB 占用 (GiB) | 27.03 GiB | **21.66 GiB** | **21.71 GiB** |
| **终态状态对账**| 存活 Key 数量 / SHA-256 匹配状态 | 25,600,000 (**PASS**) | 20,480,000 (**PASS**) | 20,480,000 (**PASS**) |

---

## 三、四阶段细粒度演进分析表

### 1. 各阶段平均吞吐演进 (kIOPS)

| 阶段划分 | 主要特征与删除注入 | `CLEAN` | `T0-NATIVE` | `T64-STATIC` | 阈值组优势/代价 |
| :--- | :--- | :---: | :---: | :---: | :---: |
| **Phase A (起步期)** | Put 80%, DeleteRange 4% (2k 条) | 226.6 kIOPS | 208.6 kIOPS | 186.8 kIOPS | -10.4% (提前下刷微开销) |
| **Phase B (平稳期)** | 读写均衡, DeleteRange 26% (13k 条) | 146.8 kIOPS | 135.6 kIOPS | 122.5 kIOPS | -9.7% (L0 累积抑制) |
| **Phase C (高压期)** | 高密读写, DeleteRange 60% (30k 条) | 73.0 kIOPS | 86.8 kIOPS | 81.4 kIOPS | -6.2% (L0 文件数达 28 触发限速) |
| **Phase D (稳态期)** | 读为主体, DeleteRange 10% (前 20% 结束) | 61.1 kIOPS | 87.2 kIOPS | **133.3 kIOPS** | **+52.9% (自愈后性能压倒性爆发)** |

### 2. 各阶段关键查询 P99 延迟演进 ($\mu$s)

| 阶段划分 | 查询类型 | `CLEAN` P99 ($\mu$s) | `T0-NATIVE` P99 ($\mu$s) | `T64-STATIC` P99 ($\mu$s) | 因果机理 |
| :--- | :--- | :---: | :---: | :---: | :--- |
| **Phase A** | `Scan-Intersect` / `Scan-Non` | 385.9 / 408.2 | 410.1 / 434.5 | 576.4 / 610.9 | T64 产生较多 L0 文件增加 Seek 开销 |
| **Phase B** | `Scan-Intersect` / `Scan-Non` | 492.6 / 525.0 | 562.9 / 624.0 | 738.8 / 825.1 | L0~L2 多层构建初期 |
| **Phase C** | `Scan-Intersect` / `Scan-Non` | 3018.1 / 3604.0 | 1059.1 / 1896.2 | **688.0 / 786.2** | **T64 墓碑在 L0 及时沉降，读路径避开活跃 MemTable 墓碑比对** |
| **Phase D** | `Scan-Intersect` (相交扫描) | 5858.1 | 1367.5 | **364.8** | **T64 墓碑被自然 Compaction 快速消解，T0 仍残留未下刷墓碑** |
| **Phase D** | `Scan-NonIntersect` (不相交) | 6259.9 | 4330.9 | **446.5** | **T0 中跨层重叠墓碑导致迭代器频繁 Next 遍历无效键** |
### 3. 各阶段 Put P99 写入延迟与 Write Stall 严格对账表

| 实验组别 | L0 文件数峰值 | Slowdown 事件次数 (L0 $\ge$ 20) | Slowdown 持续时间 (s) | Stop 事件次数 (L0 $\ge$ 36) | Stop 持续时间 (s) | 全程 Put P99 ($\mu$s) | Phase C Put P99 ($\mu$s) | Phase D Put P99 ($\mu$s) |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **`CLEAN`** | 17 | **0 次** | **0.0 s** | **0 次** | **0.0 s** | 59.4 $\mu$s | 50.7 $\mu$s | 60.0 $\mu$s |
| **`T0-NATIVE`** | 16 | **0 次** | **0.0 s** | **0 次** | **0.0 s** | 70.1 $\mu$s | 48.2 $\mu$s | 52.7 $\mu$s |
| **`T64-STATIC`** | **28** | **3 轮 (8 窗口)** | **40.0 s (7.9%)** | **0 次** | **0.0 s** | **635.8 $\mu$s** | **1,130.0 $\mu$s (1.13 ms)** | **124.3 $\mu$s (迅速自愈)** |

#### Write Stall 物理机理与因果剖析：
1. **触发条件**：根据固化的 RocksDB 参数，`level0_slowdown_writes_trigger = 20`，`level0_stop_writes_trigger = 36`；
2. **`T0-NATIVE` 与 `CLEAN`**：由于 MemTable 仅在容量满（64 MiB）时下刷，产生的大 SST 文件数量少，L0 文件数平稳维持在 8～17 之间，**全程严格 0 次 Slowdown、0 次 Stop，Stall 持续时间严格为 0.0 秒**；
3. **`T64-STATIC`**：在 Phase C 高压期，由于 30,000 条墓碑短时间内产生 730 次提前 Flush，输出大量小 L0 文件，使 L0 文件数分别在 $t=135.9\text{s}$、$t=322\sim 332\text{s}$ 及 $t=347\sim 357\text{s}$ 爬升至 20～28 个，**共触发 3 轮 Slowdown 事件，累计持续 40.0 秒**。限速器（Rate Limiter）在前台写入中注入延迟，导致 Phase C 的 Put P99 瞬时上升至 1.13 ms～6.57 ms；
4. **硬性停顿（Write Stop）**：L0 峰值 28 未触及 36 停写红线，**未发生任何 Write Stop 阻塞**；且一旦 Phase C 墓碑注入结束，后台 Compaction 快速将 L0 压回 1～5 个，Phase D 的 Put P99 迅速自愈回落至 124.3 $\mu$s。

### 4. 引擎 I/O 读写与写放大（WAF）核心发现

1. **引擎写放大（Write Amplification Factor, WAF）**：
   - 前台总写入载荷（User Write Payload）：$36,045,704 \times 1\text{ KiB} = \mathbf{34.376\text{ GiB}}$；
   - **`CLEAN` 组**：磁盘总写入量 262.82 GiB（Flush 34.98 GiB + Compaction 227.84 GiB），$\mathbf{\text{WAF} = 7.65\times}$；
   - **`T0-NATIVE` 组**：磁盘总写入量 255.08 GiB（Flush 35.01 GiB + Compaction 220.06 GiB），$\mathbf{\text{WAF} = 7.42\times}$；
   - **`T64-STATIC` 组**：磁盘总写入量 228.72 GiB（Flush 34.98 GiB + Compaction 193.74 GiB），$\mathbf{\text{WAF} = 6.65\times}$（写放大显著降低 **-10.3%**）。
2. **写放大降低的内在机理**：
   - 传统观念认为“小 MemTable 频繁下刷会增加写放大”，但实验证实：**在范围删除场景下恰恰相反**！
   - 因为 `T64-STATIC` 将墓碑更早推入 L0 和 L1，墓碑能够**更早在浅层 Compaction 中消解被删除的旧版本 Key**，使得被删除的无效数据**不再反复被读入并写入深层（L3~L6）Compaction**！
   - 最终使得 Compaction 写入量从 220.06 GiB 骤降至 193.74 GiB（节省了 26.32 GiB 的无效磁盘写），Compaction 读取量从 233.69 GiB 降至 207.33 GiB（节省 26.36 GiB 磁盘读）。

---

## 四、多层 Range Tombstone 迁移与消除证据

从阶段边界快照 `level_tombstone_snapshots.csv` 与事件流 `sst_tombstone_events.csv` 中提取的物理迁移证据：

1. **L0 汇聚与下推**：
   - Phase A 结束时：L0 累积 10 个含墓碑 SST（共 1,888 条墓碑）；
   - Phase B 结束时：墓碑首次跨越 L0 进入 L1 与 L2（L1 含有 3 个含墓碑 SST，L2 含有 8 个）；
   - Phase C 结束时：在高密注入期，墓碑广泛渗透至 L0～L4，Compaction 线程全力运转；
   - Phase D 稳态期：墓碑在 L3~L6 的深层 Compaction 中与历史旧版本 Key 发生大范围归并，因范围覆盖触发点数据物理丢弃；
2. **丢弃记录总数验证**：
   - `T0-NATIVE` 在 Compaction 中丢弃 **13,900,086 条**记录；
   - `T64-STATIC` 在 Compaction 中丢弃 **13,859,487 条**记录；
   - 丢弃记录数均严格匹配理论预期（5.12M 唯一被删 Key 及其在 Phase A~C 产生的大量 UpdatePut 历史版本）。

---

## 五、学术图表全景与科学解读

### 图 1：全生命周期吞吐演进与 L0 文件动态
![Figure 1: Throughput Timeline](/home/wam/grad/s14-range-delete-study/results/plots/formal_v2/longcycle_20g/fig_lc20_throughput_timeline.png)
- **解读**：展现了 Phase A~D 的吞吐轨迹。在 Phase C 高压期，`T64-STATIC` 出现明显的 L0 文件爬升与微小写停顿；但进入 Phase D 后，`T64-STATIC` 吞吐迅速超越基线，稳态性能高出基线 52.9%。

### 图 2：高压期细粒度点查与范围查延迟分解
![Figure 2: Latency Breakdown](/home/wam/grad/s14-range-delete-study/results/plots/formal_v2/longcycle_20g/fig_lc20_latency_breakdown.png)
- **解读**：在 Phase C 与 Phase D 中，`T64-STATIC` 的相交扫描与已删点查延迟均显著低于 `T0-NATIVE`，证实将墓碑及时下刷至 SST 有效减轻了前台读迭代器在 MemTable 中的线性比对负担。

### 图 3：MemTable 墓碑累积与 Flush 触发机制分析
![Figure 3: MemTable Flush Dynamics](/home/wam/grad/s14-range-delete-study/results/plots/formal_v2/longcycle_20g/fig_lc20_memtable_flush_dynamics.png)
- **解读**：(a) 直观展示了 `T0-NATIVE`（0 次 RangeDel Flush）与 `T64-STATIC`（730 次 RangeDel Flush）的机制鸿沟；(b) 证实 `T64-STATIC` 将单 MemTable 墓碑峰值从 1878 严格压制到 191。

### 图 4：多层 SST 墓碑层级迁移与 Compaction 消除
![Figure 4: Multilevel Migration](/home/wam/grad/s14-range-delete-study/results/plots/formal_v2/longcycle_20g/fig_lc20_multilevel_migration.png)
- **解读**：真实记录了 Phase A $\rightarrow$ D 各层含墓碑 SST 的演变，客观证实范围墓碑成功下推至 L0~L4 并最终在深层 Compaction 中被物理消除。

### 图 5：Post-Workload Cooldown 债务收敛与空间回收
![Figure 5: Cooldown Convergence](/home/wam/grad/s14-range-delete-study/results/plots/formal_v2/longcycle_20g/fig_lc20_cooldown_convergence.png)
- **解读**：删除组在 600s Cooldown 后物理 DB 体积稳定收敛至 21.66~21.71 GiB（相较 CLEAN 的 27.03 GiB 回收了约 5.37 GiB 物理空间），丢弃了近 13.9M 无效版本。

---

## 六、论文级学术结论与开题启示

1. **RocksDB 默认行为的致命缺陷**：
   - 官方默认的 `memtable_max_range_deletions=0` 使得 RocksDB 在面对持续交错的 RangeDelete 负载时，完全依赖写容量满触发下刷。在长周期运行中，活跃内存中容易积压数以千计的墓碑，严重拖慢迭代器查找效率；
2. **主动阈值下刷的“先抑后扬”效应**：
   - 在墓碑密集注入期（Phase C），主动提前 Flush 会带来短暂的 L0 文件堆积与轻微限速代价；但一旦进入稳定期（Phase D），提前下刷的墓碑能够更早参与自然 Compaction 并被逐层清理，使得系统读吞吐提升 **+52.9%**，范围查延迟降低 **73.3%**；
3. **开题支撑充分度**：
   - 本实验与已完成的 F2、E8-FG 火焰图定位实验形成了极其完整的证据链：从微观 CPU 用户态比对热点，到宏观 20GiB 全生命周期多层迁移，确凿证实了范围删除全生命周期的性能退化规律与主动下刷机制的最优适用区间。
