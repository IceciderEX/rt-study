# FormalV2-LongCycle-20GiB：动态混合负载下范围删除全生命周期验证实验协议

## 1. 实验定位与科学边界

- **实验全称**：`FormalV2-LongCycle-20GiB：动态混合负载下范围删除全生命周期验证`；
- **定位**：现有静态预加载开题证据（F1/F2）的动态演化与全生命周期适用范围验证；
- **核心原则**：
  1. 从全新空库开始，连续运行至终态约 20GiB 有效逻辑数据；
  2. 全程不重启数据库，阶段间不进行人工 Flush、不执行人工 CompactRange、不等待后台完全静置；
  3. 观察范围墓碑从 MemTable 进入 L0 并伴随自然写压逐步参与多层 Compaction 的动态性能演化与写放大效应；
  4. 如实记录并按三层边界（确证事实、合理解释、不可外推结论）报告结果。

---

## 2. 规模口径与数量定义

| 规模口径 | 定义与统计方式 | 20GiB 正式规模 | 2GiB Pilot 规模 (10%) |
| :--- | :--- | :---: | :---: |
| **`cumulative_put_value_bytes`** | 前台所有成功 Put 写入的 Value 累计字节数（含新 Key 与历史更新） | $30.6\text{ GiB}$ ($30,600,000 \times 1\text{KiB}$) | $3.06\text{ GiB}$ ($3,060,000 \times 1\text{KiB}$) |
| **`logical_live_value_bytes`** | 终态可见 Key 对应 Value 的逻辑总字节数（扣除被墓碑覆盖的 Key） | **$20.00\text{ GiB} \pm 5\%$** ($20,480,000 \times 1\text{KiB}$) | **$2.00\text{ GiB}$** ($2,048,000 \times 1\text{KiB}$) |
| **`physical_db_bytes`** | 数据库目录实际物理磁盘占用（通过 `du -b` 与文件系统统计） | 预计 $22 \sim 32\text{ GiB}$（含历史版本与 SST 元数据） | 预计 $2.2 \sim 3.2\text{ GiB}$ |
| **`engine_output_bytes`** | RocksDB 内部 Flush 与 Compaction 累计输出字节总数 | 监控并统计写放大 (WA) | 监控并统计写放大 (WA) |

### 核心几何与参数设计：
- **Value 大小**：固定 $1,024\text{ Bytes}$ ($1\text{ KiB}$)；
- **Key 格式**：固定 16 字节十进制数字字符串（`%016lu`），严格升序可排；
- **总新插入 Key 数**：$25,600,000$ 个 Key（Pilot 为 $2,560,000$ 个）；
- **总更新 Put 次数**：$5,000,000$ 次更新（Pilot 为 $500,000$ 次）；
- **DeleteRange 总数**：$50,000$ 条范围墓碑（Pilot 为 $5,000$ 条）；
  - **基础覆盖墓碑**：$40,000$ 条，单条跨度 128 Keys，并集覆盖 $5,120,000$ 个 Key（**覆盖率严格为 $20.0\%$**）；
  - **受控重叠墓碑**：$10,000$ 条，完全落在已有删除区间内，重叠率 $20.0\%$，不增加删除并集；
- **终态可见 Key 数**：$25,600,000 - 5,120,000 = 20,480,000$ 个 Key（Pilot 为 $2,048,000$ 个）。

---

## 3. RocksDB 引擎配置与 FormalV2 基线差异对照

使用纯净官方 RocksDB `v11.8.0`（Commit `abeebd9630f11bd08c28b7bd43c7bdfc62050654`）。

| 配置参数 | FormalV2 默认冻结基线 | LongCycle-20GiB 规范基线 | 配置调整原因说明 |
| :--- | :---: | :---: | :--- |
| **`value_size`** | $256\text{ Bytes}$ | **$1,024\text{ Bytes}$ ($1\text{ KiB}$)** | 20GiB 逻辑数据规模标准定义，与工业真实负载对齐 |
| **`max_background_jobs`** | $8$ | **$4$** | 规范指定，限制并发后台线程，稳定观测 Compaction 压力 |
| **`block_cache`** | $512\text{ MiB}$ (或 $128\text{ MiB}$) | **$128\text{ MiB}$** | 规范指定，避免过大缓存掩盖冷读 IOPS 退化 |
| **`write_buffer_size`** | $64\text{ MiB}$ | $64\text{ MiB}$ | 保持一致 |
| **`max_write_buffer_number`** | $4$ | $4$ | 保持一致 |
| **`min_write_buffer_number_to_merge`** | $1$ | $1$ | 保持一致 |
| **`num_levels`** | $7$ | $7$ | 保持一致 |
| **`target_file_size_base`** | $64\text{ MiB}$ | $64\text{ MiB}$ | 保持一致 |
| **`max_bytes_for_level_base`** | $256\text{ MiB}$ | $256\text{ MiB}$ | 保持一致 |
| **`compression_type`** | `kNoCompression` | `kNoCompression` | 保持一致，排除压缩算法对 CPU 的混淆 |
| **WAL 设置** | 开启 (`disableWAL=false`) | 开启 (`disableWAL=false`) | 真实持久化与全生命周期日志模拟 |

---

## 4. 四阶段动态混合负载比例与墓碑注入设计

全流程按照累计新插入逻辑数据量分为连续四个阶段：

```mermaid
graph LR
    A["Phase A: 早期增长 (0 -> 6.25G)"] --> B["Phase B: 稳定下沉 (6.25G -> 12.5G)"]
    B --> C["Phase C: 墓碑突发 (12.5G -> 18.75G)"]
    C --> D["Phase D: 读写恢复 (18.75G -> 25G)"]
```

| 负载特征 | Phase A (0 $\rightarrow$ 6.25G) | Phase B (6.25G $\rightarrow$ 12.5G) | Phase C (12.5G $\rightarrow$ 18.75G) | Phase D (18.75G $\rightarrow$ 25G) |
| :--- | :---: | :---: | :---: | :---: |
| **新 Key Put 比例** | $70\%$ | $50\%$ | $35\%$ | $40\%$ |
| **已有 Key 更新 Put 比例** | $10\%$ | $20\%$ | $25\%$ | $15\%$ |
| **Get 点查比例** | $15\%$ | $20\%$ | $25\%$ (细分为三类点查) | $30\%$ |
| **Fixed-Range Scan 比例** | $5\%$ | $10\%$ (约 50% 相交) | $15\%$ (约 80% 相交) | $15\%$ |
| **DeleteRange 注入条数** | $2,000$ 条 (1GiB 后注入) | $13,000$ 条 (删 Phase A 旧 Key) | $30,000$ 条 (集中于中间 60% 窗口) | $5,000$ 条 (仅前 20% 窗口注入) |
| **删除状态与目标** | 建立初始 SST 与 L0 | 数据进入 L1/L2，墓碑下沉 | 形成严重突发与跨层分布 | **后 80% 窗口 0 墓碑 (恢复观察期)** |

### 点查与扫描精细分类：
1. **Get 点查分类**：
   - `GetDeleted`：查询已被 DeleteRange 覆盖的 Key（预期返回 NotFound）；
   - `GetLiveAdjacent`：查询与删除区间紧邻但存活的 Key（跨越边界探针）；
   - `GetLiveControl`：查询远离删除区间的纯存活 Key（基准对照）。
2. **Fixed-Range 扫描分类**（`[start, start+100)`）：
   - `ScanIntersect`：扫描区间跨越墓碑区间；
   - `ScanNonIntersect`：扫描区间完全处于存活区间；
   - `ScanBoundary`：专门从删除区间边界开始扫描。

---

## 5. 实验矩阵与交错执行协议

| 组别编号 | 策略名称 | DeleteRange 处理方式 | `memtable_max_range_deletions` | 重复次数 |
| :--- | :--- | :---: | :---: | :---: |
| **1** | **`LC20-CLEAN`** | 替换为 No-op（无删除上限对照） | $0$ | $N=3$ |
| **2** | **`LC20-T0`** | 正常执行 | $0$ | $N=3$ |
| **3** | **`LC20-T512`** | 正常执行 | $512$ | $N=3$ |

- **交错执行序列**：
  `CLEAN-r01 → T0-r01 → T512-r01 → CLEAN-r02 → T0-r02 → T512-r02 → CLEAN-r03 → T0-r03 → T512-r03`；
- **状态对账规则**：
  - `LC20-CLEAN` 组独立对账预期模型（$25,600,000$ 个可见 Key，SHA-256 跨轮次一致）；
  - `LC20-T0` 与 `LC20-T512` 终态可见 Key 数必须严格为 **$20,480,000$**，全库 SHA-256 摘要跨组与跨轮次必须 **逐比特 100% 严格一致**。

---

## 6. 时间序列与正确性审计规范

1. **每 5 秒时间序列采样**：
   - 前台：Trace IOPS、API IOPS、各操作 P50/P95/P99/P99.9 延迟；
   - 内部：活跃 MemTable 大小、Immutable 数量、L0~L6 文件数与字节数、Pending Compaction Bytes、Write Stall 计数、丢弃 Key 数；
   - 系统：CPU 利用率、RSS 内存、NVMe 读写带宽与利用率；
2. **10 分钟 Cooldown 观察**：
   - 前台负载结束后，不人工 Flush、不人工 CompactRange，静默观察 10 分钟后台收敛与 SST 层级稳定；
3. **全库遍历对账**：
   - 全库顺序扫描验证可见 Key 数、可见 Value 字节总数、全库 SHA-256 摘要与物理磁盘 `du -b` 占用。
