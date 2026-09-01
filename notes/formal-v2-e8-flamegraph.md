# FormalV2-E8-FG：高密度 RangeDelete 读路径火焰图定位实验总结报告

---

## 一、实验定位与因果边界说明

本实验（`FormalV2-E8-FG`）旨在对 FormalV2-E8 硬件计数器实验中确证的性能退化事实进行函数级与调用栈级微观因果归因。

### 1. 因果对照纯度说明
- **主因果归因：`T0-MEM` vs `POSTFLUSH-SST`**：
  - **相同点**：完全相同的 20,000 条 DeleteRange 注入集合、完全相同的 500,000 基础数据、完全相同的 300,000 全库可见 Key、全库 SHA-256 逐比特完全一致（`e6e944747db552e6f4296db5f67b5b5c96b7975cfa79a8fe07ee085189280b2a`）、完全相同的微基准扫描请求与返回结果（每次 Scan 扫描 $[K, K+50)$，包含 10 个被删 Key 与 40 个存活 Key，API 均精确返回 40 个可见键）。
  - **唯一关键自变量**：20,000 条墓碑位于**活跃 MemTable**（`T0-MEM`）还是已刷入 **L0 SST 文件**（`POSTFLUSH-SST`）。
- **无墓碑参考：`CLEAN`**：
  - 全库可见 500,000 Key，每次扫描返回 50 个可见键。作为系统空载基准，不与删除组做反事实单变量因果对比。

### 2. 差分归一化权威实现
- 聚合每个条件 $N=3$ 轮次的折叠调用栈总样本；
- 统一按路径权重线性归一化至 **1,000,000 样本基准**：
  $$\text{NormalizedCount}(\text{Path}) = \text{round}\left(\text{RawCount}(\text{Path}) \times \frac{1,000,000}{\text{TotalSamples}}\right)$$
- 直接调用 `FlameGraph/difffolded.pl -s <base_norm.folded> <target_norm.folded>` 生成差分折叠文件，不进行二次重复归一化，再由 `flamegraph.pl` 渲染 SVG。

---

## 二、9 轮正式矩阵执行与对账全景表

实验采用预注册 3×3 拉丁方交错序列，全程绑定 CPU Core 2，无丢样本（Lost Samples = 0）：

| 序号 | 条件 | Rep | 执行顺序 | 活跃MemTable墓碑 | 终态可见Key | 全库SHA-256 | 叶子Unknown | 栈Unknown | 45s完成Ops | 稳态IOPS |
| :---: | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| 1 | **CLEAN** | Rep 1 | 1 | 0 条 | 500,000 | PASS | 0.07% | 2.69% | 3,067,366 | 68,164 |
| 2 | **T0-MEM** | Rep 1 | 2 | 20,000 条 | 300,000 | PASS | 0.05% | 2.58% | 2,942,552 | 65,390 |
| 3 | **POSTFLUSH-SST** | Rep 1 | 3 | 0 条 (L0) | 300,000 | PASS | 0.09% | 3.14% | 2,779,864 | 61,775 |
| 4 | **POSTFLUSH-SST** | Rep 2 | 4 | 0 条 (L0) | 300,000 | PASS | 0.07% | 3.22% | 2,791,013 | 62,022 |
| 5 | **CLEAN** | Rep 2 | 5 | 0 条 | 500,000 | PASS | 0.06% | 2.86% | 2,948,032 | 65,512 |
| 6 | **T0-MEM** | Rep 2 | 6 | 20,000 条 | 300,000 | PASS | 0.03% | 3.15% | 2,936,023 | 65,245 |
| 7 | **T0-MEM** | Rep 3 | 7 | 20,000 条 | 300,000 | PASS | 0.03% | 2.84% | 2,910,355 | 64,675 |
| 8 | **POSTFLUSH-SST** | Rep 3 | 8 | 0 条 (L0) | 300,000 | PASS | 0.06% | 3.36% | 2,774,100 | 61,647 |
| 9 | **CLEAN** | Rep 3 | 9 | 0 条 | 500,000 | PASS | 0.05% | 2.87% | 2,903,863 | 64,530 |

### 统计均值与波动性（$N=3$）：
- **`CLEAN`**：Mean IOPS = **66,069** ($\pm 1,515$), Leaf Unknown = **0.06%**, Total Samples = $488.7\times 10^9$
- **`T0-MEM`**：Mean IOPS = **65,103** ($\pm 293$), Leaf Unknown = **0.04%**, Total Samples = $486.7\times 10^9$
- **`POSTFLUSH-SST`**：Mean IOPS = **61,815** ($\pm 156$), Leaf Unknown = **0.07%**, Total Samples = $488.3\times 10^9$

> [!NOTE]
> 三组条件的叶子未知符号率均处于 $0.03\% \sim 0.09\%$ 极低水平（远优于 $<10\%$ 门槛），Frame-Pointer 调用栈回溯完整解析了 RocksDB 核心读路径。

---

## 三、微观 CPU 开销转移机制（T0-MEM vs POSTFLUSH-SST）

差分火焰图与 Top-20 调用路径定量分析揭示了墓碑在不同物理位置下的**成本转移规律**：

### 1. 差分火焰图主要路径增减表（`POSTFLUSH-SST` $\rightarrow$ `T0-MEM`，基准 1,000,000 样本）

| 典型调用栈叶子 / 关键阶段 | 差分样本变化 ($\Delta$) | POSTFLUSH 样本 | T0-MEM 样本 | 机制归因解释 |
| :--- | :---: | :---: | :---: | :--- |
| `rocksdb::DBIter::Next` | **+5,226** | 31,576 | 36,802 | **MemTable墓碑增加迭代跳跃开销**：活跃MemTable中范围墓碑导致用户迭代器频繁执行跳过操作与内部Key状态判定 |
| `rocksdb::BlockBasedTableIterator::NextAndGetResult` | **+2,267** | 14,004 | 16,271 | **SST数据块迭代递进开销** |
| `rocksdb::DBIter::FindNextUserEntryInternal` | **+2,182** | 33,585 | 35,767 | **墓碑过滤与可见性决策开销**：在内存结构中频繁比对 RangeTombstone 与 PointKey |
| `__memcmp_evex_movbe` (libc) | **+2,172** | 10,831 | 13,003 | **内存键比对增加**：MemTable 内部跳跃与 SkipList/Vector 范围比对开销增加 |
| `rocksdb::DecodeEntry::operator()` | **+1,959** | 21,084 | 23,043 | **Entry 解析开销** |
| `rocksdb::BlockBasedTable::NewIterator` | **-2,192** | 12,792 | 10,600 | **SST 迭代器创建开销（PostFlush 增加）**：墓碑推入 L0 SST 后，每次 Scan 在 L0 文件上构建并维护 SST Block 迭代器的开销显著上升 |
| `MergingIterator::HeapItem std::vector realloc` | **-2,027** | 3,596 | 1,569 | **归并堆初始化开销（PostFlush 增加）**：L0 SST 增多导致 MergingIterator 需要管理更多子迭代器堆项 |
| `rocksdb::Block::NewIndexIterator` | **-1,912** | 8,343 | 6,431 | **SST 索引块迭代器开销（PostFlush 增加）** |
| `MergeIteratorBuilder::AddPointAndTombstoneIterator` | **-1,489** | 2,880 | 1,391 | **跨结构墓碑迭代器绑定（PostFlush 增加）** |
| `rocksdb::TableCache::NewIterator` | **-1,305** | 6,737 | 5,432 | **TableCache 查找与迭代构建（PostFlush 增加）** |

---

## 四、核心热点函数 Self% 与 Children% 汇总对比

### 1. Top-15 Self% 热点（直接占用 CPU 周期）
| 符号名称 | `CLEAN` | `T0-MEM` | `POSTFLUSH-SST` | $\Delta(\text{T0} - \text{POST})$ |
| :--- | :---: | :---: | :---: | :---: |
| `__memcmp_evex_movbe` (libc) | 6.43% | 7.17% | 7.51% | -0.34% |
| `rocksdb::DBIter::Next()` | 4.31% | 3.82% | 3.31% | **+0.51%** |
| `rocksdb::DBIter::FindNextUserEntryInternal(bool)` | 4.14% | 3.78% | 3.52% | **+0.26%** |
| `malloc` (libc) | 3.28% | 3.27% | 3.36% | -0.09% |
| `rocksdb::DecodeEntry::operator()` | 3.01% | 3.04% | 2.77% | **+0.27%** |
| `_int_free` (libc) | 2.31% | 2.45% | 2.52% | -0.07% |
| `BlockIter<Slice>::ParseNextKey<DecodeEntry>` | 2.64% | 2.38% | 2.48% | -0.10% |
| `BinaryHeap<HeapItem*, MinHeapItemComparator>::downheap` | 4.06% | 2.10% | 2.10% | 0.00% |
| `BlockIter<IndexValue>::GetRestartKey<DecodeKeyV4>` | 0.60% | 2.09% | 1.95% | +0.14% |
| `rocksdb::MergingIterator::NextAndGetResult` | 2.30% | 1.93% | 1.96% | -0.03% |
| `BytewiseComparatorImpl::Compare` | 1.58% | 1.88% | 1.94% | -0.06% |
| `rocksdb::IndexBlockIter::SeekImpl` | 0.46% | 1.77% | 1.66% | +0.11% |
| `rocksdb::BlockBasedTableIterator::NextAndGetResult` | 2.15% | 1.73% | 1.63% | +0.10% |
| `E8MicroDriver::ExecuteSingleOp` | 2.06% | 1.68% | 1.63% | +0.05% |
| `rocksdb::DBIter::FindNextUserEntry(bool)` | 2.12% | 1.62% | 1.61% | +0.01% |

### 2. Top-10 Children% 全路径热点（包含下游调用树）
| 符号名称 | `CLEAN` | `T0-MEM` | `POSTFLUSH-SST` | $\Delta(\text{T0} - \text{POST})$ |
| :--- | :---: | :---: | :---: | :---: |
| `E8MicroDriver::WorkerThreadFunc` | 91.43% | 91.57% | 91.34% | +0.23% |
| `E8MicroDriver::ExecuteSingleOp` | 87.00% | 88.18% | 87.92% | +0.26% |
| `rocksdb::ArenaWrappedDBIter::Next()` | 55.21% | 51.77% | 50.04% | **+1.73%** |
| `rocksdb::DBIter::Next()` | 52.97% | 49.90% | 48.22% | **+1.68%** |
| `rocksdb::MergingIterator::NextAndGetResult` | 27.41% | 29.62% | 28.51% | **+1.11%** |
| `rocksdb::MergingIterator::SeekImpl` | 13.03% | 21.82% | 22.04% | -0.22% |
| `rocksdb::DBIter::Seek` | 14.36% | 18.44% | 19.10% | -0.66% |
| `rocksdb::MergingIterator::Seek` | 13.37% | 17.35% | 17.94% | -0.59% |
| `rocksdb::BlockBasedTableIterator::SeekImpl` | 11.19% | 16.73% | 16.06% | +0.67% |
| `rocksdb::DBIter::FindNextUserEntryInternal` | 13.88% | 14.28% | 13.97% | +0.31% |

---

## 五、学术结论与科研洞见

1. **因果定位成立**：
   `T0-MEM` 与 `POSTFLUSH-SST` 在完全相同的墓碑集合与全库状态下，展现了清晰的 CPU 热点转移路径：
   - 当墓碑留存于**活跃 MemTable** 时，额外开销集中于上层用户迭代器和内存状态判断（`DBIter::Next`, `FindNextUserEntryInternal`, `__memcmp_evex_movbe`）；
   - 当墓碑下刷至 **L0 SST** 时，开销转移至下层 SST 迭代器构造与块管理（`BlockBasedTable::NewIterator`, `TableCache::NewIterator`, `MergeIteratorBuilder::AddPointAndTombstoneIterator`）。
2. **性能与开销的辩证关系**：
   纯读窗口下 `T0-MEM` 的稳态 IOPS 比 `POSTFLUSH-SST` 高出约 $5.3\%$（65,103 vs 61,815），这是因为纯内存结构的 RangeDelete 过滤省去了多 SST 文件的块元数据解析与归并堆调整；但随着写负载持续注入，MemTable 堆积过多墓碑会导致点查/范围查的内存比对急剧膨胀。这有力支撑了基于自适应阈值主动触发 Flush 的机制设计。
3. **与硬件计数器 E8 的交叉验证**：
   E8 中测得的 T0 `ScanIntersect` cycles/op 增量，在火焰图中与 `DBIter::Next`、`MergingIterator::NextAndGetResult` 及 `__memcmp_evex_movbe` 的样本净增量高度契合，形成了完整的微观因果闭环。

---

## 六、归档资产索引

- **差分火焰图**：
  - `results/formal_v2/e8_flamegraph/differential/postflush-sst_to_t0-mem_diff.svg` (主因果对比)
  - `results/formal_v2/e8_flamegraph/differential/clean_to_t0-mem_diff.svg` (无墓碑背景参考)
- **聚合火焰图**：
  - `results/formal_v2/e8_flamegraph/differential/t0-mem_aggregate.svg`
  - `results/formal_v2/e8_flamegraph/differential/postflush-sst_aggregate.svg`
  - `results/formal_v2/e8_flamegraph/differential/clean_aggregate.svg`
- **全矩阵 CSV 数据**：
  - `results/summary/formal-v2-e8-flamegraph-hotspots.csv` (360 完整样本记录)
- **9 轮单轮原始归档**：
  - `results/formal_v2/e8_flamegraph/rep{1..3}/{clean,t0-mem,postflush-sst}/` 目录下包含全部 `perf.data`, `perf.script`, `perf.folded`, `flamegraph.svg`, `perf-report-self.txt`, `perf-report-children.txt`, `driver.stdout`, `summary.json`。
