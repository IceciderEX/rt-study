# Formal V2 RangeDelete 实验驱动重构与审计报告

版本：v2.3-Formal-Frozen（2026-08-21）  
适用目标：作为正式学术论文主表、主图及全生命周期性能评估的基石。

---

## 1. 核心 P0 / P1 缺陷修复与审计对账（全量收口）

| 序号 | 问题分类 | 缺陷描述 | 修复方案与工程实现 | 验证状态 |
| :---: | :--- | :--- | :--- | :---: |
| 1 | **P0: Manifest 总量闭环与 OpId 唯一性** | 未验证总操作数闭环、ValueSize 匹配及跨分片 OpId 重复。 | **三维总量闭环与全局 OpId 去重审计**：实现 [manifest_parser.h](file:///home/wam/grad/s14-range-delete-study/tools/formal_v2/manifest_parser.h)，断言 $\text{total\_ops\_count} \equiv \sum_{i=1}^{24} \text{ops\_count}_i = 300,000$；校验 $\text{manifest.value\_size} \equiv \text{config.value\_size} = 256$；加载时通过哈希集合检测全部 300,000 条记录的 `op_id` 全局唯一性。 | **100% PASS** |
| 2 | **P0: AST 级严格 JSON 解析器** | 旧实现依赖 `find()` 字符串匹配，抗乱序与容错性不足。 | **标准递归下降 AST JSON 解析器**：编写 [json_parser.h](file:///home/wam/grad/s14-range-delete-study/tools/formal_v2/json_parser.h)，支持完整词法/语法树分析、转义字符、嵌套对象与类型强校验，杜绝解析误判。 | **100% PASS** |
| 3 | **P0: Scan API 完整生命周期计时** | 计时仅覆盖 `Seek+Next`，遗漏 `DB::NewIterator()`。 | **完整 Scan API 计时包络**：将 `db_->NewIterator(read_opts)` 纳入纳秒计时区间，真实反映上层调用完整 Scan API 的端到端开销。 | **100% PASS** |
| 4 | **P0: Cooldown 严格 10 秒窗口隔离** | 10s 结束后全库验证期间的后台事件混入 Cooldown 指标。 | **引入 `kVerification` 阶段**：10 秒 Cooldown 结束后立即调用 `StartVerificationStage()` 冻结主实验写放大统计，验证期间事件标记为 `VERIFICATION`，绝不混入 `pwa_val_norm_total`。 | **100% PASS** |
| 5 | **P0: Scan 严格分区边界与截断统计** | RangeScan 未强制 $key_2 \le worker\_end$，LimitScan 尾部可能跨分片。 | **双重边界硬约束与截断率统计**：准入时校验端点；执行循环中强制注入 `it->key().compare(w_end_slice) < 0` 约束；新增 `scan_limit_truncated_count` 统计截断次数。 | **100% PASS** |
| 6 | **P0: 纯 DB API 调用包络延迟测量** | 计时区间内混入 Key 格式化与预期 Value 生成，Live/Deleted Get 存在开销不对称。 | **严格分离驱动开销与引擎调用包络**：Key 编码与期望 Value 预生成移至计时器前；DB 调用处于纯净包络内；严格语义校验与模型更新移至计时器停止后。 | **100% PASS** |
| 7 | **P0: 阶段屏障双硬同步** | 单 Barrier 仅保证阶段结束会合，Worker 提前抢跑导致 Phase B/C 耗时失真。 | **双硬屏障（Start/End Barrier Pair）硬同步**：Coordinator 记录 $T_0 \to$ `phase_start_barrier` 放行 $\to$ 执行 $\to$ `phase_end_barrier` 会合 $\to$ Coordinator 记录 $T_1$。$\text{True Phase IOPS} = \Delta \text{ops} / \Delta t$。 | **100% PASS** |
| 8 | **P1: 前台时间指标明确拆分** | 阶段活动时间之和不能代表包含屏障切换的前台整体墙钟时间。 | **双时间指标体系**：另报 `foreground_wallclock_sec` 作为整体前台 IOPS 的精确分母；`sum_phase_active_sec` 专用于三阶段执行耗时分析。 | **100% PASS** |
| 9 | **P1: Get 实时双向严格断言** | 旧驱动未在 Get 返回时校验期望状态。 | **实时双向严苛判定**：预期 Live 断言 `s.ok()` 且 Value 版本匹配；预期 Deleted 断言 `s.IsNotFound()`。任何偏差立即判定失败。 | **100% PASS** |
| 10 | **P1: 引擎输出写放大统一命名** | 引擎输出字节与系统写放大混淆。 | **统一命名规范**：明确标注为 `fwa_val_norm_fg`、`cwa_val_norm_fg`、`pwa_val_norm_fg`、`pwa_val_norm_total`（以更新 Value 字节 `total_logical_put_bytes` 归一化）。 | **100% PASS** |

---

## 2. 负 Trace 拒绝测试与前台冒烟审计验证

### 2.1 4 类负 Trace 严格拒绝测试
运行 [scripts/formal_v2/test_negative_traces.py](file:///home/wam/grad/s14-range-delete-study/scripts/formal_v2/test_negative_traces.py)：
- **Case 1: 校验和篡改** $\to$ 捕获 `SHA-256 verification failed`，拦截退出 (**PASS**);
- **Case 2: 跨分片越界 Key** $\to$ 捕获 `out of partition`，拦截退出 (**PASS**);
- **Case 3: 非法操作码 (`op_type=9`)** $\to$ 捕获 `Invalid op_type`，拦截退出 (**PASS**);
- **Case 4: 重复操作序号 (`dup_op_id`)** $\to$ 捕获 `Duplicate op_id`，拦截退出 (**PASS**)。

### 2.2 2 轮同配置冒烟测试与事件 CSV 窗口审计
运行 [scripts/formal_v2/run_preformal_smoke_audit.py](file:///home/wam/grad/s14-range-delete-study/scripts/formal_v2/run_preformal_smoke_audit.py)：
```
=== Running Formal V2 Pre-Formal Smoke & Event Window Audit ===
[FormalDriver] Passed 100% Comprehensive Admission Audit for 24 payload traces (Unique OpIds: 300000)
[Preload] Preload completed and settled in 0.68 s.
[Coordinator] Phase 0: True IOPS = 211505.79 (0.4728s)
[Coordinator] Phase 1: True IOPS = 14428.01 (6.9310s)
[Coordinator] Phase 2: True IOPS = 8145.85 (12.2762s)
[Verification] DB Live Keys: 316193, DB SHA-256 == Model SHA-256 == 366c5e5b3e5c4e5558be1175b16f5e76b8c71be13bb8e607fae36492d09f1c34 >> PASS <<

[AUDIT 1] Deterministic State Checksum Verified: 366c5e5b3e5c4e5558be1175b16f5e76b8c71be13bb8e607fae36492d09f1c34
[AUDIT 2] Foreground Wallclock (19.7056s) & Sum Phase Active (19.6373s) Verified.
[AUDIT 3] Events CSV Window Timeline Analysis: Preload (3 events) isolated from Foreground/Cooldown.
=========================================================
  PRE-FORMAL SMOKE & EVENT CSV WINDOW AUDIT PASSED (100%)
=========================================================
```
