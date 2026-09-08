# M3b：动态混合负载 Release N=5 性能边界与端到端评测报告

> **实验性质与比较定位严正声明**：
> - **Native-T0 vs AMTV-T0**：是**主机制比较**，在相同 T0（零阈值 Flush）、相同写投影、相同终态和零后台 Flush 干扰条件下，验证关闭全部 Audit/Verify 热路径探针后，AMTV 点查旁路机制在混合原生 Scan 负载下的真实端到端性能边界。
> - **Native-T512 vs AMTV-T512**：继续严格定义为**端到端配置比较**。不得把 T512 的 Flush 次数或 I/O 差异解释为保持相同物理维护条件时的纯读路径收益。
> - **永久物理维护成本纪律**：不得把 AMTV-T0 三窗口内零 Flush/Compaction 输出解释为永久物理维护成本为零（墓碑累积在活跃 MemTable 中，一旦触发刷盘仍有对应 SST 维护代价）。
> - **跨阶段对比纪律**：M3b 与 M2d 仅作为不同读负载构成（纯点查 vs 10% 原生 Scan 混合读）下的敏感性对照，不得直接报告为严格的“收益稀释百分比”。
> - **无探针构建保证**：全部 20 轮运行均在 `-O3 -DNDEBUG` 优化下完成，关闭 `ROCKSDB_READ_PATH_AUDIT` 与 `ROCKSDB_AMTV_VERIFY`，消除所有逐操作 Audit tag、原子计数与双轨校验。

---

## 1. 构建隔离与零探针符号核验

为确保 Release 构建的绝对纯净，杜绝任何调试探针引入的热路径开销与符号残留，本次评测完成了预处理检查与 `nm -C` 符号审计：

### 1.1 代码版本与二进制产物哈希

| 组分 / 产物 | 源码路径 / 二进制位置 | Commit / SHA-256 哈希 | 构建配置 / 编译标志 |
|:---|:---|:---|:---|
| **RocksDB 源码** | `/home/wam/grad/rocksdb-v11.8.0` | Commit: `3b400ec3757eeb18ea8f48cfb60f9be6fbfe3d73` | Release (`DEBUG_LEVEL=0`) |
| **Study Repo 源码** | `/home/wam/grad/s14-range-delete-study` | Commit: `5452b3c2b1a91b5d06c2f7cc022a418da7719f8d` | Release (`-O3 -DNDEBUG`) |
| **静态库 `librocksdb.a`** | `/home/wam/grad/build-m2d-release/librocksdb.a` | SHA-256: `22175db42bdb9250f478dc8a9ad04131a95863c22e722b34c1ffc225ca4a4b67` | `-std=c++20 -O3 -DNDEBUG` |
| **测试驱动 `m3a_driver_release`** | `/home/wam/grad/s14-range-delete-study/bin/m3a_driver_release` | SHA-256: `4ea4e605fe3f2012f0a5b15e9828a89ce638ff6029602cbe74bf852a545a69eb` | `-std=c++20 -O3 -DNDEBUG` |

### 1.2 零探针符号与预处理泄漏证明

- **预处理代码检查**：执行 `g++ -E ... tools/amtv_m3a/m3a_driver.cc`，确认 `g_read_path_audit_stats`、`tl_amtv_get_probe_stats`、`ROCKSDB_AMTV_VERIFY_FATAL` 及 Audit tag 相关宏均未展开，输出严格为 `ZERO_PREPROCESSED_LEAK`。
- **静态库符号审计**：执行 `nm -C build-m2d-release/librocksdb.a | grep -E "g_read_path_audit|g_current_audit_op_type|AMTVGetProbeStats|tl_amtv_get_probe_stats|ROCKSDB_AMTV_VERIFY"`，输出严格为 `ZERO_SYMBOLS`。
- **可执行驱动符号审计**：执行 `nm -C bin/m3a_driver_release | grep -iE "read_path_audit|amtv_verify|AuditOpScope|tl_amtv_get_probe_stats|g_read_path_audit"`，输出严格为 `ZERO_AUDIT_SYMBOLS`。
- **Fail-Fast 断言隔离**：所有语义、状态和停止断言均采用宏 `CHECK_INVARIANT`（触发即输出错误上下文并调用 `std::abort()`），未采用依赖宏开关的 `assert()`，在 `-DNDEBUG` 优化下 100% 保持硬生效。

---

## 2. 实验协议、矩阵调度与环境约束

### 2.1 Trace 结构与固定物理 Scan 语义

沿用 M3a 预注册几何体系，使用 5 个全新种子生成 5 套完整 Trace：
- **种子分配**：Rep 1 (`610001`)、Rep 2 (`620001`)、Rep 3 (`630001`)、Rep 4 (`640001`)、Rep 5 (`650001`)。
- **三阶段操作分布**（每个 Worker 37,500 操作，8 个 Worker 共 300,000 操作）：
  - **Phase A (Baseline, 100k ops)**：70k GetLive、5k `Scan-PlannedIntersect`、5k `Scan-NonIntersect`、20k Put。
  - **Phase B (Dynamic Injection, 100k ops)**：40k GetLive、5k `Scan-Intersect`、5k `Scan-NonIntersect`、30k Put、20k DeleteRange。
  - **Phase C (Post-Injection Read-Dominant, 100k ops)**：80k GetLive、5k `Scan-Intersect`、5k `Scan-NonIntersect`、10k Put。
- **严格物理 Scan 语义**：
  - 扫描范围严格为 `[start_key, start_key + 100)`，不设置人工 Limit；
  - Phase B/C 相交 Scan 范围覆盖 5 条被删除跨度（50 个键），固定断言返回 **50 个可见键**；
  - 非相交 Scan 固定断言返回 **100 个可见键**；
  - Phase A 计划相交 Scan（尚无墓碑）固定返回 **100 个可见键**，单列统计，严禁并入真实相交 Scan。

### 2.2 20 轮预注册交错平衡调度表

采用 5-Round 循环平衡交错顺序，杜绝缓存热身与次序效应偏差：

| 轮次 | Rep | 种子 | 执行配置 | 运行模式 | 物理 CPU 亲和性 | 内存分配策略 |
|:---:|:---:|:---:|:---|:---:|:---|:---|
| **1** | 1 | 610001 | **Native-T0** | Release | Socket 0 (Cores 0-19) | `numactl --interleave=all` |
| **2** | 1 | 610001 | **Native-T512** | Release | Socket 0 (Cores 0-19) | `numactl --interleave=all` |
| **3** | 1 | 610001 | **AMTV-T0** | Release | Socket 0 (Cores 0-19) | `numactl --interleave=all` |
| **4** | 1 | 610001 | **AMTV-T512** | Release | Socket 0 (Cores 0-19) | `numactl --interleave=all` |
| **5** | 2 | 620001 | **Native-T512** | Release | Socket 0 (Cores 0-19) | `numactl --interleave=all` |
| **6** | 2 | 620001 | **AMTV-T0** | Release | Socket 0 (Cores 0-19) | `numactl --interleave=all` |
| **7** | 2 | 620001 | **AMTV-T512** | Release | Socket 0 (Cores 0-19) | `numactl --interleave=all` |
| **8** | 2 | 620001 | **Native-T0** | Release | Socket 0 (Cores 0-19) | `numactl --interleave=all` |
| **9** | 3 | 630001 | **AMTV-T0** | Release | Socket 0 (Cores 0-19) | `numactl --interleave=all` |
| **10** | 3 | 630001 | **AMTV-T512** | Release | Socket 0 (Cores 0-19) | `numactl --interleave=all` |
| **11** | 3 | 630001 | **Native-T0** | Release | Socket 0 (Cores 0-19) | `numactl --interleave=all` |
| **12** | 3 | 630001 | **Native-T512** | Release | Socket 0 (Cores 0-19) | `numactl --interleave=all` |
| **13** | 4 | 640001 | **AMTV-T512** | Release | Socket 0 (Cores 0-19) | `numactl --interleave=all` |
| **14** | 4 | 640001 | **Native-T0** | Release | Socket 0 (Cores 0-19) | `numactl --interleave=all` |
| **15** | 4 | 640001 | **Native-T512** | Release | Socket 0 (Cores 0-19) | `numactl --interleave=all` |
| **16** | 4 | 640001 | **AMTV-T0** | Release | Socket 0 (Cores 0-19) | `numactl --interleave=all` |
| **17** | 5 | 650001 | **Native-T0** | Release | Socket 0 (Cores 0-19) | `numactl --interleave=all` |
| **18** | 5 | 650001 | **AMTV-T0** | Release | Socket 0 (Cores 0-19) | `numactl --interleave=all` |
| **19** | 5 | 650001 | **Native-T512** | Release | Socket 0 (Cores 0-19) | `numactl --interleave=all` |
| **20** | 5 | 650001 | **AMTV-T512** | Release | Socket 0 (Cores 0-19) | `numactl --interleave=all` |

- **执行纪律**：全流程禁止 `drop_caches`、`sudo`、`sysctl`、sleep、限流、人工 Flush、人工 CompactRange 及 Phase B/C 间等待；20 轮运行总耗时 **451.84 秒**，无一次重试。

---

## 3. 端到端性能数据与配对统计矩阵

### 3.1 各阶段吞吐率与耗时汇总（Mean ± Std 与 Median / IQR, N=5）

| 配置 | Phase A IOPS | Phase B IOPS | Phase B 耗时 (s) | Phase C IOPS | 全流程 Overall IOPS | 累计 Flush 次数 | 尾部活跃墓碑 |
|:---|---:|---:|---:|---:|---:|---:|---:|
| **Native-T0** | 482,777 ± 33,734<br>*(Med: 498,502 / IQR: 63,005)* | 3,390 ± 43<br>*(Med: 3,374 / IQR: 74)* | 29.50 ± 0.38<br>*(Med: 29.64 / IQR: 0.64)* | 385,268 ± 11,614<br>*(Med: 387,779 / IQR: 12,574)* | 10,011 ± 131<br>*(Med: 9,969 / IQR: 230)* | 0.0 | 20,000 |
| **AMTV-T0** | 484,252 ± 41,861<br>*(Med: 457,624 / IQR: 40,861)* | **37,248 ± 1,624**<br>*(Med: **37,373** / IQR: **1,643**)* | **2.69 ± 0.12**<br>*(Med: **2.68** / IQR: **0.12**)* | **510,662 ± 56,589**<br>*(Med: 475,262 / IQR: 87,957)* | **97,085 ± 4,224**<br>*(Med: **97,970** / IQR: **4,715**)* | 0.0 | 20,000 |
| **Native-T512** | 505,798 ± 23,380<br>*(Med: 510,268 / IQR: 8,094)* | 46,831 ± 3,045<br>*(Med: 48,525 / IQR: 4,235)* | 2.14 ± 0.15<br>*(Med: 2.06 / IQR: 0.19)* | 353,720 ± 11,684<br>*(Med: 350,460 / IQR: 11,326)* | 114,563 ± 5,958<br>*(Med: 117,567 / IQR: 7,077)* | 39.0 | 15.2 |
| **AMTV-T512** | 513,602 ± 39,182<br>*(Med: 502,655 / IQR: 46,726)* | 51,180 ± 5,263<br>*(Med: 50,785 / IQR: 4,603)* | 1.97 ± 0.19<br>*(Med: 1.97 / IQR: 0.19)* | 345,276 ± 51,775<br>*(Med: 347,151 / IQR: 42,927)* | 122,049 ± 7,144<br>*(Med: 123,356 / IQR: 8,668)* | 38.0 | 495.8 |

### 3.2 逐轮配对差值与加速比（Paired Differences across 5 Reps）

由于每个 Rep 内四种配置共享完全相同的完整 Trace、读写投影、删除几何与初始 Seed DB 副本，我们对每一轮计算严格的配对差异：

| Rep | 种子 | Phase B: Native-T0 IOPS | Phase B: AMTV-T0 IOPS | **主比较 Phase B 吞吐加速比** | Native-T0 耗时 (s) | AMTV-T0 耗时 (s) | **耗时净缩短 (s)** | **全流程 Overall 吞吐比** | T512 配置 Phase B 吞吐比 |
|:---:|:---:|---:|---:|---:|---:|---:|---:|---:|---:|
| **1** | 610001 | 3,426.2 | 34,397.3 | **10.04x** | 29.19 s | 2.91 s | **-26.28 s** | **8.88x** | 0.94x |
| **2** | 620001 | 3,454.6 | 37,372.6 | **10.82x** | 28.95 s | 2.68 s | **-26.27 s** | **9.60x** | 1.46x |
| **3** | 630001 | 3,373.8 | 38,525.0 | **11.42x** | 29.64 s | 2.60 s | **-27.04 s** | **10.05x** | 1.04x |
| **4** | 640001 | 3,342.4 | 36,882.4 | **11.03x** | 29.92 s | 2.71 s | **-27.21 s** | **9.67x** | 1.03x |
| **5** | 650001 | 3,352.5 | 39,060.4 | **11.65x** | 29.83 s | 2.56 s | **-27.27 s** | **10.31x** | 1.05x |
| **统计** | **Mean ± Std** | **3,390 ± 43** | **37,248 ± 1,624** | **10.99 ± 0.55x** | **29.50 ± 0.38** | **2.69 ± 0.12** | **-26.81 ± 0.46 s** | **9.70 ± 0.49x** | **1.10 ± 0.18x** |
| **统计** | **Median (IQR)** | **3,374 (74)** | **37,373 (1,643)** | **11.03x (0.60x)** | **29.64 (0.64)** | **2.68 (0.12)** | **-27.04 s (0.93 s)** | **9.67x (0.44x)** | **1.04x (0.02x)** |

---

## 4. 逐阶段逐操作详细延迟与有效键开销（Release N=5）

以下数据来自 Release 驱动内部线程局部直方图，在消除所有 Audit 探针后，精确刻画各操作真实硬件开销：

### 4.1 Phase B (动态写入注入期，包含 20k DeleteRange)

| 配置 | 操作名称 | 样本数 | P50 (μs) | P95 (μs) | P99 (μs) | P99.9 (μs) | Max (μs) | 平均耗时 (μs) | 单 Scan 可见键 | 单键有效开销 (μs/key) |
|:---|:---|---:|---:|---:|---:|---:|---:|---:|:---:|:---:|
| **Native-T0** | `GetLive` | 40,000 | **4,025.50** | 10,501.30 | **11,389.80** | 13,858.20 | 17,486.82 | 4,461.19 | 0 | — |
| | `Scan-Intersect` | 5,000 | 4,186.11 | 10,731.16 | 11,634.66 | 14,734.86 | 17,548.16 | 4,646.05 | 50.0 (PASS) | **92.92 μs** |
| | `Scan-NonIntersect` | 5,000 | 4,185.72 | 10,735.60 | 11,600.50 | 14,257.58 | 16,948.50 | 4,629.61 | 100.0 (PASS) | **46.30 μs** |
| | `Put` | 30,000 | 55.19 | 189.41 | 311.75 | 530.94 | 7,796.37 | 72.30 | 0 | — |
| | `DeleteRange` | 20,000 | 113.60 | 271.35 | 393.42 | 1,497.49 | 7,728.66 | 130.52 | 0 | — |
| **AMTV-T0** | `GetLive` | 40,000 | **9.61** | **19.78** | **27.58** | **53.37** | **247.70** | **10.72** | 0 | — |
| | `Scan-Intersect` | 5,000 | **1,787.58** | 3,367.34 | 3,726.66 | 4,632.39 | 5,136.79 | 1,800.78 | 50.0 (PASS) | **36.02 μs** |
| | `Scan-NonIntersect` | 5,000 | **1,797.22** | 3,356.66 | 3,691.17 | 4,682.02 | 5,658.88 | 1,802.18 | 100.0 (PASS) | **18.02 μs** |
| | `Put` | 30,000 | 13.24 | 108.45 | 188.72 | 285.32 | 408.37 | 29.62 | 0 | — |
| | `DeleteRange` | 20,000 | 44.29 | 149.71 | 227.68 | 326.41 | 419.76 | 58.38 | 0 | — |
| **Native-T512** | `GetLive` | 40,000 | 87.89 | 291.25 | 347.12 | 434.20 | 1,124.39 | 113.52 | 0 | — |
| | `Scan-Intersect` | 5,000 | 252.48 | 553.07 | 653.77 | 834.49 | 3,813.82 | 278.99 | 50.0 (PASS) | 5.58 μs |
| | `Scan-NonIntersect` | 5,000 | 258.90 | 531.41 | 618.39 | 814.03 | 3,123.73 | 275.91 | 100.0 (PASS) | 2.76 μs |
| | `Put` | 30,000 | 61.41 | 823.74 | 1,832.84 | 2,552.63 | 4,356.33 | 156.32 | 0 | — |
| | `DeleteRange` | 20,000 | 98.06 | 1,080.70 | 1,945.06 | 2,811.68 | 4,364.48 | 210.53 | 0 | — |
| **AMTV-T512** | `GetLive` | 40,000 | 22.11 | 61.43 | 82.19 | 123.96 | 581.12 | 26.50 | 0 | — |
| | `Scan-Intersect` | 5,000 | 226.02 | 548.55 | 666.30 | 841.24 | 3,684.68 | 262.51 | 50.0 (PASS) | 5.25 μs |
| | `Scan-NonIntersect` | 5,000 | 224.55 | 524.63 | 625.85 | 801.07 | 2,518.93 | 256.02 | 100.0 (PASS) | 2.56 μs |
| | `Put` | 30,000 | 123.40 | 818.24 | 1,492.49 | 2,614.63 | 3,490.61 | 209.17 | 0 | — |
| | `DeleteRange` | 20,000 | 172.78 | 1,159.38 | 1,609.40 | 2,770.13 | 3,664.30 | 270.82 | 0 | — |

### 4.2 Phase A (无墓碑基准期) 与 Phase C (静态存活读主导期)

- **Phase A (`Scan-PlannedIntersect` vs `Scan-NonIntersect`)**：
  - 4 组配置下，`Scan-PlannedIntersect` 的 P50 均为 **60 ~ 63 μs**，平均耗时 **68 ~ 71 μs**，逐条固定返回 100 键，有效单键开销约 **0.70 μs/key**；
  - `Scan-NonIntersect` 的 P50 均为 **44 ~ 45 μs**，平均耗时 **46 ~ 48 μs**，逐条固定返回 100 键，有效单键开销约 **0.47 μs/key**；
  - 证明在尚未注入 DeleteRange 时，两类 Scan 几何位置本身的寻道与前向读取底噪完全对称一致。
- **Phase C (动态写入停止，静态墓碑驻留)**：
  - **AMTV-T0**：GetLive P50 为 **7.77 μs** (P99: 19.75 μs)；`Scan-Intersect` P50 为 **54.80 μs**，`Scan-NonIntersect` P50 为 **50.82 μs**；
  - **Native-T0**：GetLive P50 为 **8.80 μs** (P99: 28.09 μs)；`Scan-Intersect` P50 为 **70.58 μs**，`Scan-NonIntersect` P50 为 **65.93 μs**；
  - 证实一旦写入停止，MemTable 视图处于静态缓存状态，原生物化与锁排队不再发生，两组点查均恢复至微秒级（7 ~ 9 μs）。

---

## 5. 引擎输出写放大与物理维护状态审计

### 5.1 三窗口 I/O 输出与写放大系数 (WAF)

依据评测协议，三窗口统计覆盖：W1（前台执行期）+ W2（10秒固定冷却期）+ W3（稳定静止排干期）。
写放大计算标准：以三窗口内引擎实际输出的 `Flush Bytes + Compaction Write Bytes` 除以前台写入理论量（60,000 Puts × 256 B = 15,360,000 B = 14.65 MiB）：

| 配置 | 三窗口 Flush 输出 (MB) | 三窗口 Compaction 读 (MB) | 三窗口 Compaction 写 (MB) | 引擎实际写输出 (MB) | **引擎层写放大系数 (WAF)** |
|:---|---:|---:|---:|---:|:---:|
| **Native-T0** | **0.00 MB** | 0.00 MB | 0.00 MB | **0.00 MB** | **0.00 ± 0.00** |
| **AMTV-T0** | **0.00 MB** | 0.00 MB | 0.00 MB | **0.00 MB** | **0.00 ± 0.00** |
| **Native-T512** | 1.54 ± 0.00 MB | 47.96 ± 4.54 MB | 47.81 ± 4.65 MB | 49.35 ± 4.65 MB | **3.37 ± 0.30** |
| **AMTV-T512** | 1.52 ± 0.01 MB | 42.66 ± 0.77 MB | 42.42 ± 0.86 MB | 43.94 ± 0.86 MB | **3.00 ± 0.06** |

> **关键物理边界纪律**：
> 1. **T0 组零写放大界定**：T0 组在观测窗口内不产生任何磁盘 Flush 与 Compaction，因而 WAF=0。但这**绝不等于永久物理维护成本为零**。20,000 条 DeleteRange 与新增 Put 此时全部驻留在原生活跃 MemTable 及 AMTV 多层跳表中，若后续系统关闭或触发自然内存 Flush，其刷写至 L0 及后续下推的 I/O 成本依然存在。
> 2. **T512 组 I/O 差异界定**：T512 组在 Phase B 触发了 38~39 次小文件刷盘，随后诱发了 L0->L1/L2 的级联 Compaction，带来了 ~43-49 MB 的额外写入。这一 I/O 开销是由墓碑阈值 Flush 这一端到端配置策略引起的，**不能解释为 AMTV 读路径算法带来的纯 I/O 消除收益**。

### 5.2 T512 代际 Flush 原因与墓碑恒等守恒

全部 5 轮中的 10 次 T512 运行（5 次 Native-T512，5 次 AMTV-T512）均严格审查了 `LOG` 文件中的代际事件日志：
- **触发原因 100% 为阈值 Flush**：全部 385 次 Flush 事件的 `flush_reason` 字段无一例外均为 `"Memtable Max Range Deletions"`，无任何自然内存满引起的容量 Flush；
- **严格代际墓碑守恒**：
  - **Native-T512**：5 轮稳定触发 **39 次 Flush**，已刷盘墓碑数稳定为 $19,978 \sim 19,994$，尾部活跃代残留墓碑数为 $6 \sim 22$ 条，二者求和**严格等于 20,000**；
  - **AMTV-T512**：5 轮稳定触发 **38 次 Flush**，已刷盘墓碑数稳定为 $19,492 \sim 19,513$，尾部活跃代残留墓碑数为 $487 \sim 508$ 条（均逼近 512 阈值），二者求和**严格等于 20,000**；
- **AMTV 收敛与回退**：AMTV 组在全部运行中 `fallback_events` 严格为 **0**，多层跳表 Run 守恒与归并状态机在各阶段屏障前均达到单调稳定收敛。

---

## 6. 机制深入分析与真实物理边界讨论

### 6.1 主机制比较：Native-T0 vs AMTV-T0

1. **点查路径直接瓶颈彻底绕开**：
   - 在动态注入期 Phase B 中，Native-T0 的 GetLive 延迟中位数高达 **4,025.50 μs**（P99: 11,389.80 μs），整机前台发生严重吞吐停滞（3,390 IOPS）；
   - AMTV-T0 的 GetLive 延迟中位数稳定在 **9.61 μs**（P99: 27.58 μs），延迟降低达 **99.76%**（中位数加速 **419 倍**）；
   - 证明在 Release 构建下，即便去除了所有 Audit 探针，AMTV 基于 `AMTVSnapshot` 和独立层级跳表的 GetOnly 旁路机制依然 100% 成立，点查完全绕开了原生活跃 MemTable 视图物化与互斥排队。

2. **原生 Scan 路径的真实硬件边界**：
   - AMTV 并未对 Scan 路径进行改造。在 Phase B 中，AMTV-T0 的单次 `Scan-Intersect` P50 为 **1,787.58 μs**，`Scan-NonIntersect` P50 为 **1,797.22 μs**；
   - 10,000 次 Scan 在各 Worker 线程侧耗时求和达：$10,000 \times 1.80\ \text{ms} \approx \mathbf{18.0\ \text{线程秒}}$；
   - 而其余 90,000 次前台操作（Get/Put/DeleteRange）各 Worker 线程侧耗时求和仅约 $\mathbf{2.5\ \text{线程秒}}$；
   - **事实口径界定**：Scan 仅占 10% 的前台操作样本，但在各 Worker 线程侧耗时求和中占到了 **87.8%**（约 88%）。原生 Scan 路径因必须调用原生动态分片墓碑迭代与范围墓碑推进，单次耗时稳定在 1.8 ms 量级，成为混合读负载下决定前台阶段墙钟耗时的主要耗时项。

3. **Scan 性能的间接改善及其边界**：
   - 在 Phase B 中，AMTV-T0 的 `Scan-Intersect` P50（1,787.58 μs）显著优于 Native-T0（4,186.11 μs），加速比约 **2.34x**；单键扫描开销从 Native-T0 的 92.92 μs/key 降至 36.02 μs/key；
   - 这一改善在因果上与“40,000 次 GetLive 绕开 `reader_mutex` 后，Scan 线程在尝试物化分片列表时的锁争用显著减轻”的机理假说高度一致；
   - 但它不是 Scan 算法本身的直接优化：Scan 依然产生了毫秒级的重构耗时，且相交 Scan 的每键开销（36.02 μs/key）依然远高于非相交 Scan（18.02 μs/key）与无墓碑时的基准 Scan（0.70 μs/key）。

### 6.2 端到端配置比较：Native-T512 vs AMTV-T512

1. **高频 Flush 掩盖了读路径锁冲突**：
   - 在 T512 配置下，每累积 512 条 DeleteRange 即触发一次强制 Flush，活跃 MemTable 中的墓碑数量被截断在 512 条以内，原生活跃物化开销大幅萎缩；
   - 因此，Native-T512 的 Phase B IOPS 达到 46,831 IOPS，AMTV-T512 为 51,180 IOPS，AMTV-T512 仅带来 **1.10x**（中位数 1.04x）的边际吞吐提升；
   - 这一现象证明：当系统采用激进的物理维护策略（频繁小文件 Flush）来抑制活跃 MemTable 墓碑累积时，读路径上的点查锁争用不再是全局第一瓶颈。

2. **维护开销的代价转移**：
   - T512 配置将读路径问题转化为了写放大与后台 Compaction 压力：三窗口写放大达 **3.00 ~ 3.37**，引发了 ~45 MB 的 Compaction 写入；
   - 相比之下，AMTV-T0 在三窗口内完全免除了这 39 次小文件刷盘和对应的级联 Compaction，在实现 37,248 IOPS（与 T512 处于同一量级）的同时，保持了零后台写放大。

### 6.3 M3b 与 M2d 敏感性对照

| 负载构成特性 | M2d Release (纯点查读) | M3b Release (10% 原生 Scan 混合读) | 物理机理与差异成因分析 |
|:---|:---:|:---:|:---|
| **前台读操作构成** | 190k GetLive (100% 点查) | 190k GetLive + 30k Scan (13.6% 扫描) | M3b 引入了未优化的原生 RocksDB Scan 迭代器 |
| **Native-T0 Phase B IOPS** | 3,285 ± 47 IOPS | 3,390 ± 43 IOPS | 无论是否有 Scan，Native-T0 均被活跃墓碑物化与读锁争用锁死在 ~3.3k IOPS |
| **AMTV-T0 Phase B IOPS** | 165,820 ± 4,120 IOPS | 37,248 ± 1,624 IOPS | AMTV-T0 吞吐由点查的 ~16.5 万下降至 ~3.7 万 IOPS |
| **Phase B 吞吐加速比** | **50.47x** | **10.99x (中位数 11.03x)** | 由阿姆达尔定律决定：未优化的原生 Scan 占用了 88% 的线程侧耗时 |
| **全流程 Overall 吞吐比** | **39.67x** | **9.70x (中位数 9.67x)** | 全流程包含 Phase A 基准与 Phase C 静态存活期 |
| **GetLive P50 (Phase B)** | 9.52 μs | 9.61 μs | AMTV 点查旁路效能完全恒定，未受原生 Scan 干扰 |

- **敏感性结论**：从纯点查转向包含 10% 原生 Scan 的混合读负载时，AMTV-T0 相对 Native-T0 的吞吐加速比从 **50.47 倍**收敛至 **10.99 倍**（中位数 11.03 倍）。这定量证实了：**在未改造 Scan 架构前，原生 Scan 的毫秒级耗时成为系统端到端吞吐的硬性上限**。

---

## 7. 对推进 M1c Scan 架构改造的决策依据

Release N=5 的真实硬件数据为是否推进 M1c 提供了决定性的量化结论：

1. **AMTV GetOnly 的职责边界已经彻底验证闭合**：
   - 点查路径从 4,025 μs 压缩至 9.6 μs，完全杜绝了视图物化与锁排队，在 Release 状态下完全闭环。
2. **混合读负载的决胜点完全落在 Scan 路径**：
   - Phase B 中，每次原生 Scan 耗时 1.78 ms，单键有效成本高达 36.02 μs/key（相比基准期的 0.70 μs/key 恶化了 51 倍）；
   - 即使在写入完全停止的 Phase C，`Scan-Intersect` 的单次耗时（54.80 μs）依然高出非相交扫描与基准扫描；
   - 混合负载下各 Worker 耗时求和中，Scan 占用了 88% 的时间，阿姆达尔定律明确指明：**若不解决 Scan，系统在混合读负载下的吞吐上限将被死死锁定在 3.7 万 ~ 5 万 IOPS**。
3. **M1c Scan 架构的设计使命明确批准**：
   - M1c 必须基于 AMTV 已有的多层有序墓碑切片，向迭代器输出免物化、免锁、单调前进的墓碑分片流，将 Scan-Intersect 的耗时从 1.8 ms 压缩至数十微秒级，从而释放混合负载下数十万 IOPS 的全部潜力。

---

## 8. 硬停止条件核验与数据产物清单

### 8.1 全部 8 项硬停止条件 100% PASS

1. **500K 候选空间状态与 SHA 一致性**：20 轮全部 PASS，每轮 300k Live / 200k Deleted 逐键比对 100% 正确；同一 Seed 下的四组配置 SHA-256 达到 100% 逐 bit 一致：
   - Seed `610001` (Rep 1): `e454b599b954d690780e3fff696a0d98e411f238e33af9dd2aff563ee365adef`
   - Seed `620001` (Rep 2): `c613e928a006b0c03a6f181ac7a20a2ff6571b9587809eccd2ea24b8f4e28842`
   - Seed `630001` (Rep 3): `3725f335dd2269f3f22b4f6649f58673610ddb4e1eeced9713fb6b7f3d4409ab`
   - Seed `640001` (Rep 4): `af12d73abfff4d37b6281f844da40f3fc52252a3482887d7c0c0c640263124fc`
   - Seed `650001` (Rep 5): `229e12c5fb2fdd99fc147f088c30590423628234b458513f7e5b83215127d4bf`
2. **Scan 可见键数校验**：全部 600,000 次 Scan 逐条严格断言，相交返回 50 键，非相交返回 100 键，全流程 50,000,000 个可见键 100% 精确匹配；
3. **AMTV Fallback**：10 轮 AMTV 运行中 fallback 次数严格为 **0**；
4. **T0 自然容量 Flush**：10 轮 T0 运行中 flush_count 严格为 **0**；
5. **T512 非阈值 Flush**：10 轮 T512 运行中 385 次 flush 全部为 `"Memtable Max Range Deletions"`，无任何非阈值 Flush；
6. **T512 墓碑恒等守恒**：10 轮 T512 运行中已刷墓碑数与活跃代残留墓碑数之和严格为 **20,000**；
7. **AMTV Run 守恒与状态机收敛**：各阶段屏障前及收敛排干窗口内状态机检查均 PASS；
8. **数据字段完整性**：20 轮 JSON 文件字段 100% 完整无缺失。

### 8.2 产物归档清单

1. **测试驱动与编译配置**：
   - 驱动实现：[`tools/amtv_m3a/m3a_driver.cc`](file:///home/wam/grad/s14-range-delete-study/tools/amtv_m3a/m3a_driver.cc)
   - Makefile：[`tools/amtv_m3a/Makefile`](file:///home/wam/grad/s14-range-delete-study/tools/amtv_m3a/Makefile)
   - Release 二进制：[`bin/m3a_driver_release`](file:///home/wam/grad/s14-range-delete-study/bin/m3a_driver_release)
2. **Trace 脚本与数据集**：
   - 生成脚本：[`scripts/m3a/generate_m3b_release_traces.py`](file:///home/wam/grad/s14-range-delete-study/scripts/m3a/generate_m3b_release_traces.py)
   - 静态审计脚本：[`scripts/m3a/audit_m3b_release_traces.py`](file:///home/wam/grad/s14-range-delete-study/scripts/m3a/audit_m3b_release_traces.py)
   - 5 套数据集：`traces/m3b_rel_rep{1..5}_seed{seed}/`
3. **执行与聚合脚本**：
   - 调度矩阵：[`scripts/m3a/run_m3a_release.py`](file:///home/wam/grad/s14-range-delete-study/scripts/m3a/run_m3a_release.py)
   - 数据聚合：[`scripts/m3a/aggregate_m3b_release.py`](file:///home/wam/grad/s14-range-delete-study/scripts/m3a/aggregate_m3b_release.py)
4. **数据汇总 CSV**：
   - 吞吐汇总与 IQR：[`results/amtv_m3b/m3b_release_n5_throughput_summary.csv`](file:///home/wam/grad/s14-range-delete-study/results/amtv_m3b/m3b_release_n5_throughput_summary.csv)
   - 逐阶段逐操作明细：[`results/amtv_m3b/m3b_release_n5_per_phase_ops.csv`](file:///home/wam/grad/s14-range-delete-study/results/amtv_m3b/m3b_release_n5_per_phase_ops.csv)
   - 逐轮配对差值与比率：[`results/amtv_m3b/m3b_release_n5_paired_diffs.csv`](file:///home/wam/grad/s14-range-delete-study/results/amtv_m3b/m3b_release_n5_paired_diffs.csv)
   - T512 逐代 Flush 明细：[`results/amtv_m3b/m3b_release_n5_t512_generations.csv`](file:///home/wam/grad/s14-range-delete-study/results/amtv_m3b/m3b_release_n5_t512_generations.csv)
   - 调度清单：[`results/amtv_m3b/m3b_matrix_manifest.json`](file:///home/wam/grad/s14-range-delete-study/results/amtv_m3b/m3b_matrix_manifest.json)
   - 全部 20 轮原始 JSON：`results/amtv_m3b/raw/m3b_release_*.json`
