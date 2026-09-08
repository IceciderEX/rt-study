# M3a：动态混合负载下的 AMTV GetOnly 边界审计报告（Audit N=3）

> **比较定位与实验性质声明**：
> - **Native-T0 vs AMTV-T0**：是在相同 T0、相同写投影、相同终态和零阈值 Flush 条件下，对 AMTV 点查旁路机制的主比较；本负载包含刻意保留的原生 Scan 边界路径。
> - **Native-T512 vs AMTV-T512**：继续严格定义为**端到端配置比较**，不得称为相同 Flush 机制或纯读路径消融。
> - **时间口径规范**：物化耗时与锁等待时间统一称为“累计线程侧计时/等待时间”（Cumulative Thread-Side Time），反映 8 个 Worker 线程的并发开销累加，禁止与墙钟时间（Wall-Clock Time）直接相加。
> - **执行范围约束**：本阶段仅完成隔离 Audit 构建、静态 Trace 审计、4 组 Audit Smoke 与 12 轮 Audit $N=3$ 矩阵；不运行 Release 性能矩阵、24GiB 压测或任何 Scan 优化实现。

---

## 1. 静态 Trace 审计与因果几何核验

根据 P0 修订规范，本实验对四套数据集（Smoke Seed 90001、Rep 1 Seed 510001、Rep 2 Seed 520001、Rep 3 Seed 530001）实施了逐操作语义预注册与硬因果断言，全部 4 套数据集均通过 `scripts/m3a/audit_m3a_trace.py` 静态审计。

### 1.1 Scan 样本数与可见键预期完全预注册

| 阶段 | Planned-Intersect Scan | Actual Intersect Scan | NonIntersect Scan | 每条预期可见键 | 阶段可见键总数 |
|:---|---:|---:|---:|:---|---:|
| **Phase A** | 5,000 | 0 | 5,000 | 全部 100 键 | 1,000,000 |
| **Phase B** | 0 | 5,000 | 5,000 | Intersect: 50 键<br>NonIntersect: 100 键 | 750,000 |
| **Phase C** | 0 | 5,000 | 5,000 | Intersect: 50 键<br>NonIntersect: 100 键 | 750,000 |
| **全流程合计** | **5,000** | **10,000** | **15,000** | — | **2,500,000** |

- **`Scan-PlannedIntersect`**（Phase A 前 5,000 条）：位于未来删除几何位置，但此时数据库无任何墓碑，严格断言全部返回 100 个可见键。独立命名与统计，严禁与真实 Intersect 合并。
- **`Scan-Intersect`**（Phase B/C 各 5,000 条）：每条范围覆盖 5 条历史 DeleteRange（共 50 个被删键），严格断言固定返回 50 个可见键。
- **`Scan-NonIntersect`**（Phase A/B/C 各 5,000 条）：位于永久存活区，严格断言固定返回 100 个可见键。
- **驱动逐记录断言**：C++ 驱动根据 Trace 记录的 `expected_visible_keys` 逐条断言实际扫描键数，不使用全局模糊判断。

### 1.2 硬因果前置断言与哈希一致性

在 Trace 生成期间，生成器模拟每个 Worker 线程的局部操作流，断言对每一条 Phase B `Scan-Intersect`，其所覆盖的 5 条 DeleteRange 必须已经在该 Worker 此前的操作序列中完成，杜绝任何“扫描在删除前执行却期望 50 键”的时序穿透。

| 数据集 | 种子 | 20,000 条删除几何 SHA-256 | 读投影 SHA-256 | 写投影 SHA-256 | 完整 Trace SHA-256 | 终态 500K 键 SHA-256 |
|:---|:---:|:---|:---|:---|:---|:---|
| **Smoke** | 90001 | `d900a54ff4f6e869ba8efbab6557529d2a824a3ace14bebaf267b79125e211b0` | `b43db056bba...` | `4f363fd3287...` | `7afd1fb6536...` | `1ce8ebf73b80732dc076293d88fcef22668c19a87a27d0aab890e7c3a9a895a6` |
| **Rep 1** | 510001 | `d900a54ff4f6e869ba8efbab6557529d2a824a3ace14bebaf267b79125e211b0` | `fee100cf164...` | `441fac4d30c...` | `a0b0e442bb5...` | `480ceb1d100017d2b2838998421824c54147f1df70a616d13cd62c9f255b1a66` |
| **Rep 2** | 520001 | `d900a54ff4f6e869ba8efbab6557529d2a824a3ace14bebaf267b79125e211b0` | `ae90d6ba481...` | `001d842d229...` | `2a5993cc29e...` | `d7008779c403309a96e987cba684824343ce4e0dae3b610c3c544fa097a8e561` |
| **Rep 3** | 530001 | `d900a54ff4f6e869ba8efbab6557529d2a824a3ace14bebaf267b79125e211b0` | `4aba205adf...` | `9a4d62822ac...` | `574a49a7fc6...` | `2072746e6a06d44b18a10cbce66356c0727460790d4a0850431227a4c4308dc0` |

- 四套数据集的删除几何 SHA-256 完全恒定，总覆盖范围恰好为 200,000 键（40% 空间）；
- 所有 12 轮测试终态均严格为 300,000 Live / 200,000 Deleted，且同一 Seed 下的四种配置最终状态 SHA-256 达到 100% 逐 bit 一致。

---

## 2. 源码级锁与统计探针映射

为彻底消除过往模糊合并命名的缺陷，对 RocksDB v11.8.0 读写路径探针测量的锁进行严格源码映射：

| 探针 / 指标名称 | 测量的底层物理锁 | 源码定义位置 | 触发与采集函数 | 测量事件定义 | 计时单位 |
|:---|:---|:---|:---|:---|:---:|
| `reader_mutex_contended_count` | `reader_mutex` | `db/range_tombstone_fragmenter.h:119` (`port::RWMutex reader_mutex`) | `MemTable::ConstructFragmentedRangeTombstones()` (`db/memtable.cc:952`) | 读/扫描线程尝试获取读锁失败进入排队的次数 (`!try_lock()`) | 次 |
| `cumulative_thread_side_reader_mutex_wait_nanos` | `reader_mutex` | 同上 | 同上 (`db/memtable.cc:956-963`) | 读/扫描线程阻塞等待获取 `reader_mutex` 的线程侧累计等待耗时 | 纳秒 (ns) |
| `cumulative_thread_side_materialization_count` | N/A (持有锁期间执行) | `db/memtable.cc:967` | `MemTable::ConstructFragmentedRangeTombstones()` | 实际执行未分片墓碑迭代并构造 `FragmentedRangeTombstoneList` 的次数 | 次 |
| `cumulative_thread_side_materialization_nanos` | N/A (持有锁期间执行) | `db/memtable.cc:970` | 同上 (`db/memtable.cc:968-980`) | 构造分片墓碑列表的实际执行线程侧累计耗时 | 纳秒 (ns) |
| `range_del_mutex_` (写路径) | `range_del_mutex_` | `db/memtable.h:1055` (`port::RWMutex range_del_mutex_`) | `MemTable::Add()` | 并发写入 DeleteRange 时保护 MemTable 墓碑列表的互斥锁（未在读路径 probe 中测量） | 纳秒 (ns) |

> **关键架构说明**：
> 1. 读路径审计探针测量的是 `FragmentedRangeTombstoneListCache::reader_mutex`，而非写路径的 `range_del_mutex_`。本报告严格区分，绝不混淆。
> 2. `AuditOpScope` 采用 RAII 自动管理线程局部标签 `g_current_audit_op_type`，即使在错误或迭代器提前释放路径也严格恢复为 `kNone`。
> 3. 每个 Worker 在阶段屏障前自行执行 `TakeAndReset`，主线程仅在屏障完成后汇总，经单元测试验证确保 0 重复与 0 遗漏。

---

## 3. Audit Smoke 冒烟测试验证结果（Seed 90001）

在启动 N=3 矩阵前，完成 4 组 Audit Smoke 运行，验证基础指标与恒等式完全闭合：

| 配置 | Phase B 耗时 (s) | Phase B IOPS | 终态 SHA 校验 | Iterator 扫描可见键 | AMTV Fallback | Flush 次数 | 已刷墓碑 | 活跃代残留 | 墓碑总守恒状态 |
|:---|---:|---:|:---:|---:|:---:|---:|---:|---:|:---:|
| **Native-T0** | 30.19 s | 3,312.5 | PASS | 300,000 | N/A | 0 | 0 | 20,000 | **PASS (20,000)** |
| **Native-T512** | 2.23 s | 44,770.3 | PASS | 300,000 | N/A | 39 | 19,992 | 8 | **PASS (20,000)** |
| **AMTV-T0** | 2.66 s | 37,540.9 | PASS | 300,000 | **0** | 0 | 0 | 20,000 | **PASS (20,000)** |
| **AMTV-T512** | 2.44 s | 40,994.7 | PASS | 300,000 | **0** | 38 | 19,502 | 498 | **PASS (20,000)** |

全部 4 组 Smoke 的 500K 键终态 SHA-256 均为 `1ce8ebf73b80732dc076293d88fcef22668c19a87a27d0aab890e7c3a9a895a6`，AMTV 回退为 0，T512 墓碑守恒严格成立。

---

## 4. Audit N=3 实验结果数据矩阵

四种配置在 Seeds 510001 (Rep 1)、520001 (Rep 2)、530001 (Rep 3) 下执行，采用平衡交错调度，共完成 12 轮完整测试。

### 4.1 吞吐率与阶段耗时汇总（Mean ± Std across N=3）

| 配置 | Phase A IOPS | Phase B IOPS | Phase B 耗时 (s) | Phase C IOPS | Flush 次数 | 累计已刷墓碑数 | 尾部活跃代残留 | 墓碑总数 |
|:---|---:|---:|---:|---:|---:|---:|---:|:---:|
| **Native-T0** | 486,300 ± 18,237 | 3,275 ± 76 | 30.55 ± 0.72 | 365,730 ± 12,365 | 0.0 | 0 | 20,000 | 20,000 (PASS) |
| **Native-T512** | 506,601 ± 48,960 | 55,239 ± 5,602 | 1.83 ± 0.19 | 369,732 ± 30,736 | 39.0 | 19,982 | 18 | 20,000 (PASS) |
| **AMTV-T0** | 484,847 ± 48,476 | **37,345 ± 988** | **2.68 ± 0.07** | 551,425 ± 22,886 | 0.0 | 0 | 20,000 | 20,000 (PASS) |
| **AMTV-T512** | 477,904 ± 28,683 | 60,297 ± 13,384 | 1.74 ± 0.39 | 341,554 ± 42,201 | 38.0 | 19,503 | 497 | 20,000 (PASS) |

### 4.2 Phase B (动态写入期) 逐操作延迟与底层开销（Mean across N=3）

| 配置 | 操作类型 | 样本数 | P50 (μs) | P99 (μs) | Max (μs) | 物化次数 | 累计线程侧物化计时 (ms) | `reader_mutex` 争用次数 | 累计线程侧等待 (ms) | 墓碑重寻次数 |
|:---|:---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| **Native-T0** | `GetLive` | 40,000 | **4,246.75** | **11,899.73** | 18,727.33 | **14,115** | **82,008.28** | **23,571** | **102,004.98** | 0 |
| | `Scan-Intersect` | 5,000 | 4,473.43 | 12,117.37 | 17,554.83 | 1,599 | 9,593.91 | 3,120 | 13,651.84 | 55,643 |
| | `Scan-NonIntersect` | 5,000 | 4,450.80 | 12,099.23 | 17,868.43 | 1,603 | 9,533.93 | 3,116 | 13,626.08 | 1 |
| | `Put` | 30,000 | 55.38 | 316.96 | 5,849.93 | 0 | 0.00 | 0 | 0.00 | 0 |
| | `DeleteRange` | 20,000 | 116.62 | 409.71 | 5,328.57 | 0 | 0.00 | 0 | 0.00 | 0 |
| **AMTV-T0** | `GetLive` | 40,000 | **9.49** | **27.30** | 186.83 | **0** | **0.00** | **0** | **0.00** | 0 |
| | `Scan-Intersect` | 5,000 | 1,777.33 | 3,876.89 | 6,124.09 | 3,995 | 6,949.68 | 1,001 | 1,502.17 | 55,643 |
| | `Scan-NonIntersect` | 5,000 | 1,763.06 | 3,860.64 | 6,395.41 | 3,968 | 6,902.12 | 1,025 | 1,506.33 | 1 |
| | `Put` | 30,000 | 13.94 | 193.43 | 403.06 | 0 | 0.00 | 0 | 0.00 | 0 |
| | `DeleteRange` | 20,000 | 47.97 | 236.54 | 423.12 | 0 | 0.00 | 0 | 0.00 | 0 |
| **Native-T512** | `GetLive` | 40,000 | 97.25 | 343.48 | 749.96 | 13,335 | 1,393.56 | 20,475 | 2,075.12 | 0 |
| | `Scan-Intersect` | 5,000 | 258.61 | 645.18 | 6,559.97 | 1,346 | 142.24 | 2,873 | 294.22 | 39,778 |
| | `Scan-NonIntersect` | 5,000 | 253.85 | 619.02 | 1,015.35 | 1,375 | 146.26 | 2,838 | 287.57 | 1 |
| | `Put` | 30,000 | 58.65 | 1,242.07 | 2,607.08 | 0 | 0.00 | 0 | 0.00 | 0 |
| | `DeleteRange` | 20,000 | 95.32 | 1,301.22 | 2,496.40 | 0 | 0.00 | 0 | 0.00 | 0 |
| **AMTV-T512** | `GetLive` | 40,000 | 20.25 | 69.62 | 674.62 | 0 | 0.00 | 0 | 0.00 | 0 |
| | `Scan-Intersect` | 5,000 | 219.62 | 541.20 | 7,863.65 | 3,677 | 318.01 | 1,109 | 89.18 | 41,972 |
| | `Scan-NonIntersect` | 5,000 | 213.58 | 524.42 | 2,930.22 | 3,667 | 314.73 | 1,108 | 86.15 | 1 |
| | `Put` | 30,000 | 124.26 | 1,353.98 | 2,459.57 | 0 | 0.00 | 0 | 0.00 | 0 |
| | `DeleteRange` | 20,000 | 172.01 | 1,420.01 | 2,519.13 | 0 | 0.00 | 0 | 0.00 | 0 |

### 4.3 Phase C (静态存活期) 逐操作延迟与底层开销（Mean across N=3）

| 配置 | 操作类型 | 样本数 | P50 (μs) | P99 (μs) | Max (μs) | 物化次数 | 累计线程侧物化计时 (ms) | `reader_mutex` 争用次数 | 累计线程侧等待 (ms) | 墓碑重寻次数 |
|:---|:---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| **Native-T0** | `GetLive` | 80,000 | 9.29 | 31.87 | 5,594.61 | 1 | 3.19 | 6 | 30.90 | 0 |
| | `Scan-Intersect` | 5,000 | 73.47 | 200.55 | 3,927.56 | 0 | 1.90 | 0 | 1.77 | 56,697 |
| | `Scan-NonIntersect` | 5,000 | 67.73 | 179.61 | 3,686.50 | 0 | 0.00 | 1 | 5.07 | 0 |
| | `Put` | 10,000 | 15.21 | 55.16 | 118.03 | 0 | 0.00 | 0 | 0.00 | 0 |
| **AMTV-T0** | `GetLive` | 80,000 | 7.96 | 19.89 | 146.60 | 0 | 0.00 | 0 | 0.00 | 0 |
| | `Scan-Intersect` | 5,000 | 54.82 | 90.65 | 1,271.12 | 0 | 0.00 | 1 | 3.22 | 56,697 |
| | `Scan-NonIntersect` | 5,000 | 50.32 | 81.51 | 1,312.67 | 0 | 1.05 | 1 | 4.32 | 0 |
| | `Put` | 10,000 | 11.14 | 28.19 | 87.81 | 0 | 0.00 | 0 | 0.00 | 0 |
| **Native-T512** | `GetLive` | 80,000 | 9.18 | 34.82 | 229.73 | 0 | 0.00 | 0 | 0.00 | 0 |
| | `Scan-Intersect` | 5,000 | 65.56 | 249.52 | 411.57 | 0 | 0.00 | 0 | 0.00 | 8,818 |
| | `Scan-NonIntersect` | 5,000 | 78.61 | 246.56 | 1,404.56 | 0 | 0.00 | 0 | 0.00 | 0 |
| | `Put` | 10,000 | 14.32 | 43.97 | 1,007.95 | 0 | 0.00 | 0 | 0.00 | 0 |
| **AMTV-T512** | `GetLive` | 80,000 | 10.72 | 37.22 | 220.67 | 0 | 0.00 | 0 | 0.00 | 0 |
| | `Scan-Intersect` | 5,000 | 72.68 | 243.71 | 385.87 | 0 | 0.04 | 0 | 0.00 | 12,309 |
| | `Scan-NonIntersect` | 5,000 | 80.08 | 228.88 | 380.23 | 0 | 0.00 | 0 | 0.00 | 0 |
| | `Put` | 10,000 | 14.30 | 395.14 | 903.79 | 0 | 0.00 | 0 | 0.00 | 0 |

---

## 5. 核心机制深入分析与五个问题解答

### 问题 1：GetLive 物化与锁排队削减保留率
**结论：保留率为严格的 100.0%**。
- **数据事实**：在 Native-T0 中，Phase B 的 40,000 次 GetLive 触发了 **14,115 次**未分片墓碑物化（累计线程侧耗时 **82.01 秒**），产生了 **23,571 次** `reader_mutex` 锁争用（累计线程侧等待 **102.00 秒**），导致 GetLive P50 飙升至 4,246.75 μs，P99 达 11,899.73 μs。
- **AMTV-T0 表现**：物化次数严格为 **0**，物化耗时严格为 **0.00 ms**，锁争用严格为 **0**，锁等待严格为 **0.00 ms**。GetLive P50 保持在 **9.49 μs**，P99 为 **27.30 μs**。
- **机制机理**：在 AMTV-T0 的 GetLive 路径中，原生活跃 MemTable 视图物化和 `reader_mutex` 争用均为 0，说明 GetOnly 旁路仍绕开了该点查直接瓶颈。

### 问题 2：Scan-Intersect 在动态注入期的真实表现与排队连锁反应
**结论：AMTV 并未直接优化 Scan；AMTV-T0 中较低的 Scan 等待与延迟和“Get 退出 reader mutex 竞争后 Scan 间接受益”一致，但不是唯一因果的直接证明**。
- **数据事实与物化量化**：
  - AMTV 并未直接优化 Scan。Phase B 中，AMTV-T0 的两类 Scan 原生物化合计为 `3,995 + 3,968 = 7,963` 次，高于 Native-T0 的 `1,599 + 1,603 = 3,202` 次；
  - 两个 T0 组的相交 Scan reseek 均为 **55,643**，说明 AMTV 没有改变 Scan iterator 的范围墓碑推进语义；
  - 延迟与争用数据对比：
    - Native-T0 下：`Scan-Intersect` P50 为 4,473.43 μs，`reader_mutex` 争用达 3,120 次，累计线程侧锁等待达 13.65 秒；
    - AMTV-T0 下：`Scan-Intersect` P50 为 **1,777.33 μs**，锁争用降至 1,001 次，累计线程侧锁等待降至 1.50 秒。
- **因果边界分析**：
  - 在 Native-T0 下，40,000 次 GetLive 与 10,000 次 Scan 全部争抢同一个 `reader_mutex`。而在 AMTV-T0 下，由于 40,000 次 GetLive 绕开 `reader_mutex`，争用源减少。
  - AMTV-T0 中较低的 Scan 等待与延迟，和“Get 退出 reader mutex 竞争后 Scan 间接受益”这一假说一致，但由于存在物化调度频次等并发交织，不能作为唯一因果的直接证明。
  - AMTV-T0 Phase B 单次 Scan 耗时仍为 1.78 ms 量级，且产生 55,643 次范围墓碑重寻，证明未改造的原生 Scan 路径本身仍受制于原生物化与重寻机制。

### 问题 3：原生 Scan 路径与点查的耗时量级对比
**结论：Scan 耗时比点查高出 2 个数量级，Scan 耗时在 Phase B 跨 Worker 操作耗时求和中占 88%**。
- **单操作耗时对比（AMTV-T0 Phase B）**：
  - `GetLive`：P50 = 9.49 μs，平均耗时 15.2 μs；
  - `Put`：P50 = 13.94 μs，平均耗时 21.3 μs；
  - `DeleteRange`：P50 = 47.97 μs，平均耗时 54.1 μs；
  - `Scan-Intersect`：P50 = 1,777.33 μs，平均耗时 1,847.2 μs；
  - `Scan-NonIntersect`：P50 = 1,763.06 μs，平均耗时 1,838.5 μs。
- **时间占用量化与口径界定**：
  - 10,000 次 Scan 跨 Worker 操作耗时求和：$10,000 \times 1.84\ \text{ms} \approx \mathbf{18.4\ \text{线程秒}}$；
  - 其余 90,000 次前台操作（Get/Put/DeleteRange）跨 Worker 操作耗时求和：约 $\mathbf{2.5\ \text{线程秒}}$；
  - 严正口径界定：Scan 的 88% 只能称为“**Phase B 跨 Worker 操作耗时求和中的占比**”，不得称为 CPU 占比、全局墙钟占比或串行时间。这反映了各 Worker 在执行原生 Scan 时累计耗时显著高于点查与写入。

### 问题 4：整体吞吐率与端到端对比
**结论：在 M3a 混合读负载 Audit 构建下，AMTV-T0 吞吐相比 Native-T0 提升 11.4 倍；本构建不得与 M2d Release 直接计算收益稀释率**。
- **数据事实**：
  - M3a Audit 构建下（包含 10% 原生 Scan）：Phase B 中 Native-T0 为 3,275 IOPS，AMTV-T0 为 37,345 IOPS（提升 **11.4 倍**），Phase B 耗时从 30.55 秒缩短至 2.68 秒。
  - 对照 M2d Release（纯 GetLive + Put + DeleteRange）：整体吞吐比约 39.67 倍，Phase B 耗时比约 50.47 倍。
- **比较纪律**：
  - M3a 属于带有全量 Audit/Verify 热路径探针的 Audit 构建，探针开销不可忽略；而 M2d 是关闭探针的 Release 构建。
  - M3a Audit 构建不得与 M2d Release 直接计算正式收益稀释率。两者的真实边界差异必须等待后续全量 Release（如 M3b）在同构无探针环境下进行对照。

### 问题 5：对推进 M1c Scan 架构改造的决策依据
**结论：数据提供了坚实的量化依据**。
1. **点查直接瓶颈已被绕开，Scan 耗时占比较高**：AMTV-T0 的 GetLive 路径原生物化与排队均为 0，点查直接瓶颈被绕开；而在前台混合负载中，原生 Scan 的单次耗时达 1.78 ms，在 Phase B 跨 Worker 操作耗时求和中占比达 88%。
2. **Phase C 静态存活期仍暴露重寻缺陷**：即使在写入完全停止的 Phase C，`Scan-Intersect` 仍触发了 **56,697 次** `scan_reseek_count`，说明原生 `MergingIterator` 配合 `RangeDelAggregator` 在多墓碑跨度下存在频繁回退 Seek 的固有结构缺陷。
3. **M1c 的定位与价值**：M1c Scan 架构的切入点在于基于 AMTV 维护的有序墓碑切片，向迭代器提供单调前进的墓碑切片视图，以消除原生 Scan 路径的分片重构与反复重寻。

---

## 6. T512 物理维护分组与代际分布审计

禁止假定 Native-T512 与 AMTV-T512 维护分组一致。以下为 N=3 矩阵中全部 6 轮 T512 测试的代际 Flush 与尾部活跃代明细核查：

| 配置 | Rep | 种子 | Flush 次数 | 已刷墓碑总数 | 尾部活跃代残留 | 墓碑守恒和 | 单代 Flush 墓碑范围 | Flush 触发原因 |
|:---|:---:|:---:|:---:|---:|---:|:---:|:---:|:---|
| **Native-T512** | 1 | 510001 | 39 | 19,987 | 13 | **20,000** | 512 ~ 516 | Memtable Max Range Deletions |
| **Native-T512** | 2 | 520001 | 39 | 19,981 | 19 | **20,000** | 512 ~ 514 | Memtable Max Range Deletions |
| **Native-T512** | 3 | 530001 | 39 | 19,978 | 22 | **20,000** | 512 ~ 514 | Memtable Max Range Deletions |
| **AMTV-T512** | 1 | 510001 | 38 | 19,503 | 497 | **20,000** | 512 ~ 516 | Memtable Max Range Deletions |
| **AMTV-T512** | 2 | 520001 | 38 | 19,506 | 494 | **20,000** | 512 ~ 515 | Memtable Max Range Deletions |
| **AMTV-T512** | 3 | 530001 | 38 | 19,500 | 500 | **20,000** | 512 ~ 515 | Memtable Max Range Deletions |

> **关键物理观察**：
> 1. **严格守恒**：全部 6 轮测试中，`flushed_tombstones + active_generation_tombstones = 20,000` 严格无一偏差。
> 2. **Flush 次数与尾部残留**：
>    - Native-T512 稳定触发 **39 次 Flush**，尾部活跃代残留 **13 ~ 22 条**墓碑；
>    - AMTV-T512 稳定触发 **38 次 Flush**，尾部活跃代残留 **494 ~ 500 条**墓碑（距离 512 阈值仅差十余条，因而未触发第 39 次 Flush）；
> 3. **全量明细归档**：逐 Rep、逐 Generation 的完整明细（包含每代 job_id、墓碑数、累计数）已完整归档于 `results/amtv_m3a/m3a_audit_n3_t512_generations.csv`。

---

## 7. 交付文件与审计清单

1. **测试驱动与测试用例**：
   - 驱动实现：[`tools/amtv_m3a/m3a_driver.cc`](file:///home/wam/grad/s14-range-delete-study/tools/amtv_m3a/m3a_driver.cc)
   - 驱动 Makefile：[`tools/amtv_m3a/Makefile`](file:///home/wam/grad/s14-range-delete-study/tools/amtv_m3a/Makefile)
   - 多线程聚合单元测试：[`tools/amtv_m3a/m3a_audit_aggregator_test.cc`](file:///home/wam/grad/s14-range-delete-study/tools/amtv_m3a/m3a_audit_aggregator_test.cc)（通过）
2. **Trace 生成与校验脚本**：
   - Trace 生成器：[`scripts/m3a/generate_trace_m3a.py`](file:///home/wam/grad/s14-range-delete-study/scripts/m3a/generate_trace_m3a.py)
   - 静态 Trace 审计脚本：[`scripts/m3a/audit_m3a_trace.py`](file:///home/wam/grad/s14-range-delete-study/scripts/m3a/audit_m3a_trace.py)（100% 通过）
3. **测试执行与数据聚合脚本**：
   - Smoke 与 Matrix 调度器：[`scripts/m3a/run_m3a_audit.py`](file:///home/wam/grad/s14-range-delete-study/scripts/m3a/run_m3a_audit.py)
   - 结果聚合与表格生成器：[`scripts/m3a/aggregate_m3a_results.py`](file:///home/wam/grad/s14-range-delete-study/scripts/m3a/aggregate_m3a_results.py)
4. **数据产物（CSV 与原始 JSON）**：
   - 阶段与吞吐汇总表：[`results/amtv_m3a/m3a_audit_n3_summary.csv`](file:///home/wam/grad/s14-range-delete-study/results/amtv_m3a/m3a_audit_n3_summary.csv)
   - 逐阶段逐操作开销明细表：[`results/amtv_m3a/m3a_audit_n3_per_phase_ops.csv`](file:///home/wam/grad/s14-range-delete-study/results/amtv_m3a/m3a_audit_n3_per_phase_ops.csv)
   - T512 逐代 Flush 明细表：[`results/amtv_m3a/m3a_audit_n3_t512_generations.csv`](file:///home/wam/grad/s14-range-delete-study/results/amtv_m3a/m3a_audit_n3_t512_generations.csv)
   - 全部 12 轮原始 JSON 归档：`results/amtv_m3a/raw/m3a_audit_*.json`
