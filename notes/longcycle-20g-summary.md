# FormalV2-LongCycle-20GiB：Pilot 验收报告与正式实验评估

**实验名称**：`FormalV2-LongCycle-20GiB：动态混合负载下范围删除全生命周期验证`  
**验证状态**：`Pilot 阶段验收 100% 通过（T0 与 T512 双轮逐比特对账一致）`  
**报告日期**：2026-08-24  

---

## 一、Pilot 实验验收结果与实测数据

在 10% 缩放 Pilot 规模（$2.56\text{M}$ 总写入 Key，5,000 条 DeleteRange，终态 $2.048\text{M}$ 存活 Key，约 $2.1\text{GiB}$ 逻辑数据）下，完成了 `LC20-PILOT-T0` 与 `LC20-PILOT-T512` 两轮独立全流程验证：

| 指标项 | LC20-PILOT-T0 | LC20-PILOT-T512 | 对账一致性与判定 |
| :--- | :---: | :---: | :---: |
| **墓碑阈值 (T)** | `T=0`（立即旁路） | `T=512`（基准） | 机制差异受控 |
| **总前台操作数** | 5,627,840 ops | 5,627,840 ops | 100% 确定性 Trace |
| **前台执行耗时 / IOPS** | 30.22s / 186,236 IOPS | 35.25s / 159,678 IOPS | 混合负载稳定高吞吐 |
| **Cooldown 耗时** | 60.0s | 60.0s | 后台静置观察充分 |
| **终态物理磁盘占用** | **2.26 GB** (2,255,255,145 B) | **2.28 GB** (2,278,240,457 B) | 符合 2GiB 预期 |
| **全库可见 Key 数量** | **2,048,000** | **2,048,000** | 精确删除 512,000 (20.00%) |
| **终态全库 SHA-256** | `b194850c0e644b23a90d8bfc295a91b68eabc9f226c975be30b709887ccdf08a` | `b194850c0e644b23a90d8bfc295a91b68eabc9f226c975be30b709887ccdf08a` | **100% 逐比特一致通过** |
| **单轮总墙上时间** | 98.4s (1.64 min) | 103.6s (1.73 min) | 自动化流水线畅通 |

---

## 二、关键机制与 LSM 分层审计

1. **成熟 Key 语义与零覆写审计**：
   - 静态 Trace 扫描确证：Put 在 DeleteRange 之后覆写已被删除 Key 的次数为 **0**；
   - 动态执行期间，范围墓碑完全覆盖先前的历史成熟 Key，无任何未来数据被意外覆盖。
2. **LSM 分层与 Compaction 墓碑消除**：
   - RocksDB 原生 Dynamic Level Base 机制运作正常；
   - RocksDB 日志确证 Compaction 将 L0 文件逐层合并下推（输出状态 `[2, 0, 0, 0, 0, 3, 32]`），并在合并过程中成功记录 `records dropped: 68004`，确证范围墓碑跨层传播并有效清理底层失效数据。

---

## 三、20GiB 正式实验资源与运行评估

根据 Pilot 实测数据，折算 20GiB 正式实验（$10\times$ 规模）核心指标如下：

### 1. 单轮预计耗时
- **前台 4 阶段混合负载**：约 5.5 ~ 6.5 分钟（56.3M 操作，约 150k~180k IOPS）；
- **后台 Cooldown 观察期**：10.0 分钟（600 秒）；
- **全库 20.48M 键值对账**：约 40 ~ 50 秒；
- **单轮总耗时**：约 **17 ~ 18 分钟**。

### 2. 单轮物理占用
- **终态物理占用**：约 **22.6 GB ~ 25.0 GB**；
- **峰值临时物理占用**（Phase C 剧烈写压与多层 Compaction 临时 SST 并存期）：单轮预计最高峰 **32 ~ 36 GB**。

### 3. 核心 9 轮预计总写入量
- **单轮前台写入量**：$25.6\text{M} \times 1\text{KB} + 10.45\text{M} \times 1\text{KB} \approx 36.05\text{ GB}$；
- **9 轮核心矩阵前台累计写入**：$9 \times 36.05\text{ GB} = \mathbf{324.45\text{ GB}}$；
- 结合 LSM 写放大（WA $\approx 2.5 \sim 3.3$），底层 SSD 预计物理写入约 850 ~ 1100 GB。

### 4. 可用磁盘安全余量
- **当前可用磁盘**：**614 GiB**（分区总容量 916 GiB，当前使用率仅 29%）；
- **9 轮全部保留终态 DB 占用**：$9 \times 23\text{ GB} \approx 207\text{ GB}$；
- **9 轮完成后可用余量**：$\approx \mathbf{407\text{ GiB}}$（使用率 $\le 55\%$），远高于 $\ge 200\text{ GiB}$ 且使用率 $\le 70\%$ 的安全红线。

---

## 四、正式运行方案与交错执行矩阵

### 1. 9 轮正式随机交错顺序
为了杜绝宿主机随时间推移产生的热量积聚与性能漂移，9 轮核心实验（CLEAN $\times 3$, T0 $\times 3$, T512 $\times 3$）按以下随机交错序列执行：

```
Run 1: LC20-R1-T0       (T=0,    clean=false)
Run 2: LC20-R2-CLEAN    (T=0,    clean=true)
Run 3: LC20-R3-T512     (T=512,  clean=false)
Run 4: LC20-R4-CLEAN    (T=0,    clean=true)
Run 5: LC20-R5-T0       (T=0,    clean=false)
Run 6: LC20-R6-T512     (T=512,  clean=false)
Run 7: LC20-R7-T512     (T=512,  clean=false)
Run 8: LC20-R8-T0       (T=0,    clean=false)
Run 9: LC20-R9-CLEAN    (T=0,    clean=true)
```

### 2. 执行与分析流水线
1. 生成 20GiB Full Trace（耗时约 35 秒）：
   ```bash
   python3 scripts/longcycle-20g/generate_longcycle_trace.py --scale full
   ```
2. 调度执行 9 轮全矩阵（总耗时约 2.6 ~ 2.8 小时）：
   ```bash
   python3 scripts/longcycle-20g/run_longcycle_matrix.py
   ```
3. 自动生成多维度分析图表与学术评估报告（`results/formal_v2/longcycle_20g/analysis_report.md`）。
