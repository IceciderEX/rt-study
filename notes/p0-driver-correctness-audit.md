# 阶段 B & C：驱动正确性与 SmokeTest 审计报告 (notes/p0-driver-correctness-audit.md)

**生成时间**：2026-08-20  
**实验机器节点**：`s14.servers.hustpdsl.cn`  
**测试目标**：验证原生 C++ 工作负载驱动、确定性参考模型校验、三类 Get 分类、RangeScan 键数/宽度/延迟度量管道以及指标采集脚本的完整性与正确性。

---

## 1. 驱动与环境构建审计

- **RocksDB 源码与链接库**：`/home/wam/grad/rocksdb-v11.8.0/` (Tag: `v11.8.0`, commit: `abeebd9630f11bd08c28b7bd43c7bdfc62050654`, clean)
- **驱动二进制**：`/home/wam/grad/s14-range-delete-study/bin/workload_driver` (Release `-O2`, `-std=c++20`)
- **Key 格式规范**：`key_%012llu`（16 字节固定宽度字符串编码），严格保证字典序与数值序完全一一对应。
- **DeleteRange 语义验证**：`DeleteRange(begin_key, end_key)` 严格匹配数值区间 $[begin\_id, end\_id)$ 的左闭右开语义。

---

## 2. SmokeTest 执行参数与配置

- **数据集规模**：50,000 Keys (初始预置数据约 6.4 MB，严格限制在 <1GiB 安全阈值内)
- **Value 大小**：128 Bytes
- **线程数**：4 线程
- **总操作请求量**：20,000 Ops
- **操作分布**：
  - Put: 5.0%
  - Get: 70.0%
  - RangeScan: 23.0% (Scan Span: 100 Keys)
  - DeleteRange: 2.0% (Range Length: 200 Keys)
- **访问模型**：Uniform 均匀分布
- **CompactRange 限制**：完全禁止调用，未做任何后台干预或参数搜索。

---

## 3. 正确性校验与对账结果

### 3.1 在线连续对账
- 所有前台 `Put`、`DeleteRange`、`Get` 与 `RangeScan` 操作均与内存中的 `ReferenceModel` 地面真值实时对账。
- 在线验证错误数：**0 Errors**。

### 3.2 离线抽样对账 (10,000 Keys 随机抽样)
- **抽样 Key 数量**：10,000 个 Key
- **存活 Key (Live OK)**：2,655 个（预期返回 OK，实际全部返回 OK，数据完整性 100%）
- **已删 Key (Deleted NotFound)**：7,345 个（预期返回 NotFound，实际全部返回 NotFound）
- **校验结论**：`[PASS]`，一致性 100.00%。

### 3.3 全库 Iterator 遍历全量对账
- **RocksDB 实际扫描存活 Key 总数**：13,943 个
- **ReferenceModel 记录存活 Key 总数**：13,943 个
- **差异 (Delta)**：**0 个 Key**
- **逻辑总有效 Payload 大小**：1.91 MB
- **有序性检验**：全库 Iterator 输出键严格单调递增，无幽灵键，无脏读。
- **校验结论**：`[PASS]`。

---

## 4. 细分指标与分类统计审计

### 4.1 Get 三类独立延迟统计
为避免将“因删除导致快速返回 NotFound”与“正常命中点查”混合掩盖真实开销，驱动将 Get 严格拆分为三类独立统计：

| Get 类别 | 样本量 (Count) | 错误数 (Err) | P50 延迟 (μs) | P95 延迟 (μs) | P99 延迟 (μs) | 平均延迟 (μs) | 语义说明 |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **Get (Affected-Live)** | 1,994 | 0 | 5.11 | 60.99 | 456.70 | 18.22 | 处于范围墓碑覆盖区域或边界附近的有效活 Key |
| **Get (Deleted-ExpectedNF)** | 6,785 | 0 | 5.40 | 69.80 | 515.05 | 24.61 | 处于已删除区间、预期返回 NotFound 的 Key |
| **Get (Control-FarLive)** | 5,239 | 0 | 3.93 | 54.32 | 404.55 | 16.13 | 远离任何删除区间的纯净活 Key 对照组 |

### 4.2 RangeScan 精确度量
- **扫描操作总数**：4,560 次 (0 错误)
- **请求设定的键空间宽度 (Span)**：平均 99.9 个 Key
- **实际返回的有效 Key 数**：平均 50.6 个 Key / Scan (范围覆盖率 ~50.6%)
- **扫描吞吐率**：9,115.7 scans/s，460,955.2 keys/s
- **扫描延迟**：P50 = 68.43 μs, P95 = 188.86 μs, P99 = 607.67 μs, Mean = 92.89 μs
- **单位有效 Key 归一化延迟**：**1.837 μs / key**
- **防误判机制**：明确记录“设定跨度”与“实际返回键数”，杜绝将“删除后返回键变少导致的扫描耗时减少”误判为存储系统性能提升。

### 4.3 Range Tombstone 空间分布统计
- **DeleteRange 执行总次数**：435 次
- **并集覆盖删除 Key 数**：36,281 个 Key
- **键空间并集覆盖率**：72.56%
- **墓碑重叠因子 (Overlap Factor)**：2.391x (累计删除长度 / 并集覆盖长度)

---

## 5. 存储、日志与清理策略审计

- **数据库路径**：`/home/wam/grad/s14-range-delete-study/run-db/smoke-test-db`
- **原始数据与日志目录**：`/home/wam/grad/s14-range-delete-study/results/raw/smoke-test/`
  - `driver_stdout.log` (驱动全量标准输出)
  - `system_iostat.log` (系统底层 I/O 每秒采样)
  - `LOG` (RocksDB 引擎内部运行日志)
- **汇总结构化数据**：`/home/wam/grad/s14-range-delete-study/results/summary/smoke-test.csv`
- **清理隔离性核验**：清理脚本仅针对本实例目录 `smoke-test-db` 执行清理，绝不触碰 `/home/wam/grad/` 下其他任何文件。

---

## 6. 审计结论
1. 驱动在 RocksDB v11.8.0 原生环境下的编译、链接与运行全部正常；
2. Put/Get/RangeScan/DeleteRange 语义与参考模型 100% 吻合，通过抽样与全量对账；
3. Get 三类独立统计与 RangeScan 归一化指标采集管线运行正常；
4. 准入条件全部满足，驱动完全就绪。
