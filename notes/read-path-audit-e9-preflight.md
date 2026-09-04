# E9-DIA 动态混合负载下的范围墓碑缓存失效频率审计预检报告 (E9-T0-Preflight)

## 0. 实验概述与约束边界

- **测试对象**：`E9-T0-Preflight`（T0 基线，N=1，8 Worker，完整 A/B/C 三阶段）；
- **前置原则**：
  - 不调整 V2-A 参数；
  - 不实现任何 AMTV 代码；
  - 不改变 RocksDB 正常的 Flush / Compaction 调度策略；
  - 审计仅用于读路径内部机制归因，不替代正式吞吐性能评测。
- **验证通过标准**：
  1. 8 个 Worker 在每个 Phase 均有完整快照；
  2. 恒等式 `sum(worker snapshots) == phase summary` 与 `sum(phase summaries) == run summary` 100% 精度成立；
  3. 终态数据模型、可见键数及全库 SHA-256 对账一致并通过；
  4. `audit_materialization_events.csv` 记录明细（实际记录 43,941 条事件）；
  5. 审计启用 vs 关闭同配置运行，完成前台耗时开销审计（开销仅为 +2.35%）；
  6. 无分类混入、迭代器状态全部为 OK、无丢弃样本。

---

## 一、 审计启用 vs 关闭运行开销核验

为验证 `-DROCKSDB_READ_PATH_AUDIT` 及 per-op delta 采样的探针开销，在完全相同的硬件环境与数据目录下分别执行了审计启用（`e9_t0_audit`）与关闭（`e9_t0_noaudit`）的单轮对比测试：

| 配置名称 | 审计开关状态 | Phase A 耗时 (s) | Phase B 耗时 (s) | Phase C 耗时 (s) | 前台有效总耗时 (s) | 平均 IOPS (ops/s) |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| `e9_t0_noaudit` | **DISABLED** | 0.4402 | 7.1373 | 11.7546 | **19.3321** | 15,518.2 |
| `e9_t0_audit`   | **ENABLED**  | 0.4089 | 7.0046 | 12.3732 | **19.7867** | 15,161.7 |
| **净差异 (Delta)** | - | -0.0313 s | -0.1327 s | +0.6186 s | **+0.4546 s (+2.35%)** | -2.30% |

> 在本次同配置单轮配对观察中，启用审计的前台耗时较关闭审计高 2.35%；该差异可能包含运行波动，故审计运行的吞吐与延迟仅用于机制归因，不替代正式性能比较。

---

## 二、 预检正确性与完整性核验

1. **多线程快照与求和恒等式**：
   - 8 个 Worker 在 Phase 0 / 1 / 2 均完整生成快照，无丢失 Worker；
   - 检查断言：
     $$\sum_{c} \text{WorkerOpDelta}[w][p][c] \equiv \text{WorkerTlsSnapshot}[w][p]$$
     $$\sum_{w=0}^{7} \text{WorkerSnapshot}[w][p][c] \equiv \text{PhaseSummary}[p][c]$$
     $$\sum_{p=0}^{2} \text{PhaseSummary}[p][c] \equiv \text{RunSummary}[c]$$
     全部校验通过，退出码为 0。
2. **正确性对账**：
   - **DB 存活键数**：`316,193`（模型预期：`316,193`，匹配率 100%）；
   - **DB 数据载荷**：`83.23 MB`；
   - **DB SHA-256**：`366c5e5b3e5c4e5558be1175b16f5e76b8c71be13bb8e607fae36492d09f1c34`；
   - **Model SHA-256**：`366c5e5b3e5c4e5558be1175b16f5e76b8c71be13bb8e607fae36492d09f1c34`；
   - **对账结果**：`>> PASS <<`。
3. **安全路径约束**：
   - DB 目录被严格解析并断言位于预注册的 `run-db/e9_dynamic_audit/<run_id>/` 根路径内；
   - 任何越界路径将被拒绝并直接中止。

---

## 三、 四份审计 CSV 数据产物核验

产物目录：`results/summary/e9_dynamic_audit/audit_enabled/`

### 3.1 `audit_run_summary.csv` (全貌)
```csv
exp_id,rep,total_ops,total_reads,materialized_reads,overall_materialization_rate_per_1k,lock_contended_reads,materialization_or_lock_affected_reads,overall_read_latency_ms,total_materialization_ms,total_lock_wait_ms,overall_materialization_and_lock_ratio,total_cache_invalidations,phase_a_materialization_ms,phase_b_materialization_ms,phase_c_materialization_ms,phase_a_materialization_or_lock_affected_reads,phase_b_materialization_or_lock_affected_reads,phase_c_materialization_or_lock_affected_reads,phase_a_invalidations,phase_b_invalidations,phase_c_invalidations,verification_status
e9_t0_audit,1,300000,190000,9091,47.8474,34850,43941,135971.7082,27860.4206,98607.4332,0.930105,10000,162.1678,13011.9806,14686.2722,7928,20013,16000,1200,6000,2800,PASS
```

### 3.2 `audit_phase_summary.csv` 表头与前 10 行
```csv
exp_id,rep,phase,op_class,op_count,total_latency_ms,p50_us,p95_us,p99_us,materialized_ops,materialization_rate_per_1k,lock_contended_ops,materialization_or_lock_affected_reads,affected_p50_us,affected_p95_us,affected_p99_us,view_materialization_ms,lock_wait_ms,materialization_and_lock_ratio,active_mem_prep_ms,active_mem_lookup_ms,imm_mem_prep_ms,imm_mem_lookup_ms,active_mem_iter_construct_ms,imm_mem_iter_construct_ms,sst_iter_construct_ms,reseek_count,boundary_advance_count,child_next_count,covered_skip_count,cache_invalidation_count
e9_t0_audit,1,0,GetLive,58535,1171.9198,6.09,116.50,249.37,883,15.0850,4883,5766,117.42,309.40,401.01,120.9275,620.7759,0.632896,761.9491,17.7756,0.0000,0.0000,0.0000,0.0000,0.0000,0,0,0,0,0
e9_t0_audit,1,0,GetDeleted,1465,32.3249,2.46,174.62,315.27,23,15.6997,132,155,169.52,384.36,407.13,4.2360,23.8295,0.868233,28.6124,0.5914,0.0000,0.0000,0.0000,0.0000,0.0000,0,0,0,0,0
e9_t0_audit,1,0,ScanIntersect,2651,194.5838,54.49,206.04,349.48,33,12.4481,254,287,200.25,388.10,464.35,4.8110,40.4627,0.232670,0.0000,0.0000,0.0000,0.0000,47.3540,0.1500,5.3178,5409,5096,585,585,0
e9_t0_audit,1,0,ScanNonIntersect,17349,1223.8178,55.52,154.98,296.02,255,14.6983,1465,1720,154.98,355.46,458.48,32.1916,179.0181,0.172583,0.0000,0.0000,0.0000,0.0000,224.2943,0.8624,33.8756,43,45,7,7,0
e9_t0_audit,1,0,Put,18800,208.1284,8.56,29.02,42.97,0,0.0000,0,0,0,0.00,0.00,0.00,0.0000,0.0000,0.000000,0.0000,0.0000,0.0000,0.0000,0.0000,0.0000,0.0000,0,0,0,0,0
e9_t0_audit,1,0,DeleteRange,1200,40.4579,30.79,53.64,74.49,0,0.0000,0,0,0,0.00,0.00,0.00,0.0000,0.0000,0.000000,0.0000,0.0000,0.0000,0.0000,0.0000,0.0000,0.0000,0,0,0,0,1200
e9_t0_audit,1,0,TOTAL_READS,80000,2622.6463,7.77,132.31,267.53,1194,14.9250,6734,7928,132.95,323.99,415.61,162.1662,864.0862,0.391304,790.5614,18.3669,0.0000,0.0000,271.6482,1.0123,39.1933,5452,5141,592,592,0
e9_t0_audit,1,0,TOTAL_ALL,100000,2871.2325,8.21,112.00,251.04,1194,11.9400,6734,7928,132.95,323.99,415.61,162.1662,864.0862,0.357426,790.5614,18.3669,0.0000,0.0000,271.6482,1.0123,39.1933,5452,5141,592,592,1200
e9_t0_audit,1,1,GetLive,16858,25028.7166,789.05,4410.36,4754.50,2945,174.6945,8374,11319,2044.21,4529.52,4821.23,7306.3883,17019.9612,0.971938,24409.1973,37.3138,0.0000,0.0000,0.0000,0.0000,0.0000,0,0,0,0,0
e9_t0_audit,1,1,GetDeleted,3142,5308.7157,789.05,4513.09,4748.75,542,172.5016,1456,1998,3007.17,4609.82,4859.36,1606.5285,3632.2639,0.986829,5254.5390,7.2899,0.0000,0.0000,0.0000,0.0000,0.0000,0,0,0,0,0
```

### 3.3 `audit_materialization_events.csv` 表头与前 10 行
```csv
run_id,rep,phase,worker,op_id,op_class,latency_us,materialization_us,lock_wait_us,active_mem_prep_us,active_mem_lookup_us,sst_iter_construct_us
e9_t0_audit,1,0,0,92,ScanNonIntersect,105.21,0.00,0.26,0.00,0.00,2.04
e9_t0_audit,1,0,0,117,GetLive,32.55,0.00,13.17,14.34,0.32,0.00
e9_t0_audit,1,0,0,127,GetLive,43.01,0.00,24.43,26.95,0.36,0.00
e9_t0_audit,1,0,0,167,GetLive,27.25,0.00,11.37,14.52,0.32,0.00
e9_t0_audit,1,0,0,183,ScanNonIntersect,119.17,0.00,27.53,0.00,0.00,5.27
e9_t0_audit,1,0,0,219,GetLive,25.26,0.00,10.82,11.60,0.31,0.00
e9_t0_audit,1,0,0,263,GetLive,24.78,9.25,0.00,12.53,0.14,0.00
e9_t0_audit,1,0,0,280,GetLive,30.98,0.00,16.98,17.91,0.26,0.00
e9_t0_audit,1,0,0,340,GetLive,23.95,9.72,0.00,12.06,0.20,0.00
e9_t0_audit,1,0,0,365,ScanNonIntersect,116.89,0.00,22.33,0.00,0.00,2.48
```

### 3.4 `audit_worker_snapshots.csv` 表头与前 5 行
```csv
exp_id,rep,phase,worker_id,op_class,op_count,total_latency_ms,p50_us,p95_us,p99_us,materialized_ops,materialization_rate_per_1k,lock_contended_ops,materialization_or_lock_affected_reads,affected_p50_us,affected_p95_us,affected_p99_us,view_materialization_ms,lock_wait_ms,materialization_and_lock_ratio,active_mem_prep_ms,active_mem_lookup_ms,imm_mem_prep_ms,imm_mem_lookup_ms,active_mem_iter_construct_ms,imm_mem_iter_construct_ms,sst_iter_construct_ms,reseek_count,boundary_advance_count,child_next_count,covered_skip_count,cache_invalidation_count
e9_t0_audit,1,0,0,GetLive,7314,138.3172,5.94,112.13,248.92,61,8.3402,611,672,121.84,288.93,382.04,7.5407,79.5667,0.629765,89.3541,2.2157,0.0000,0.0000,0.0000,0.0000,0.0000,0,0,0,0,0
e9_t0_audit,1,0,0,GetDeleted,186,3.8634,2.19,180.00,287.88,1,5.3763,20,21,153.85,287.88,304.57,0.1435,3.2365,0.874875,3.4427,0.0666,0.0000,0.0000,0.0000,0.0000,0.0000,0,0,0,0,0
e9_t0_audit,1,0,0,ScanIntersect,335,24.2874,55.32,211.73,324.38,2,5.9701,34,36,200.37,352.67,379.27,0.4181,5.2274,0.232445,0.0000,0.0000,0.0000,0.0000,5.8777,0.0182,0.6423,661,640,73,73,0
e9_t0_audit,1,0,0,ScanNonIntersect,2165,142.6209,54.06,136.46,273.43,9,4.1570,181,190,146.75,346.11,437.31,0.8553,21.1575,0.154345,0.0000,0.0000,0.0000,0.0000,23.4105,0.1097,3.8684,4,6,1,1,0
e9_t0_audit,1,0,0,Put,2350,26.0021,8.20,30.29,44.45,0,0.0000,0,0,0.00,0.00,0.00,0.0000,0.0000,0.000000,0.0000,0.0000,0.0000,0.0000,0.0000,0.0000,0.0000,0,0,0,0,0
```

---

## 四、 核心阶段统计表 (Phase A / B / C 深度剖析)

汇总自 `audit_phase_summary.csv`（8 个 Worker 并发聚合）：

| 阶段 (Phase) | 操作类别 (Op Class) | 操作数 (Ops) | 端到端总时间 (ms) | P50 延迟 (μs) | P99 延迟 (μs) | 物化触发操作数 (次) | 每千读物化率 (/1k) | 锁等待操作数 (次) | 受物化或锁波及读数 (占比) | 受波及读 P50 (μs) | 受波及读 P99 (μs) | 累计物化耗时 (ms) | 累计锁等待耗时 (ms) | 物化+锁占读总时间比 |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **Phase A (读敏感)** | `GetLive` | 58,535 | 1,171.9 | 6.09 | 249.4 | 883 | 15.1 | 4,883 | 5,766 (9.8%) | 117.4 | 401.0 | 120.9 | 620.8 | **63.3%** |
| Phase A | `GetDeleted` | 1,465 | 32.3 | 2.46 | 315.3 | 23 | 15.7 | 132 | 155 (10.6%) | 169.5 | 407.1 | 4.2 | 23.8 | **86.8%** |
| Phase A | `ScanIntersect` | 2,651 | 194.6 | 54.49 | 349.5 | 33 | 12.4 | 254 | 287 (10.8%) | 200.2 | 464.4 | 4.8 | 40.5 | **23.3%** |
| Phase A | `ScanNonIntersect` | 17,349 | 1,223.8 | 55.52 | 296.0 | 255 | 14.7 | 1,465 | 1,720 (9.9%) | 155.0 | 458.5 | 32.2 | 179.0 | **17.3%** |
| Phase A | **TOTAL_READS** | **80,000** | **2,622.6** | **7.77** | **267.5** | **1,194** | **14.9** | **6,734** | **7,928 (9.9%)** | **132.9** | **415.6** | **162.2** | **864.1** | **39.1%** |
| Phase A | Put (写操作) | 18,800 | 208.1 | 8.56 | 42.97 | 0 | - | 0 | 0 | - | - | 0.0 | 0.0 | - |
| Phase A | DeleteRange (墓碑写) | 1,200 | 40.5 | 30.79 | 74.49 | 0 | - | 0 | 0 | - | - | 0.0 | 0.0 | Inval=1200 |
| **Phase B (写突发)** | `GetLive` | 16,858 | 25,028.7 | 789.05 | 4,754.5 | 2,945 | 174.7 | 8,374 | 11,319 (67.1%) | 2,044.2 | 4,821.2 | 7,306.4 | 17,020.0 | **97.2%** |
| Phase B | `GetDeleted` | 3,142 | 5,308.7 | 789.05 | 4,748.8 | 542 | 172.5 | 1,456 | 1,998 (63.6%) | 3,007.2 | 4,859.4 | 1,606.5 | 3,632.3 | **98.7%** |
| Phase B | `ScanIntersect` | 6,791 | 12,617.7 | 1,103.14 | 4,987.6 | 1,104 | 162.6 | 3,400 | 4,504 (66.3%) | 2,921.0 | 5,051.5 | 3,076.0 | 8,142.0 | **88.9%** |
| Phase B | `ScanNonIntersect` | 3,209 | 4,156.2 | 789.05 | 4,722.9 | 540 | 168.3 | 1,652 | 2,192 (68.3%) | 1,599.4 | 4,815.4 | 1,023.1 | 2,522.3 | **85.3%** |
| Phase B | **TOTAL_READS** | **30,000** | **47,111.2** | **844.97** | **4,853.5** | **5,131** | **171.0** | **14,882** | **20,013 (66.7%)** | **2,231.9** | **4,921.6** | **13,012.0** | **31,316.5** | **94.1%** |
| Phase B | Put (写操作) | 64,000 | 2,719.9 | 34.59 | 159.5 | 0 | - | 0 | 0 | - | - | 0.0 | 0.0 | - |
| Phase B | DeleteRange (墓碑写) | 6,000 | 548.5 | 85.63 | 236.0 | 0 | - | 0 | 0 | - | - | 0.0 | 0.0 | Inval=6000 |
| **Phase C (读恢复)** | `GetLive` | 40,770 | 41,770.1 | 22.01 | 6,667.3 | 1,385 | 34.0 | 6,681 | 8,066 (19.8%) | 5,712.7 | 7,346.0 | 7,329.6 | 33,268.3 | **97.2%** |
| Phase C | `GetDeleted` | 19,230 | 19,784.6 | 9.66 | 6,659.2 | 652 | 33.9 | 3,142 | 3,794 (19.7%) | 5,799.9 | 7,231.2 | 3,486.5 | 16,022.6 | **98.6%** |
| Phase C | `ScanIntersect` | 19,655 | 24,167.0 | 192.64 | 6,889.0 | 715 | 36.4 | 3,326 | 4,041 (20.6%) | 5,971.1 | 7,174.5 | 3,796.1 | 16,761.0 | **85.1%** |
| Phase C | `ScanNonIntersect` | 345 | 516.2 | 206.41 | 6,480.1 | 14 | 40.6 | 85 | 99 (28.7%) | 5,403.0 | 9,695.7 | 74.0 | 375.0 | **87.0%** |
| Phase C | **TOTAL_READS** | **80,000** | **86,237.8** | **26.85** | **6,740.4** | **2,766** | **34.6** | **13,234** | **16,000 (20.0%)** | **5,792.9** | **7,279.6** | **14,686.3** | **66,426.9** | **94.1%** |
| Phase C | Put (写操作) | 17,200 | 834.3 | 45.71 | 151.2 | 0 | - | 0 | 0 | - | - | 0.0 | 0.0 | - |
| Phase C | DeleteRange (墓碑写) | 2,800 | 271.0 | 95.62 | 212.1 | 0 | - | 0 | 0 | - | - | 0.0 | 0.0 | Inval=2800 |
| **全流程总计** | **TOTAL_READS** | **190,000** | **135,971.7** | **11.45** | **6,426.4** | **9,091** | **47.8** | **34,850** | **43,941 (23.1%)** | **2,118.4** | **6,715.9** | **27,860.4** | **98,607.4** | **93.0%** |

---

## 五、 一页严格三层结论

根据 E9 单轮预检审计结果，严格按照三层认识框架归纳：

### 1. 已确认事实 (Confirmed Empirical Facts)
1. **写端全量失效确实发生**：全流程 10,000 条 DeleteRange（Phase A 1,200 条，Phase B 6,000 条，Phase C 2,800 条）触发了刚好 10,000 次活跃 MemTable 范围墓碑缓存失效（`cache_invalidation_count = 10,000`），二者具有 1:1 的直接因果对应；
2. **读写交织下物化发生率显著攀升**：
   - 在写突发的 Phase B 中，读操作触发范围墓碑视图物化的频率达到 **171.0 次/千读**（约每 5.8 次读取就有 1 次触发重建），累计触发 5,131 次物化，远高于低写率 Phase A 的 14.9 次/千读；
3. **并发读者在构建锁上的等待耗时远超物化自身耗时**：
   - 全流程物化累计耗时为 **27.86 秒**，而读者排队等待构建锁的累计耗时高达 **98.61 秒**（为物化耗时的 **3.54 倍**）；
   - 在 Phase B 中，多达 **14,882 次读操作（占读总数 49.6%）** 经历了互斥锁争用；在 Phase C 中有 **13,234 次（占读总数 16.5%）** 经历了锁争用；
4. **读时延结构中物化与锁等待占主导比例**：
   - 在 Phase B 和 Phase C 中，$\text{materialization\_and\_lock\_ratio}$ 均达到了 **94.1%**；全流程读端到端耗时中有 **93.0%** 消耗在范围墓碑视图物化与互斥锁排队等待中；
   - 受物化或锁争用波及的读操作（`materialization_or_lock_affected_reads`，共 43,941 次），其延迟从基线微秒级（Phase A 未受损读 P50 约 6~7 μs）恶化至毫秒级（Phase B 受损读 P50 为 **2.23 ms**，Phase C 受损读 P50 为 **5.79 ms**，P99 达到 **7.28 ms**）。

### 2. 合理但待验证的解释 (Reasonable but Unverified Hypotheses)
1. **T0 累积效应放大了解构耗时与锁排队深度**：
   - 在 T0 配置下，活跃 MemTable 中的墓碑持续驻留而不触发主动下刷，累积墓碑数量多达数千甚至上万。随着墓碑数增长，单次 `FragmentTombstones` 排序与红黑树切分的耗时上升，进而拉长了持锁窗口，导致更多并发读者积压在锁队列上。这一推论需待后续对比测试 T512（上限 512 条即下刷）以观察锁等待时间是否随阈值截断而大幅降低；
2. **读写交替频率决定合并失效的概率**：
   - 在 Phase B 中，6,000 次 DeleteRange 与 30,000 次读操作交错执行，导致物化次数达 5,131 次；若写入集中在更窄的批处理窗口内连续发生，多次 DeleteRange 合并为单次物化的几率可能增加；
3. **Scan 与 Point Get 承担相同的锁排队代价**：
   - 数据表明，`GetLive` 和 `ScanIntersect` 在 Phase B/C 的受损读 P50 / P99 非常接近（均为 2~3 ms 及 5~7 ms），这与“两者在底层均依赖同一互斥锁保护的 `FragmentedRangeTombstoneList` 缓存检查”这一代码调用事实相吻合。

### 3. 当前不能推出的结论 (Non-Sequitur / Prohibited Claims)
1. **当前不能断言“必须实现 AMTV 且 AMTV 必然优于主动 Flush 方案 (T512)”**：
   - 本预检仅测试了 **T0（即完全不按墓碑数 Flush 的被动极端场景）**。在 T0 下观测到的严重锁等待与高频物化，可能是因为 MemTable 中墓碑数量严重堆积所致；
   - 若引入 **T512**，MemTable 墓碑数被刚性限制在 512 条以内，单次物化耗时可能低至十数微秒，读者锁争用窗口可能迅速收缩，从而在现有架构下以低代价消除长尾；
   - 因此，**在未完成包含 T512 与 RTP-MC V2-A 的正式对比矩阵前，不能断言 AMTV 是解决此问题的唯一途径或必然最优解**；
2. **当前不能断言“稳态点查覆盖查询在动态混合负载下存在显著独立开销”**：
   - 预检数据显示，`active_mem_tombstone_cover_lookup_nanos` 累计耗时在 Phase A / B / C 分别仅为 19.5 ms、49.4 ms、122.8 ms，平均单次点查仅消耗约 0.3 ~ 0.5 μs；读路径端到端耗时的主要矛盾仍集中在物化及排队争用，二分查找覆盖检查本身并不构成主导瓶颈。

---

## 六、 运行元信息记录 (Environment & Reproducibility Manifest)

- **`rocksdb-rt-opt` Commit**: `2c4f321705801ff31dfc5db0e2a99fe148736ae9`
- **`rt-study` Commit**: `28a93414edab9d522a85617b7e9808f3ad23c0ff` (预检更新待提交)
- **Binary SHA-256**: `2b91cbe7f28f06957aace184641f7e9e8d9ee1526e016cf74fc8eb2327b35e71`
- **Trace Manifest SHA-256**: `855f2e4a575d1640ed37277634eb003ea0876e5957ac764a2f7b8275b3b9bc7e`
- **Config SHA-256**: `84cb79dffadfa90f98b25b6969f354d74082039e97a8a24cb5270592a4f4c4dd`
- **Compile Command**:
  `g++ -O2 -g -std=c++20 -fno-rtti -DNDEBUG -Wall -Wextra -pthread -DROCKSDB_READ_PATH_AUDIT -I/home/wam/grad/rocksdb-v11.8.0/include -I/home/wam/grad/rocksdb-v11.8.0 -I.`
- **CPU Affinity**: 48 Cores allocated across NUMA nodes
- **Kernel / OS**: Linux 6.8.0-136-generic #136~22.04.1-Ubuntu x86_64
