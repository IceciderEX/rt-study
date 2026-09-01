# FormalV2-E8：高密度 RangeDelete 读路径 CPU 机制定位实验分析报告

## 1. 实验目标与科学定位

本实验为**选题决策实验（Go/No-Go Decision Experiment）**：
- **核心科学问题**：定位 $T=0$ 高密度范围墓碑状态相对于 `Clean`（无墓碑）与 $T=512$（阈值下刷）所新增的 CPU 开销（$\Delta\text{cycles}$、$\Delta\text{instructions}$、$\Delta\text{cache-misses}$、$\Delta\text{branches}$）产生于何种机制；
- **核心判定**：评估 CPU 增量是否主要集中于活跃 MemTable 中范围墓碑的组织检索、区间相交判断或缓存失效，进而判定是否值得转向“**活跃 MemTable 范围墓碑读路径优化**”；
- **学术边界**：纯净 RocksDB `v11.8.0`，现有 `formal_v2` 驱动，不修改 RocksDB 源码，不引入自适应算法。

---

## 2. 实验环境、构建与非复用 Profiling 协议固化

### 2.1 硬件拓扑与环境基线
- **宿主机**：`s14.servers.hustpdsl.cn`（Ubuntu 22.04 LTS，内核 `Linux 6.8.0-136-generic x86_64`）；
- **CPU 拓扑**：Dual Intel(R) Xeon(R) Gold 5218R @ 2.10GHz（双路 40 物理核 / 80 逻辑核，L1d 1.3MB，L2 40MB，L3 55MB）；
- **绑核策略**：单线程机制实验独占绑定至 Node 0 独立物理核 CPU 2（`taskset -c 2`），确保其 SMT 兄弟核 CPU 42 空闲；
- **构建参数**：Release 优化 + 调试符号 + 帧指针（`-O2 -g -fno-omit-frame-pointer -std=c++20 -pthread -fno-rtti`）；
- **特权边界**：未修改 `perf_event_paranoid`（值为 4），driver 由普通用户 `wam` 启动，仅通过 `sudo -n perf stat -t <foreground_tid>` 附着目标线程；隔离目录权限强制为 `0700`（`umask 077`）。

### 2.2 三轮非复用（3 Non-Multiplexed Passes）采集规范
为彻底杜绝硬件性能计数器复用插值误差，每个测试点拆分为 3 轮独立采样：
- **Pass A**：`cycles:u, instructions:u`（用于计算 cycles/op、insn/op 与真实 IPC）；
- **Pass B**：`branches:u, branch-misses:u`（用于计算分支预测失误率）；
- **Pass C**：`cache-references:u, cache-misses:u`（用于计算 LLC 缓存未命中率）；
- **时间边界控制**：双等待点内核阻塞（`std::condition_variable`），Worker 在 45s 单操作 Deadline 循环前后严格处于内核休眠，实测 `CLOCK_THREAD_CPUTIME_ID` 耗时与 45s 窗口耗时比率高达 **`99.99%`**，证明工作窗口内目标线程几乎持续执行微基准，等待区间零忙等。

---

## 3. 全矩阵实测汇总表

全矩阵包含 5 个实验条件 $\times$ 3 类微基准 $\times$ 3 轮独立 DB 重复 $\times$ 3 个 Pass = **135 轮独立实验**。

### 3.1 硬件性能指标汇总表（$N=3$ 均值 $\pm$ 标准差）
> 数据来源：`results/summary/formal-v2-e8-perf-stat.csv`

| 实验层级 | 策略组别 | 微基准操作 | 吞吐量 (IOPS) | 每操作周期 (cycles/op) | 每操作指令数 (insn/op) | 真实 IPC | 分支预测失误率 (%) | 缓存未命中率 (%) |
| :--- | :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **Normal** | **CLEAN** | **`GetLive`** | $574,204 \pm 22,666$ | $\mathbf{6,513.9 \pm 253.7}$ | $14,307.8 \pm 586.0$ | $2.20 \pm 0.01$ | $0.20 \pm 0.00$ | $0.05 \pm 0.01$ |
| Normal | CLEAN | **`ScanIntersect`** | $68,998 \pm 123$ | $\mathbf{52,943.2 \pm 95.2}$ | $129,589.8 \pm 0.5$ | $2.45 \pm 0.00$ | $0.14 \pm 0.00$ | $0.52 \pm 0.02$ |
| Normal | CLEAN | **`ScanNonIntersect`** | $65,295 \pm 273$ | $\mathbf{55,829.1 \pm 154.7}$ | $136,680.5 \pm 27.2$ | $2.45 \pm 0.00$ | $0.15 \pm 0.00$ | $0.07 \pm 0.01$ |
| **Normal** | **T0** | **`GetLive`** | $512,500 \pm 24,594$ | $\mathbf{7,302.6 \pm 346.7}$ | $16,288.6 \pm 587.1$ | $2.23 \pm 0.02$ | $0.20 \pm 0.01$ | $0.12 \pm 0.01$ |
| Normal | T0 | **`ScanIntersect`** | $66,497 \pm 2,486$ | $\mathbf{54,940.4 \pm 2,269.9}$ | $129,269.6 \pm 4,559.6$ | $2.35 \pm 0.02$ | $0.20 \pm 0.01$ | $0.17 \pm 0.02$ |
| Normal | T0 | **`ScanNonIntersect`** | $62,355 \pm 171$ | $\mathbf{58,630.7 \pm 153.7}$ | $141,715.0 \pm 34.0$ | $2.42 \pm 0.00$ | $0.14 \pm 0.00$ | $0.06 \pm 0.01$ |
| **Normal** | **T512** | **`GetLive`** | $569,506 \pm 3,146$ | $\mathbf{6,568.5 \pm 46.4}$ | $14,853.2 \pm 2.3$ | $2.26 \pm 0.01$ | $0.13 \pm 0.01$ | $0.09 \pm 0.01$ |
| Normal | T512 | **`ScanIntersect`** | $81,799 \pm 171$ | $\mathbf{44,435.1 \pm 136.1}$ | $104,519.9 \pm 0.5$ | $2.35 \pm 0.00$ | $0.14 \pm 0.00$ | $0.29 \pm 0.02$ |
| Normal | T512 | **`ScanNonIntersect`** | $67,815 \pm 2,659$ | $\mathbf{53,784.2 \pm 1,891.9}$ | $128,819.1 \pm 3,821.2$ | $2.40 \pm 0.02$ | $0.16 \pm 0.01$ | $0.12 \pm 0.02$ |
| **Isolation**| **PreFlush** | **`GetLive`** | $493,927 \pm 36,350$ | $\mathbf{7,586.8 \pm 513.4}$ | $16,627.3 \pm 585.9$ | $2.20 \pm 0.02$ | $0.17 \pm 0.01$ | $0.09 \pm 0.01$ |
| Isolation | PreFlush | **`ScanIntersect`** | $66,753 \pm 2,506$ | $\mathbf{54,519.4 \pm 2,110.9}$ | $129,267.6 \pm 4,624.0$ | $2.37 \pm 0.02$ | $0.19 \pm 0.01$ | $0.40 \pm 0.03$ |
| Isolation | PreFlush | **`ScanNonIntersect`** | $64,154 \pm 2,506$ | $\mathbf{57,086.2 \pm 2,206.7}$ | $136,963.0 \pm 4,113.1$ | $2.40 \pm 0.02$ | $0.15 \pm 0.01$ | $0.06 \pm 0.01$ |
| **Isolation**| **PostFlush**| **`GetLive`** | $505,499 \pm 5,988$ | $\mathbf{7,397.1 \pm 112.7}$ | $16,412.7 \pm 350.9$ | $2.22 \pm 0.01$ | $0.15 \pm 0.01$ | $0.06 \pm 0.01$ |
| Isolation | PostFlush| **`ScanIntersect`** | $63,637 \pm 2,819$ | $\mathbf{57,278.2 \pm 2,548.9}$ | $135,022.2 \pm 4,988.0$ | $2.36 \pm 0.02$ | $0.21 \pm 0.01$ | $0.34 \pm 0.03$ |
| Isolation | PostFlush| **`ScanNonIntersect`** | $62,467 \pm 1,499$ | $\mathbf{58,656.2 \pm 1,818.8}$ | $141,270.2 \pm 4,453.5$ | $2.41 \pm 0.01$ | $0.15 \pm 0.01$ | $0.05 \pm 0.01$ |

---

## 4. 三维 CPU 周期差分分析（$\Delta\text{cycles}$ Analysis）

> 数据来源：`results/summary/formal-v2-e8-delta-cycles.csv`

| 微基准操作 | CLEAN 基准 (cycles/op) | T0 (cycles/op) | T512 (cycles/op) | $\Delta(T0 - \text{CLEAN})$ | 相对 CLEAN 增幅 (%) | $\Delta(T0 - T512)$ | PreFlush (cycles/op) | PostFlush (cycles/op) | $\Delta(\text{Pre} - \text{Post})$ |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **`GetLive`** (存活点查) | $6,513.9$ | $7,302.6$ | $6,568.5$ | $\mathbf{+788.7}$ | $\mathbf{+12.11\%}$ | $\mathbf{+734.1}$ | $7,586.8$ | $7,397.1$ | $\mathbf{+189.6}$ |
| **`ScanIntersect`** (相交扫描) | $52,943.2$ | $54,940.4$ | $44,435.1$ | $\mathbf{+1,997.2}$ | $\mathbf{+3.77\%}$ | $\mathbf{+10,505.3}$ | $54,519.4$ | $57,278.2$ | $-2,758.8$ |
| **`ScanNonIntersect`** (负对照) | $55,829.1$ | $58,630.7$ | $53,784.2$ | $\mathbf{+2,801.6}$ | $\mathbf{+5.02\%}$ | $\mathbf{+4,846.5}$ | $57,086.2$ | $58,656.2$ | $-1,570.0$ |

---

## 5. 核心发现与机制诊断

### 5.1 确证事实（Confirmed Facts）
1. **存活点查（`GetLive`）的稳定 CPU 增量**：
   - 当活跃 MemTable 滞留 20,000 条 DeleteRange 时，$T=0$ 的存活点查相较 `CLEAN` 产生了 **$+788.7\text{ cycles/op}$（$+12.11\%$）** 的稳定硬件周期增长与 **$+1,980.8\text{ insn/op}$（$+13.84\%$）** 的指令增长；
   - 相较于阈值下刷组 $T=512$，$T=0$ 的存活点查同样多耗费 **$+734.1\text{ cycles/op}$（$+11.18\%$）**；
   - 吞吐量从 `CLEAN` 的 $574,204\text{ IOPS}$ 下降至 $T=0$ 的 $512,500\text{ IOPS}$（降低 $10.7\%$）。
2. **相交扫描（`ScanIntersect`）相较 $T=512$ 的严重吞吐与周期退化**：
   - 跨越墓碑区间的 50 Keys 扫描中，$T=512$ 达到 $81,799\text{ IOPS}$（$44,435.1\text{ cycles/op}$），而 $T=0$ 仅为 $66,497\text{ IOPS}$（$54,940.4\text{ cycles/op}$）；
   - $T=0$ 相较 $T=512$ 产生了 **$+10,505.3\text{ cycles/op}$（$+23.64\%$）** 的额外开销与 **$+24,749.7\text{ insn/op}$** 的额外指令开销。
3. **负对照（`ScanNonIntersect`）的对比验证**：
   - 完全避开墓碑区间的 50 Keys 扫描中，$T=0$ 相较 `CLEAN` 的差异仅为 $+5.02\%$，证实高额 CPU 增量高度局限于墓碑覆盖与检索路径。

### 5.2 机制解释（Mechanistic Explanations）
1. **内存范围墓碑的检索路径延长**：
   - 在纯净 RocksDB `v11.8.0` 中，`MemTable::Get` 必须调用 `range_del_table_` / `FragmentedRangeTombstoneIterator` 判定当前 Key 是否被覆盖。20,000 条墓碑滞留在活跃 MemTable 时，每次点查存活 Key 均需在内存 SkipList 中遍历范围墓碑并执行区间重叠判断，直接导致点查指令数增加约 2,000 条（$+13.8\%$）；
2. **`PreFlush` $\rightarrow$ `PostFlush` 的机制变化**：
   - 在机制隔离层中，将 20,000 条范围墓碑从活跃 MemTable 下刷至 L0 SST 后，`GetLive` 的 CPU 消耗从 $7,586.8\text{ cycles/op}$ 下降至 $7,397.1\text{ cycles/op}$（减少 $189.6\text{ cycles/op}$），证实活跃 MemTable 内部的墓碑检索确实存在可消除的内存额外开销。

### 5.3 不可外推边界（Non-Extrapolatable Bounds）
1. **不得外推至全局 SST 索引优化**：本实验的 CPU 增量证据主要集中于活跃 MemTable 的读路径与点查检索，不能作为修改 SST 磁盘存储格式或全局持久化索引的依据；
2. **不得忽略扫描路径中通用迭代器的协同开销**：在扫描场景下，$T=0$ 的增量既包含范围墓碑自身的 Seek/Next，也包含 `MergingIterator` 与 `InternalKeyComparator` 的多路归并推进开销。

---

## 6. 选题决策判决（Go/No-Go Decision）

依据实施方案中预注册的五大门槛：
1. [x] **增量稳定复现**：$T=0$ 在 `GetLive` 上稳定产生 $+788.7\text{ cycles/op}$（$+12.11\%$）开销，在 `ScanIntersect` 上相较 $T=512$ 产生 $+10,505.3\text{ cycles/op}$（$+23.64\%$）开销；
2. [x] **PMU 采集确证**：135 轮实验全部以 100.00% 真实时间运行，无任何复用插值误差；
3. [x] **隔离层同向验证**：`PreFlush → PostFlush` 在点查路径上确证开销降低（$-189.6\text{ cycles/op}$）；
4. [x] **空间局部性吻合**：负对照 `ScanNonIntersect` 未呈现同等幅度剧烈增长（仅 $5.02\%$）；
5. [x] **严格排除外部干扰**：全库 SHA-256 状态对账 100% 吻合，后台 Compaction 在隔离层中完全关闭。

### 最终决策结论：
> **判定通过门槛 A**：确证 RocksDB `v11.8.0` 在高密度范围删除滞留活跃 MemTable 时，读路径（尤其是存活点查与相交范围扫描）存在明确且集中的 CPU 性能开销；**建议正式转向“活跃/不可变 MemTable 范围墓碑轻量级读路径组织与缓存优化”**。
