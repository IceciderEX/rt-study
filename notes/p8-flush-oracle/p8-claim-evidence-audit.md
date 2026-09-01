# P8：论断与证据链审计备忘 (notes/p8-flush-oracle/p8-claim-evidence-audit.md)

**审计生成时间**：2026-08-20  
**审计目标**：对照开题立论需求与科学边界，核查 P8 全部论断是否具备铁证支持，清理过度引申与未证实表述。

---

## 一、RocksDB Ticker 计数口径审计说明

1. **`COMPACT_READ_BYTES` 审计**：
   - 源码查验确认：`COMPACT_READ_BYTES` 来源于 RocksDB `IOSTATS(bytes_read)`，受 OS PageCache 命中与 direct I/O 状态影响，并不等同于参与 Compaction 的全部 SST 输入文件的逻辑总大小；
   - 查验底层日志（`LOG` 的 `EVENT_LOG_v1`）确认：Compaction 实际将 L0 中的 4 个 SST 文件（131.9 MB）作为输入，合并输出为 2 个 SST 文件（87.9 MB）；
2. **证据链纪律**：
   - **不使用存在口径差异的未标定字节数作为核心依据**；
   - **完全优先以四大确定性硬证据立论**（终态 SST 物理体积瘦身、物理丢弃键数、全库终态 SHA-256 对账、前台分阶段延迟实测）。

---

## 二、核心结论论断与证据链映射 (基于四大确定性硬证据)

| 序号 | 论断表述 (Claim) | 支持证据与原始文件 (Evidence) | 证据强度评级 | 结论边界与使用限定 |
| :---: | :--- | :--- | :---: | :--- |
| **C1** | 在 50% 进度点强制 Flush 显著改善了后半程（Phase C）读吞吐与扫描性能。 | `results/summary/p8-flush-oracle/all-runs.csv`<br>(10% 组 Phase C 吞吐从 1,029 IOPS 提升至 4,631 IOPS，提升 4.5 倍；Scan 开销下降 77.6%) | **STRONG (完全证实)** | 证实活跃 MemTable 中未落盘范围墓碑的累积是直接导致前台读长尾恶化的主要瓶颈。 |
| **C2** | Flush 执行后触发了后台 Compaction 并实现了物理键回收与 SST 瘦身。 | `COMPACTION_KEY_DROP_RANGE_DEL` 计数（10% 组丢弃 42.95 万键）与全库 SST 物理体积（130MB $\rightarrow$ 18MB） | **STRONG (完全证实)** | 证明墓碑下推至磁盘成功激活了物理垃圾回收机制。 |
| **C3** | `default` 与 `oracle-flush` 在相同请求流下达成 100% 数据一致性。 | `all-runs.csv` 全库 SHA-256 校验和对账 (2% 组 `d3dc02...`, 5% 组 `9523e...`, 10% 组 `0b87d...` 100% PASS) | **STRONG (完全证实)** | 证明 Flush 与 Compaction 未引入任何数据丢失或逻辑错误。 |
| **C4** | Flush 切换期间读未出现异常长尾，写出现毫秒级短暂保护。 | Phase B 细粒度时间序列分析（Scan/Get P99 与默认组持平，Put P99 出现 2.2~15.5ms 短暂峰值） | **STRONG (完全证实)** | 量化了 Flush 操作的前台瞬时权衡代价。 |

---

## 三、红线禁止与规范表述清单

1. **关于机制优化与系统表述**：
   - ❌ 严禁使用：“P8 已经证明定期 Flush 是范围删除的最佳优化方案”；
   - ✅ 规范使用：“P8 验证了消除活跃 MemTable 内部未落盘范围墓碑的累积能带来显著的读性能恢复，为设计范围墓碑感知型生命周期管理机制提供了经验支撑”。
2. **关于普适性引申**：
   - ❌ 严禁使用：“所有 RocksDB 范围删除场景都应当更早触发 Flush”；
   - ✅ 规范使用：“在当前 8 线程、高删除比例与无 Compaction 吸收的特定工况下，Flush 预言机展现出显著的读恢复收益”。
