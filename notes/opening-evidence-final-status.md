# 开题验证终态证据与全量实验清单审计报告 (notes/opening-evidence-final-status.md)

**报告时间**：2026-08-21 11:46  
**测试平台**：`s14.servers.hustpdsl.cn` (80 vCPUs, 78GB RAM, KIOXIA EXCERIA PRO NVMe SSD)  
**RocksDB 版本**：官方 Tag `v11.8.0`（commit `abeebd9630f11bd08c28b7bd43c7bdfc62050654`）  
**实测规模总述**：已累计完成 **320 余轮（精确为 325 轮）生命周期实测**，全部通过全库 64 位 SHA-256 逐比特对账。

---

## 一、全量实验轮数逐项精确对账清单 (共 325 轮)

依据磁盘上所有实验组别原始数据清单，全量 25 个子测试集逐项累加清单如下：

$$\begin{aligned}
\text{总轮数} &= \underbrace{(1 + 18 + 12 + 6 + 9 + 9 + 9 + 18 + 18 + 39)}_{\text{P0~P8 与 S1~S4 基础探索: 139 轮}} \\
&+ \underbrace{(18 + 24 + \mathbf{4} + 63 + 9 + 12 + 12)}_{\text{E1~E4 多维验证与 B1~B2 阶段基线: 142 轮}} \\
&+ \underbrace{(9 + 1 + 9 + 9 + 3 + 1 + 9 + 3)}_{\text{反事实、写密集与 24GiB 大规模实测: 44 轮}} \\
&= 139 + 142 + 44 = \mathbf{325\text{ 轮}}
\end{aligned}$$

> **文字对账说明**：此前粗分类中 E1~E4 将 118 轮（含 E2-Sanity 4 轮）笔误记为 114 轮，导致子项求和出现 277 + 44 = 321 的表象差异。如下表所示，单独列出 **E2-Sanity（4 轮）** 后，各子项精确累加和恒等于 **325 轮**。为保证学术报告严密性，主叙述统一采用 **“已完成 320 余轮生命周期实测”**。

| 序号 | 实验阶段与套件名称 | 实测轮数 | 包含内容与测试用途 | 数据归档路径 |
| :---: | :--- | :---: | :--- | :--- |
| 1 | **P0 冒烟基线 (Smoke Test)** | 1 | 系统环境与驱动冒烟功能校验 | `results/summary/smoke-test.csv` |
| 2 | **P1 比例扫描基线 (P1 Ratio)** | 18 | 6 档 DeleteRange 比例 $\times$ 3 重复 | `results/summary/p1_summary.csv` |
| 3 | **P2 墓碑规模基线 (P2 Tombstone)**| 12 | 4 档墓碑数量 $\times$ 3 重复 | `results/summary/p2_summary.csv` |
| 4 | **P3 组合负载基线 (P3 Comp)** | 6 | 2 档读写比例 $\times$ 3 重复 | `results/summary/p3_summary.csv` |
| 5 | **P4 局部性分布 (P4 Locality)** | 9 | 3 档访问分布 $\times$ 3 重复 | `results/summary/p4_summary.csv` |
| 6 | **P5 压缩策略矩阵 (P5 Compaction)**| 9 | 3 档 Compaction 选项 $\times$ 3 重复 | `results/summary/p5_summary.csv` |
| 7 | **P6 动态阶段原型 (P6 Dynamic)** | 9 | 3 档阶段控制 $\times$ 3 重复 | `results/summary/p6-dynamic-origin/all-runs.csv` |
| 8 | **P7 P1 协议重放 (P7 Replay)** | 18 | 6 档原生重放 $\times$ 3 重复 | `results/summary/p7-p1-replay/all-runs.csv` |
| 9 | **P8 下刷预言机 (P8 Oracle)** | 18 | 6 档下刷干预 $\times$ 3 重复 | `results/summary/p8-flush-oracle/all-runs.csv` |
| 10 | **S1~S4 补充矩阵 (Supplement)** | 39 | 13 个多维补充配置 $\times$ 3 重复 | `results/summary/supplement/all-runs.csv` |
| 11 | **E1 跨度矩阵 (E1 Span Matrix)** | 18 | 6 档跨度 $\times$ 3 重复 | `results/summary/e1_all_runs.csv` |
| 12 | **E2 Value 大小矩阵 (E2 ValSize)** | 24 | 8 档 Value 大小 $\times$ 3 重复 (32B~1024B) | `results/summary/e2_all_runs.csv` |
| 13 | **E2-Sanity 极值校验 (E2 Sanity)** | **4** | **4 档极值验证 (Value=8B/16B/2048B/4096B 各 1 轮)** | `results/summary/e2_sanity_summary.csv` |
| 14 | **E3 动态多阶段矩阵 (E3 Dynamic)** | 63 | 21 档阈值/负载矩阵 $\times$ 3 重复 | `results/summary/e3_all_runs.csv` |
| 15 | **E4 MemTable 预算矩阵 (E4 Budget)**| 9 | 3 档写缓冲预算 $\times$ 3 重复 | `results/summary/e4_all_runs.csv` |
| 16 | **B1 自然下刷边界 (B1 Natural)** | 12 | 4 档 Put 写入量边界 $\times$ 3 重复 | `results/summary/b1_all_runs.csv` |
| 17 | **B2 动态三阶段 (B2 Dynamic)** | 12 | 4 档静态阈值 $\times$ 3 重复 | `results/summary/b2_all_runs.csv` |
| 18 | **B1-CF 反事实实验 (B1-CF)** | 9 | 3 档写缓冲容量 $\times$ 3 重复 (同请求流/同终态) | `results/summary/b1_cf_all_runs.csv` |
| 19 | **LS24 Pilot 试跑 (LS24 Pilot)** | 1 | 24GiB 稀释版大规模试跑预检 | `results/summary/ls24_pilot_summary.csv` |
| 20 | **LS24 正式矩阵 (LS24 Formal)** | 9 | 3 档阈值 $\times$ 3 重复 (24GiB 稀释版) | `results/summary/ls24_all_runs.csv` |
| 21 | **WB-CleanWrite (WB Clean)** | 9 | 3 档写缓冲容量 $\times$ 3 重复 (写密集无删除) | `results/summary/wb_cleanwrite_all_runs.csv` |
| 22 | **LS24-Clean (LS24 Clean)** | 3 | 3 重复无删除基准 (24GiB 稀释版原轨迹) | `results/summary/ls24_clean_all_runs.csv` |
| 23 | **LS24-DP Pilot (DP Pilot)** | 1 | 40% 覆盖率前置验收试跑 (24GiB) | `results/summary/ls24_dp_pilot_summary.csv` |
| 24 | **LS24-DP Formal (DP Formal)** | 9 | 3 档阈值 $\times$ 3 重复 (24GiB 40% 覆盖率) | `results/summary/ls24_density_all_runs.csv` |
| 25 | **LS24-DP-Clean (DP Clean)** | 3 | 3 重复严格同轨迹无删除基准 (24GiB) | `results/summary/ls24_density_clean_all_runs.csv` |
| **合计** | **全量实测总计** | **325** | **全生命周期实测（1 冒烟 + 324 正式与对照）** | **组内 100% 通过 SHA-256 逐比特对账** |

---

## 二、四大新增补强任务终态核心结论

### 1. 指标口径统一审计 ([notes/final-metric-definition-audit.md](file:///home/wam/grad/s14-range-delete-study/notes/final-metric-definition-audit.md))
- 补齐 B1-CF 9 轮后台代价：16MiB 组改善读性能伴随 **13.33 MB 物理 Flush 写入**与 **+14.33 MB SST 空间增量**（PWA=0.417），64MiB/128MiB 组为 0 MB，确立 16MiB 并非无代价；
- 明确区分全量原始样本全局 P99 与分阶段快照聚合 P99；统一真实阶段吞吐为 $\text{True Phase IOPS} = \Delta \text{ops} / \Delta t$。

### 2. WB-CleanWrite：写密集无删除实验 ([notes/wb-cleanwrite-summary.md](file:///home/wam/grad/s14-range-delete-study/notes/wb-cleanwrite-summary.md))
- 实测证明：在 256MB 写密集负载下，16MiB 写缓冲引发 **19 次 Flush、574.0 MB Compaction 写入、PWA 达 3.26x 并产生 69.3ms 写停顿**（64MiB/128MiB 为 0 停顿）；
- 确立了静态调小写缓冲不能作为通用的静态配置解。

### 3. LS24-Clean：24GiB 原轨迹基准 ([notes/ls24-clean-summary.md](file:///home/wam/grad/s14-range-delete-study/notes/ls24-clean-summary.md))
- 测定 24GiB 原轨迹纯净无删除基线为：Scan 开销 **$1.17\text{ μs/key}$**，Get P99 **$127.3\text{ μs}$**，Put P99 **$44.1\text{ μs}$**；
- 量化了 20,000 个删除未 Flush 时造成的退化倍数（点查恶化 101.0 倍，扫描恶化 22.2 倍）。

### 4. LS24-DensityPreserved：24GiB 40% 覆盖率实测与同轨迹 Clean 对照 ([notes/ls24-density-preserved-summary.md](file:///home/wam/grad/s14-range-delete-study/notes/ls24-density-preserved-summary.md))
- **前置验收通过**：实际并集覆盖 **39,956,234 键（39.69% 覆盖率，符合 40% $\pm$ 1% 验收标准）**，区间重叠率 **0.00%**，点查删除命中率 39.76%，范围扫描重叠率 41.89%；
- **同轨迹 Clean 对照数据**：
  - **DP-Clean 基准**：Scan 开销 **$0.79 \pm 0.01\text{ μs/key}$**，Scan P99 **$113.8\text{ μs}$**，Get P99 **$23.5\text{ μs}$**，Put P99 **$45.5\text{ μs}$**，组内 SHA-256 为 `dffbefb4...` (PASS)；
  - **$T=0$ (默认关闭)**：Scan 开销退化至 **$8.54\text{ μs/key}$**（**Phase B 暴增至 $45.68\text{ μs/key}$，较同阶段 Clean 恶化 58.56 倍**），Scan P99 恶化至 $9.67\text{ ms}$（Phase B 达 $12.42\text{ ms}$），Get(Del) P99 恶化至 $7.98\text{ ms}$（Phase B 达 $12.22\text{ ms}$，较 Clean 恶化 461 倍），组内 SHA-256 为 `24be8bee...` (PASS)；
  - **$T=256$ (折中阈值)**：Scan 开销恢复至 $1.50\text{ μs/key}$，Get(Del) P99 降至 $166.5\text{ μs}$，物理丢弃 31.6M 键（SST 缩减 7.82GB）；**但产生 74.7 次 Flush、16.04 GB Compaction 写入，PWA 达 939.5x，Phase B Put P99 高达 8.33 ms**；
  - **$T=2048$ (保守阈值)**：全程吞吐 **136.6 kIOPS**，Put P99 仅 **$163.7\text{ μs}$**；**但点查删除尾延迟增加至 $357.7\text{ μs}$（为 T=256 的 2.15 倍）**，且少回收 7.34M 键。
