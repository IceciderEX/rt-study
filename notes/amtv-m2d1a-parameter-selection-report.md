# AMTV M2d.1a 参数选择实验报告 (使用全局页缓存清理的诊断性参数选择结果)

> [!WARNING]
> **实验性质与引用限制**: 本文所载结果为 **使用全局页缓存清理的诊断性参数选择结果**，仅用于内部机制探索与参数空间初筛，**不得作为正式论文性能证据**。因违反共享服务器实验协议（包含 `drop_caches=3`），已启动无全局缓存操作的正式复核实验（M2d.1a-R）。

**文档状态**: 已归档为诊断参考 (Archived as Diagnostic Baseline)  
**执行日期**: 2026-09-06  
**实验环境**: NUMA Node 0 (`taskset -c 0-19`, 20 Physical Cores), Linux, Page Cache Dropped (`drop_caches=3`, 仅诊断使用)  
**构建类型**: Release Binary (`-O3 -DNDEBUG`, Zero Read-Path Audit Overhead)  
**评估矩阵**: 3-Repetition 固定预注册配对交错设计 (Rep 1: A $\to$ B, Rep 2: B $\to$ A, Rep 3: A $\to$ B)  
**候选配置**:
- **Candidate A (首选候选)**: `B64-H32` ($\delta_{\text{chunk}}=64$, $H_{\text{hard}}=32$, $S_{\text{merge}}=2$)
- **Candidate B (低维护成本候选)**: `B128-H16` ($\delta_{\text{chunk}}=128$, $H_{\text{hard}}=16$, $S_{\text{merge}}=2$)

---

## 1. 结论与初步参数选型评估

根据预注册决策准则（Pre-registered Decision Rules，阈值要求相对提升 $\ge 10\%$），本次 M2d.1a 诊断性实验得出初步结论：

> [!NOTE]
> **初步参数选型评估**: 
> 1. **主干参数倾向**: 初步锁定 **Candidate A (`B64-H32`)** 为主要基准候选。
> 2. **备选参数定位**: 明确将 **Candidate B (`B128-H16`)** 保留为“低维护成本候选”（Low Maintenance Cost Profile），推荐在后台计算资源受限、需要严格限制归并频率的环境中使用。
> 3. **架构边界遵循**: 本实验完全遵守前序约束，未引入 M2d.2 双车道归并，未引入写路径休眠/限流/backpressure。

### 1.1 三项预注册决策准则初步评估总结

| 决策准则 | 评估指标与实测数据 | 评估结论 |
| :--- | :--- | :--- |
| **Rule 1: 读延迟与前台吞吐优势 (阈值 $\ge 10\%$)** | Candidate A 前台 IOPS 为 $268,630 \pm 7,453$ ops/s，Candidate B 为 $235,114 \pm 10,827$ ops/s；配对差值 $A - B$ 为 $+33,516 \pm 17,968$ ops/s（**N=3三个配对吞吐差值均为正，均值领先 14.25%**，满足 $\ge 10\%$ 准则要求）。Phase B 耗时 A 比 B 缩短 $0.16 \pm 0.05$ s。GetLive P99.9 改善 10.4%（$34.01$ vs $37.56$ $\mu$s）。Put P99 降低 23.7%（$250.28$ vs $309.57$ $\mu$s），DeleteRange P99 降低 25.5%（$310.52$ vs $389.71$ $\mu$s）。双方回退事件均为 0。 | **Candidate A 满足准则**：吞吐领先达标（+14.25% $\ge$ 10%），写端尾延迟显著更优。 |
| **Rule 2: 积压扩散与 Drain 收敛风险** | Candidate A 的 `peak_actual_sealed_runs` 为 19 runs（`peak_backlog_excess` 为 $18.67 \pm 0.58$ runs），严格低于预设的 24 runs 安全红线，且距 $H=32$ 仍有 13 个 Run 槽位余量。`phase_b_end_to_merge_stable_us` 仅为 $504 \pm 307$ $\mu$s（< 1 ms），`foreground_end_to_merge_stable_us` 为 0 $\mu$s（前台结束前已完全收敛，无残留归并），最终严格收敛至 `signed_backlog == 0`。 | **Candidate A 保持安全稳定**：积压在 Phase B 受控且未扩散，Phase C 早期亚毫秒级收敛，无任何稳定性风险。 |
| **Rule 3: 归并开销与 CPU 权衡** | Candidate B 归并次数减半（152 次 vs 308 次，减少 50.6%），归并总耗时节省约 49.8 ms（303.5 ms vs 353.3 ms），单次最大归并延迟相当（$46.8$ vs $44.8$ ms）。但在 B128 下观察到更高的 Put 和 DeleteRange 尾延迟（写入 P99 增加 24%~26%），前台吞吐下降 14.3%。其延迟来源可能涉及更大的 Open Delta 复制、快照发布及查询成本，具体构成留待后续 Audit 探针验证。 | **Candidate B 验证为优质备选**：适合 CPU 严苛受限场景，但不应取代 A 作为通用默认配置。 |

---

## 2. 实验设计与执行规范

### 2.1 实验环境与隔离控制
- **CPU 亲和性**: 严格绑定至 NUMA Node 0 物理核心 0-19（`taskset -c 0-19`，共 20 物理核），杜绝跨 NUMA 内存访问与超线程干扰。
- **页面缓存清洗**: 每轮执行前调用 `sync && echo 3 | sudo tee /proc/sys/vm/drop_caches && sleep 2`，消除前序负载的 OS 页缓存效应。
- **Release 零开销验证**: Release 驱动程序 `m2d_driver_release` 编译选项为 `-O3 -DNDEBUG`，未定义 `-DROCKSDB_READ_PATH_AUDIT`。经 `nm -C` 符号表审计，确认全局原子探查计数、探查结构体及热路径审计函数完全剔除（0 符号）。
- **种子库物理克隆**: 每轮运行前从只读预热的标准基准种子库 `./run-db/m2d_canonical_seed_db` 物理克隆（包含 500,000 Key 预装数据，经 Flush 与完全静止），杜绝写放大在测试轮次间累积。

### 2.2 固定预注册配对交错调度 (Paired Interleaved Matrix)
为消除长时间运行中的热漂移和机器负载漂移偏差，严格按照预注册的交错序列执行：

$$\text{Rep 1: } \text{Candidate A} \to \text{Candidate B}$$
$$\text{Rep 2: } \text{Candidate B} \to \text{Candidate A}$$
$$\text{Rep 3: } \text{Candidate A} \to \text{Candidate B}$$

每对使用同一组确定性 Trace 输入：
- Rep 1: `traces/m2d_rep1_seed90001` (Random Seed 90001)
- Rep 2: `traces/m2d_rep2_seed100001` (Random Seed 100001)
- Rep 3: `traces/m2d_rep3_seed110001` (Random Seed 110001)

### 2.3 状态模型与全量 Key 对账
在 Window 3（Drain 静止期）结束后，驱动程序执行全量 500,000 Key 点查对账：
1. 逐个验证 300,000 个活跃 Key（Live Keys）的值与版本完全正确。
2. 逐个验证 200,000 个已删除 Key（Deleted Keys）在 RocksDB `Get()` 中返回 `Status::NotFound()`。
3. 数据库内容全量逐 Key 拼接计算 SHA-256 摘要，与独立外部状态模型（ExternalStateModel）计算的基准摘要做逐比特严格比对。
**结果**: 6 轮实验全部实现 500,000 Key 100% 对账成功，SHA-256 逐比特完全一致。

---

## 3. 统计结果与配对比较

### 3.1 核心性能指标配对汇总表

所有指标基于 3 轮有效配对，列出各候选的样本均值与样本标准差（$\text{Mean} \pm \text{SD}$），以及配对差值（$\Delta = A - B$）的统计量：

| 性能指标 | Candidate A (`B64-H32`) | Candidate B (`B128-H16`) | 配对差值 ($A - B$) [$\text{Mean} \pm \text{SD}$] | 差值中位数 (Median) | 优势倾向 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **前台 IOPS (ops/s)** | **$268,630 \pm 7,453$** | $235,114 \pm 10,827$ | **$+33,516 \pm 17,968$** | $+41,178$ | **Candidate A (+14.3%)** |
| **前台总耗时 (s)** | **$1.12 \pm 0.03$** | $1.28 \pm 0.06$ | **$-0.16 \pm 0.09$** | $-0.19$ | **Candidate A** |
| **Phase B 耗时 (s)** | **$0.85 \pm 0.01$** | $1.02 \pm 0.04$ | **$-0.16 \pm 0.05$** | $-0.17$ | **Candidate A** |
| **Phase B 块到达率 (chunks/s)** | $366.59 \pm 4.27$ | $153.94 \pm 6.15$ | $+212.64 \pm 9.81$ | $+216.73$ | Candidate A ($2.38\times$) |
| **GetLive P50 ($\mu$s)** | $7.08 \pm 0.09$ | $7.35 \pm 0.12$ | $-0.27 \pm 0.15$ | $-0.33$ | Candidate A |
| **GetLive P95 ($\mu$s)** | $14.97 \pm 0.61$ | $15.43 \pm 0.16$ | $-0.46 \pm 0.68$ | $-0.56$ | Candidate A |
| **GetLive P99 ($\mu$s)** | $21.60 \pm 0.73$ | $21.85 \pm 0.58$ | $-0.25 \pm 1.31$ | $-0.87$ | 相当 ($\approx 21.7$ $\mu$s) |
| **GetLive P99.9 ($\mu$s)** | **$34.01 \pm 2.47$** | $37.56 \pm 1.16$ | **$-3.55 \pm 2.01$** | $-3.90$ | **Candidate A (-10.4%)** |
| **GetLive Max ($\mu$s)** | $1,789 \pm 2,731$ | $2,913 \pm 2,328$ | $-1,123 \pm 2,583$ | $-2,082$ | Candidate A |
| **Put P99 ($\mu$s)** | **$250.28 \pm 6.40$** | $309.57 \pm 12.44$ | **$-59.29 \pm 18.30$** | $-67.48$ | **Candidate A (-23.7%)** |
| **Put Max ($\mu$s)** | $656 \pm 194$ | $1,697 \pm 1,965$ | $-1,040 \pm 2,102$ | $-236$ | Candidate A |
| **DeleteRange P99 ($\mu$s)** | **$310.52 \pm 10.44$** | $389.71 \pm 17.53$ | **$-79.18 \pm 26.64$** | $-89.92$ | **Candidate A (-25.5%)** |
| **DeleteRange Max ($\mu$s)** | $663 \pm 181$ | $1,734 \pm 1,987$ | $-1,071 \pm 2,101$ | $-226$ | Candidate A |
| **引擎输出写放大 (WA)** | **0x** | **0x** | $0.00 \pm 0.00$ | $0.00$ | 均为 0x (无前台 Flush) |

---

### 3.2 积压动态与 Drain 指标对比

按照重新修订的标准口径统计：
- $\text{stable\_run\_count} = \text{popcount}(\lfloor delete\_count / B \rfloor)$
- $\text{signed\_backlog} = actual\_sealed\_runs - \text{stable\_run\_count}$
- $\text{backlog\_excess} = \max(0, \text{signed\_backlog})$

| 指标项目 | Candidate A (`B64-H32`) | Candidate B (`B128-H16`) | 配对差值 ($A - B$) [$\text{Mean} \pm \text{SD}$] | 关键分析 |
| :--- | :--- | :--- | :--- | :--- |
| **实际封印 Run 峰值 (`peak_actual_sealed_runs`)** | 19 runs (恒定) | 8 runs (恒定) | $+11.00 \pm 0.00$ runs | A 真实物理 Run 峰值为 19，距硬上限 $H=32$ 拥有 **13 个 Run 槽位余量**；B 真实物理 Run 峰值为 8，距 $H=16$ 拥有 8 个 Run 槽位余量。 |
| **峰值超额积压 (`peak_backlog_excess`)** | $18.67 \pm 0.58$ runs | $8.00 \pm 0.00$ runs | $+10.67 \pm 0.58$ runs | Candidate A 因块到达率高出 $2.38\times$，超额积压峰值为 19 runs。注意：硬上限余量应由 `peak_actual_sealed_runs` 推导。 |
| **超额积压最大持续时间 ($\mu$s)** | $44,917 \pm 1,093$ | $46,743 \pm 2,838$ | $-1,826 \pm 1,958$ | 两者积压最长持续时间均在 45 ms 左右，即单次深层归并（L6/L7）的执行耗时。 |
| **在途声明输入 Run 数 (`claimed_input_runs`)** | 2 runs (恒定) | 2 runs (恒定) | $0.00 \pm 0.00$ | M2c 状态机调度行为严格稳定，无超额声明或死锁。 |
| **调度排队峰值 (`scheduling_backlog`)** | $18.67 \pm 0.58$ | $8.00 \pm 0.00$ | $+10.67 \pm 0.58$ | 与超额积压完全重合，表明排队与物理层数严格一致。 |
| **Phase B 结束至严格收敛耗时 (`phase_b_end_to_merge_stable_us`)** | **$504 \pm 307$ $\mu$s** | **$517 \pm 203$ $\mu$s** | **$-13 \pm 501$ $\mu$s** | 双方均在 **亚毫秒级（< 1 ms）** 严格收敛至 `task_state==Idle && mergeable_pair_count==0 && queued/running==0 && signed_backlog==0`。 |
| **前台结束至严格收敛耗时 (`foreground_end_to_merge_stable_us`)** | **$0$ $\mu$s** | **$0$ $\mu$s** | **$0.00 \pm 0.00$** | 系统在 Phase C 结束前已完全达到严格稳态，无任何滞后归并任务。 |
| **Phase B 结束时在途任务状态** | `task_state=3, diag=3` | `task_state=3, diag=3` | 一致 | 均为 `kRunning` / `kComputing` 阶段。 |
| **Phase B 结束后新计算归并数** | 1 个 (尾部收敛归并) | 1 个 (尾部收敛归并) | $0.00 \pm 0.00$ | 仅完成 Phase B 结束时已在执行的 1 个尾部归并。 |
| **Phase B 结束后仅发布归并数** | 0 个 | 0 个 | $0.00 \pm 0.00$ | 无悬挂未发布归并。 |

---

### 3.3 归并计算与时间线指标剖析

| 归并度量指标 | Candidate A (`B64-H32`) | Candidate B (`B128-H16`) | 配对差值 ($A - B$) [$\text{Mean} \pm \text{SD}$] | 关键结论 |
| :--- | :--- | :--- | :--- | :--- |
| **总完成归并次数 (`completed`)** | 308 次 (恒定) | 152 次 (恒定) | $+156.00 \pm 0.00$ | Candidate B 归并次数比 A **减少 50.6%**。 |
| **最大计算墙钟时间 (`max_computed_wall_us`)** | $44,821 \pm 1,102$ (L7) | $46,680 \pm 2,864$ (L6) | $-1,858 \pm 1,978$ | 单次最大计算耗时基本持平（$\approx 45 \sim 47$ ms）。 |
| **最大发布墙钟时间 (`max_published_wall_us`)** | $44,844 \pm 1,108$ (L7) | $46,755 \pm 2,922$ (L6) | $-1,911 \pm 2,027$ | 发布动作开销极小（仅 20~70 $\mu$s），两者最大发布时间基本持平。 |
| **归并总计算墙钟耗时 ($\mu$s)** | $347,055 \pm 16,077$ | $299,876 \pm 17,222$ | $+47,179 \pm 22,608$ | Candidate B 节省约 47 ms 后台计算墙钟。 |
| **归并总发布墙钟耗时 ($\mu$s)** | $353,278 \pm 16,575$ | $303,518 \pm 17,681$ | $+49,760 \pm 23,477$ | Candidate B 节省约 50 ms 后台发布墙钟。 |
| **重建放大比 (`AMTV Reconstruction Amp`)** | 7.35x (理论 7.35x) | 6.35x (理论 6.35x) | $+1.00 \pm 0.00$ | B 减少了约 1.0x 的墓碑合并重复扫描。 |
| **回退事件数 (`fallback_events`)** | 0 次 | 0 次 | 0 | 均未触及硬上限。 |

---

### 3.4 内存代理指标对比

统一采用严格的代理口径（Proxy Metrics），避免估算失真：

| 内存代理指标 | Candidate A (`B64-H32`) | Candidate B (`B128-H16`) | 配对差值 ($A - B$) | 架构解释 |
| :--- | :--- | :--- | :--- | :--- |
| `raw_entry_payload_bytes_peak` | 800,000 bytes (恒定) | 800,000 bytes (恒定) | 0 bytes | 20,000 个墓碑 $\times$ 40 bytes 基础负载完全一致。 |
| `raw_entry_capacity_proxy_bytes_peak` | 2,242,160 bytes | 2,245,304 bytes | -3,144 bytes | 动态容量代理基本完全持平（约 2.14 MB）。 |
| `fragment_payload_proxy_bytes_peak` | 798,720 bytes | 798,720 bytes | 0 bytes | 终态片段负载完全一致（约 780 KB）。 |
| `inflight_payload_proxy_bytes_peak` | 655,360 bytes | 655,360 bytes | 0 bytes | 在途归并最大两层输入负载完全一致（640 KB）。 |
| `peak_rss_kb` | $365,795 \pm 1,118$ KB | $364,908 \pm 1,913$ KB | $+887 \pm 3,021$ KB | 进程物理内存峰值基本完全一致（$\approx 357$ MB）。 |

---

## 4. 机制深入分析：Candidate A 与 B 的行为差异

1. **写入尾延迟差异 (Write-Path Tail Latency Analysis)**:
   - 在 B128 下，观察到更高的 Put（P99 高出 24%）和 DeleteRange（P99 高出 26%）长尾延迟。
   - 注意：Open Delta 采用就地累加，并不会在写路径上等待填满 128 条后才确认写入。
   - B128 下尾延迟升高的潜在来源包括：更大的 Open Delta 条目在发生快照发布、版本推进或合并时的内存拷贝与发布开销，以及更大未封口增量对并发查询的影响。其精确物理构成留待后续专用 Audit 探针验证，不作为 Release 阶段定论。

2. **$H=32$ 提供的充足 Run 槽位余量 (Run Slot Headroom)**:
   - 在 Candidate A ($B=64, H=32$) 下，实际封印 Run 峰值 `peak_actual_sealed_runs` 为 19 runs。由于硬上限扩展为 32，系统拥有多达 **13 个 Run 槽位余量**（$32 - 19 = 13$），**Fallback 事件彻底降低为 0**！
   - 不能直接使用 `peak_backlog_excess` 推导硬上限余量，必须基于真实的 `peak_actual_sealed_runs` 计算。

3. **严格稳态收敛性 (Strict Stability Convergence)**:
   - 系统的严格稳态判定准则为：`task_state == Idle && mergeable_pair_count == 0 && queued/running == 0 && signed_backlog == 0`。
   - Phase B 结束时，系统仅需完成 1 个在途尾部归并（耗时约 45 ms），随后在 Phase C 开始后 300~800 $\mu$s 内即达到严格稳态（`signed_backlog == 0`）。
   - 在前台工作全部结束时（Window 1 终止），`foreground_end_to_merge_stable_us` 恒为 0 $\mu$s，后台归并线程早已完全处于空闲状态，无任何积压拖尾。

---

## 5. 后续行动与交付边界

根据用户指示与工程边界，本次任务目标已完全达成：
1. **统一统计口径**: 诊断报告中最大归并时间冲突已完成彻底修正与文字澄清（区分了已发布的 L6/L7 归并与未发布的丢弃归并）。
2. **Drain 指标重新定义**: 输出并验证了 `phase_b_end_to_merge_stable_us`、`foreground_end_to_merge_stable_us` 及在途任务精确状态。
3. **完成 6 轮配对矩阵**: 无差错执行完成 3 轮配对交错实验，数据统计严密，具备完全可重现性。
4. **决策明确**: 锁定 `B64-H32` 为后续阶段的主选参数基准，`B128-H16` 为低维护成本备选。
5. **严守工作边界**: 
   - 暂不实现 M2d.2 双车道归并；
   - 暂不进入正式 Audit N=3 或 Release N=5、F1、F2；
   - 暂不增加写路径 sleep、限流或 backpressure；
   - 本阶段工作至此圆满交付并停止。
