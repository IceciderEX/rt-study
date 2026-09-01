# S3：受控回收前台时间序列实验报告 (notes/supplement/s3-reclamation-timeseries.md)

**实验时间**：2026-08-20  
**核心目标**：以秒级时间序列粒度检验物理回收（`CompactRange`）的“瞬时前台干扰”与“中长期收益”。  
**工作负载**：前台持续运行真实混合负载（Get 75%, Scan 20%, Put 5%），Zipfian ($\theta=0.99$) 分布，持续 60 秒（单轮超 1000 万次操作）。  
**控制变量**：在 $t=20\text{s}$ 由独立后台控制线程触发一次局部物理 Compaction（`s3_hot_reclaim` vs `s3_cold_reclaim` vs `s3_control`），每组独立重复 3 次（共 9 轮运行）。  
**数据来源**：`/home/wam/grad/s14-range-delete-study/results/summary/supplement/timeseries.csv`（按 1 秒间隔高频连续采样）。

---

## 一、实测全周期聚合对比表

| 实验组别 | 回收触发区域 | 总操作量 (60s) | 平均吞吐 (IOPS) | 物理丢弃键数 | Get(Aff) P99 (μs) | Scan P99 (μs) | **Scan 单位键开销 (μs/key)** | 累计 WriteStall |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| `s3_control` | 无回收 (对照基准) | $10.42\text{ M} \pm 119\text{k}$ | $172,134 \pm 1,734$ | 0 | $22.26 \pm 0.31$ | $368.10 \pm 10.93$ | $\mathbf{2.59 \pm 0.02}$ | 0 μs |
| `s3_hot_reclaim` | 热区 $[0, 200\text{k})$ | $10.68\text{ M} \pm 69\text{k}$ | $176,408 \pm 1,411$ | 400,000 | $21.89 \pm 0.09$ | $362.72 \pm 7.48$ | $\mathbf{2.51 \pm 0.002}$ | 0 μs |
| `s3_cold_reclaim` | 冷区 $[800\text{k}, 1\text{M})$ | $10.72\text{ M} \pm 47\text{k}$ | $177,572 \pm 1,755$ | 399,934 | $21.88 \pm 0.15$ | $353.53 \pm 5.51$ | $\mathbf{2.50 \pm 0.003}$ | 0 μs |

---

## 二、秒级时间序列动态分析 (t=0s ~ 60s)

从 `timeseries.csv` 的逐秒轨迹中可见：
1. **$t = 0 \sim 20\text{s}$ (回收前基线期)**：
   三组前台吞吐均稳定在 $170\text{k} \sim 175\text{k}\text{ IOPS}$，RangeScan P99 稳定在 $360 \sim 380\text{ μs}$。
2. **$t = 20 \sim 24\text{s}$ (CompactRange 执行瞬态期)**：
   - 后台线程发起对 200,000 键区间的异步 Compaction，产生单秒约 $150 \sim 200\text{ MB/s}$ 的瞬时写带宽；
   - **前台 P99 波动**：前台 Get P99 与 RangeScan P99 未发生阶跃式尖峰（无毫秒级停顿）；
   - **WriteStall 记录**：由于底层 NVMe SSD 写入吞吐能力极强（KIOXIA 1.8TB NVMe），前台写入完全未触发 Write Stall（增量为 0 μs）。
3. **$t = 25 \sim 60\text{s}$ (回收后稳态期)**：
   - 物理丢弃 40 万无效键后，SST 数据量从 ~760MB 压缩固化；
   - 后续 RangeScan 的单位有效键遍历开销从 $2.59\text{ μs/key}$ 下降至 **$2.50 \sim 2.51\text{ μs/key}$**，整体前台 IOPS 小幅提升约 $+2.5\% \sim +3.2\%$。

---

## 三、三层论证体系

### 1. 可确认的事实 (Confirmed Facts)
1. **物理空间与墓碑丢弃真实发生**：`CompactRange` 成功触发了底层 SST 重写并丢弃了 400,000 个无效点数据与范围墓碑。
2. **读性能改善**：回收后 RangeScan 单位有效键开销稳定改善约 $3.5\%$，吞吐提升约 $2.5\% \sim 3.2\%$。
3. **未见前台阻塞尖峰**：在当前 8 线程并发、5% 写入强度的 1GB 负载与高性能 NVMe 硬件环境下，**回收期间未观察到显著的前台 P99 恶化尖峰或 Write Stall 阻塞**。

### 2. 可支持但仍需谨慎的解释 (Plausible Interpretations)
1. **当前工况下的结论限定**：
   依据严格判定准则，**“物理回收存在收益，当前工况与硬件配置下未观察到显著前台干扰”**。
2. **硬件缓冲效应**：高性能 NVMe 的高速并发通道与当前未超载的前台写入速率，使得后台 Compaction I/O 得以被硬件队列平滑吸收。

### 3. 不能由当前数据得出的结论 (Unsupported Conclusions)
1. **不能得出“在任何生产场景下 Compaction 都不会产生前台干扰”的结论**：当写入压力极高（如 50% Put）、存储介质带宽饱和或存在多层级大文件级联 Compaction 时，前台干扰依然可能存在。
