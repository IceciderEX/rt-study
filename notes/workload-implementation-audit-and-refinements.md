# Workload 实现细节只读审计、口径收紧与代码加固报告 (notes/workload-implementation-audit-and-refinements.md)

**审计时间**：2026-08-21 12:24  
**审计与加固对象**：全量 Trace 二进制文件、[`tools/thesis_validation/tv_driver.cc`](file:///home/wam/grad/s14-range-delete-study/tools/thesis_validation/tv_driver.cc) 驱动源码、统计采集与状态校验逻辑。  
**核心原则**：严格实事求是，全面收紧表述，透明公布所有实现细节与学术边界。

---

## 一、两大核心学术定位与叙事修正

### 1. B2 小规模动态 Trace 定位：98.36% 覆盖率的极端工况
- **实测审计结果**：
  - `b2_dynamic_phases.bin` 在 500k Key 空间中注入 20,000 个跨度 100 的删除区间，**实际并集覆盖 491,793 键，覆盖率达 98.36%**。
- **学术定位修正**：
  - 此前小规模 B2 / P7 中的极端读退化，**准确定位为“近乎全 Key 空间被范围删除覆盖的高压力极端工况（Extreme Stress Baseline）”**；
  - 它严格证明了极端高墓碑密度下 MemTable 读退化风险的存在，但不能代表一般 40% 覆盖率场景；
  - **真正适合写入主叙事的 40% 覆盖基准**是 **`LS24-DensityPreserved`**（实际 19,856 条范围删除、39.71% 覆盖率、0.00% 区间重叠，且配有同轨迹 `LS24-DP-Clean` 对照）。

### 2. 参考模型与真实 DB 并发执行顺序差异 & Get(Del) 严格命名
- **机制核验**：`LoadTrace()` 在运行前按 Trace 顺序一次性应用了所有 Put 与 DeleteRange 到参考模型；而真实 RocksDB 由 8 个线程并发乱序执行。
- **命名与语义严格修正**：
  - **统一更名**：原 `Get(Del)` 统一命名为 **“按 Trace 最终删除范围标记的点查请求 (Point Lookups Tagged by Trace Final Deletion Range)”**；
  - **禁止表述**：严禁再使用“实时命中已删除键”或“实时逻辑可见性分类”。

---

## 二、三处关键文字与口径严格修正

1. **重放机制表述**：
   - 修正为：**“固定 Trace 内容的并发重放；实际操作线性化顺序受线程调度影响”**（杜绝使用“统计确定性并发重放”）；
2. **Clean 对照组表述**：
   - `LS24-DP-Clean` 修正为：**“保持 Get/Scan/Put 请求不变、去除 DeleteRange 调用的功能性无范围删除对照（Functional No-DeleteRange Baseline with Identical Get/Scan/Put Trajectory）”**；
3. **覆盖键数表述**：
   - 修正为：**“范围删除并集覆盖键数 (Union Keys Covered by Range Deletions)”**（杜绝使用“实际物理失效键数”，因为后续 Put 可能重写被删键，物理丢弃由后台 Compaction 最终决定）。

---

## 三、代码层面完成的六项严格加固 (已合入 `tv_driver.cc` 并编译)

| 加固项 | 具体实现逻辑 | 防御目标与错误行为 |
| :--- | :--- | :--- |
| **1. 严格返回值检查** | `Put`/`DeleteRange` 非 `s.ok()` 报错；`Get` 只允许 `s.ok()` 或 `s.IsNotFound()`；`Scan` 迭代器结束严格检查 `it->status().ok()` | 任何 RocksDB 操作异常立即置 `experiment_failed_ = true` 并中止 |
| **2. Trace 完整性校验** | 检查 `file_size % 17 == 0`，操作数必须与配置 `total_ops` 一致，`op_type` 必须在 `[0, 4]` 范围内 | 防止文件截断、配置与数据不匹配或非法操作码混入 |
| **3. 非零退出码支持** | `RunExperiment()` 失败或全库校验失败时，`main()` 返回非零退出码 `1` | 彻底防止失败运行伪造或继续生成总结数据 |
| **4. 线程安全审计** | `TVLatencyTracker` 所有 `Record` 与 `GetQuantiles` 均由 `std::mutex` 严格保护 | 确保多线程并发记录与分位数计算无数据竞争 |
| **5. 扫描算子语义修正** | 驱动显式标注为 `LIMIT 100` 有效可见键扫描循环 | 纠正“固定区间扫描”的表述误差 |
| **6. 混杂参数核验** | 核验 `memtable_op_scan_flush_trigger` 默认全为 `0` (DISABLED) | 排除该参数与 `memtable_max_range_deletions` 产生混杂影响 |

---

## 四、二进制 Trace 逐字节实测审计终态总表

| Trace 文件名 | 总操作数 (Ops) | **实际 DeleteRange 条数** | **唯一互斥区间数** | 区间重叠率 | **范围删除并集覆盖键数** | **实际并集覆盖率** |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| `b2_dynamic_phases.bin` | 300,000 | 20,000 | 19,611 | 1.95% | 491,793 | **98.36%** (500k 极端压力工况) |
| `ls24_dynamic_phases.bin` | 300,000 | 20,000 | 19,998 | 0.01% | 1,978,967 | **1.97%** (1.006 亿稀释工况) |
| `ls24_density_phases.bin` | 300,000 | **19,856** | **19,856** | **0.00%** | **39,970,128** | **39.71%** (1.006 亿 40% 密度基准) |
| `ls24_density_clean_phases.bin`| 300,000 | **0** (19,856 No-op) | 0 | 0.00% | 0 | **0.00%** (同轨迹功能性无删除基线) |
