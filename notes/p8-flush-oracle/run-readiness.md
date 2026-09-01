# P8：范围墓碑压力下的 Flush 预言机验证实验准入报告 (notes/p8-flush-oracle/run-readiness.md)

**生成时间**：2026-08-20  
**实验机器节点**：`s14.servers.hustpdsl.cn` (80 vCPUs, 78GB RAM)  
**底层存储介质**：`/dev/nvme0n1p2` (KIOXIA EXCERIA PRO 1.8TB NVMe SSD, ext4)  
**RocksDB 版本**：纯净官方 Tag `v11.8.0` (commit `abeebd9630f11bd08c28b7bd43c7bdfc62050654`, Release `-O2`)  
**当前状态**：**所有 Trace 请求脚本复用已核验，P8 分阶段统计与 Flush 预言机专属驱动已编译完成，配置文件与执行调度脚本已就绪，处于严格冻结状态，等待您的确认指令后再行启动 18 轮正式执行**。

---

## 一、核心研究假设与实验边界

### 1.1 核心研究假设
> 当高密度 DeleteRange 使范围墓碑滞留在活跃 MemTable 并导致读长尾延迟恶化时，若在固定逻辑进度点强制执行一次原生 `Flush()`，将墓碑下推至 SST 层，**后续 Get/RangeScan 性能是否能恢复？恢复收益是否伴随可量化的 Flush 代价？**

### 1.2 严格边界与方法声明
- 本实验**不是最终优化方案**，不修改 RocksDB 源码，不实现自动 Flush 调度逻辑；
- 仅使用原生一次性 `Flush()` 作为**科学预言机（Oracle）**，验证“消除活跃 MemTable 中的墓碑堆积能否直接解除前台读阻塞”这一关键机制假设；
- 严禁使用 `CompactRange()`，严禁在 Main 阶段中二次 Flush。

---

## 二、原 P7 与 P8 实验参数逐项一致性对照表

| 配置参数项 | 原 P7 冻结参数 | P8 实验参数 | 一致性状态 |
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

## 三、实验矩阵、Flush 触发点与 Trace SHA-256 固化清单

### 3.1 实验矩阵与 Flush 触发规则 (18 轮)
对 2.0%、5.0%、10.0% 三种已证实退化的比例，分别设立 `default` 与 `oracle-flush` 对照组：

| 组别代号 | 比例 | 预留 DeleteRange 总数 | Flush 触发策略与触发点 | 重复轮次 |
| :--- | :---: | :---: | :--- | :---: |
| `p8-default-ratio-020` | 2.0% | 4,000 | 不执行 Flush (完全继承 P7) | r01 ~ r03 |
| `p8-flush-ratio-020` | 2.0% | 4,000 | 累计完成**第 2,000 个** DeleteRange 时独立线程触发一次 Flush() | r01 ~ r03 |
| `p8-default-ratio-050` | 5.0% | 10,000 | 不执行 Flush (完全继承 P7) | r01 ~ r03 |
| `p8-flush-ratio-050` | 5.0% | 10,000 | 累计完成**第 5,000 个** DeleteRange 时独立线程触发一次 Flush() | r01 ~ r03 |
| `p8-default-ratio-100` | 10.0% | 20,000 | 不执行 Flush (完全继承 P7) | r01 ~ r03 |
| `p8-flush-ratio-100` | 10.0% | 20,000 | 累计完成**第 10,000 个** DeleteRange 时独立线程触发一次 Flush() | r01 ~ r03 |

### 3.2 请求流与二进制 SHA-256 校验和
| 文件名称 | 路径 | 规格与操作数 | SHA-256 Checksum |
| :--- | :--- | :--- | :--- |
| **2.0% Trace** | `traces/p7/p7_trace_ratio_020.bin` | 200k Ops (Del: 4k, Get: 146k, Scan: 40k, Put: 10k) | `fb85c2685b0115c1743f8aec57ba122722a9a105cede8f0f58c28e7b242d046f` |
| **5.0% Trace** | `traces/p7/p7_trace_ratio_050.bin` | 200k Ops (Del: 10k, Get: 140k, Scan: 40k, Put: 10k) | `78507e52afa6bc96b6e8facf73ccd8640fb2361ce226ad85b8031665ed3269d5` |
| **10.0% Trace** | `traces/p7/p7_trace_ratio_100.bin` | 200k Ops (Del: 20k, Get: 130k, Scan: 40k, Put: 10k) | `8768b01911933478aba0da92a30d205ec179c46cd1fc4f509223b84f71c9d1e0` |
| **P8 专属驱动** | `bin/p8_driver` | C++20 Release 二进制程序 (`-O2 -std=c++20`) | `59d284f40365ff9d32adf231ec21ee204672f27dc8b8a297dded09028b6c9cf6` |

---

## 四、四阶段独立统计与监控架构

P8 驱动将 200,000 次操作划分为四个独立阶段分别统计：

```
0% ─────── Phase A (0%~45%) ───────► 45% ── Phase B (45%~55%) ──► 55% ────── Phase C (55%~100%) ──────► 100% ── Phase D (10s)
      Flush 前稳定累积阶段                 Flush 触发与执行窗口             Flush 后持续负载恢复阶段           后台收敛观察
```

1. **Phase A (0% ~ 45%，0 ~ 90,000 Ops)**：验证两组在 Flush 触发前性能完全一致；
2. **Phase B (45% ~ 55%，90,001 ~ 110,000 Ops)**：精确量化 Flush 引起的瞬时影响，记录最大 P99 延迟尖峰、写停顿和 Flush 耗时毫秒数；
3. **Phase C (55% ~ 100%，110,001 ~ 200,000 Ops)**：核心评估 Phase，量化墓碑下推至 SST 后读吞吐与扫描单位开销的恢复程度；
4. **Phase D (运行后 10 秒)**：观察后台 Compaction 收敛状态，不纳入主性能统计。

---

## 五、资源预算、执行时序与耗时估算

### 5.1 写入量与磁盘空间预算
- **单轮逻辑写入量**：Preload 128 MB + 前台 Put ~2.5 MB + Flush 生成 SST ~3 MB $\approx \mathbf{135\text{ MB}}$
- **18 轮总逻辑写入量**：$18 \times 135\text{ MB} \approx \mathbf{2.43\text{ GB}}$
- **磁盘占用峰值**：单轮运行占用 $\le \mathbf{1.0\text{ GB}}$（单轮运行后立即全量对账并清理，当前 NVMe 可用空间 **585 GB**，余量充裕率 > 99.8%）。

### 5.2 18 轮运行顺序与耗时预估
| 批次 | 组别与配置 | 轮次 | 单轮预计耗时 | 批次小计耗时 |
| :--- | :--- | :---: | :---: | :---: |
| **第 1 批** | `p8-default-ratio-020` | r01 ~ r03 | ~6s | ~18s |
| **第 2 批** | `p8-flush-ratio-020` | r01 ~ r03 | ~6s | ~18s |
| **第 3 批** | `p8-default-ratio-050` | r01 ~ r03 | ~27s | ~1.4 min |
| **第 4 批** | `p8-flush-ratio-050` | r01 ~ r03 | ~15s (Flush加速) | ~45s |
| **第 5 批** | `p8-default-ratio-100` | r01 ~ r03 | ~110s | ~5.5 min |
| **第 6 批** | `p8-flush-ratio-100` | r01 ~ r03 | ~45s (Flush加速) | ~2.3 min |
| **总计 18 轮正式运行** | - | **共 18 轮** | - | $\mathbf{\approx 10 \sim 12\text{ 分钟}}$ |

---

## 六、执行准入声明

- [x] 原 P7 与 P8 配置与 Trace Checksum 逐项核验一致
- [x] P8 四阶段统计与 Flush 预言机专属驱动已编译完成并通过校验（`bin/p8_driver`）
- [x] 全部 6 个冻结配置文件已就绪（`configs/p8-flush-oracle/`）
- [x] 自动化测试、数据聚合与矢量绘图脚本已就绪（`scripts/p8-flush-oracle/`）
- [x] **所有 P8 实验代码、配置与环境已处于冻结状态，等待您的确认指令后再行启动 18 轮正式执行**。
