# RocksDB v11.8.0 读路径与范围删除机制源代码审计报告

- **审计基准**：RocksDB `v11.8.0` 官方源码（本地目录 `/home/wam/grad/rocksdb-v11.8.0`）
- **审计目标**：全面梳理 Point Get、固定范围 Scan、Limit-K Scan 的真实源码调用链，深入分析范围墓碑在各层级的组织、生命周期、判定与跳过机理，严格区隔“源码事实”与“待验证候选性能机制”。

---

## 目录
1. [三大读操作真实调用链梳理](#1-三大读操作真实调用链梳理)
   - [1.1 Point Get 调用链](#11-point-get-调用链)
   - [1.2 固定范围 Scan 调用链（Bounded Scan）](#12-固定范围-scan-调用链bounded-scan)
   - [1.3 Limit-K Scan 调用链（Unbounded/K-Item Scan）](#13-limit-k-scan-调用链unboundedk-item-scan)
2. [范围墓碑对象、迭代器、缓存与生命周期（按层级划分）](#2-范围墓碑对象迭代器缓存与生命周期按层级划分)
   - [2.1 Active MemTable](#21-active-memtable)
   - [2.2 Immutable MemTable (Imm)](#22-immutable-memtable-imm)
   - [2.3 L0 与 SST 文件](#23-l0-与-sst-文件)
3. [墓碑分段视图的构建、失效、锁竞争与复用机制](#3-墓碑分段视图的构建失效锁竞争与复用机制)
4. [Point Get 中的范围墓碑覆盖判定与序列号可见性检查](#4-point-get-中的范围墓碑覆盖判定与序列号可见性检查)
5. [Scan 中的迭代器构建、Seek、Next、边界推进与职责分工](#5-scan-中的迭代器构建seeknext边界推进与职责分工)
6. [Flush 后范围墓碑进入 SST 的编码、读取与迭代器构建路径](#6-flush-后范围墓碑进入-sst-的编码读取与迭代器构建路径)
7. [核心机制审计对照总表（源码事实 vs 待验证候选机制）](#7-核心机制审计对照总表源码事实-vs-待验证候选机制)

---

## 1. 三大读操作真实调用链梳理

### 1.1 Point Get 调用链
- **用户接口**：`DB::Get(const ReadOptions&, ColumnFamilyHandle*, const Slice&, std::string*)`
- **源码文件**：[db/db_impl/db_impl_sync_and_async.h](file:///home/wam/grad/rocksdb-v11.8.0/db/db_impl/db_impl_sync_and_async.h)
- **调用路径**：
  1. `DBImpl::Get`（[db_impl_sync_and_async.h:26](file:///home/wam/grad/rocksdb-v11.8.0/db/db_impl/db_impl_sync_and_async.h#L26)）
     - 校验并填充 `ReadOptions::io_activity = Env::IOActivity::kGet`。
     - 调用 `DBImpl::GetImpl`（[db_impl_sync_and_async.h:70](file:///home/wam/grad/rocksdb-v11.8.0/db/db_impl/db_impl_sync_and_async.h#L70)）。
  2. `DBImpl::GetImpl`
     - **获取 SuperVersion**（[Line 133](file:///home/wam/grad/rocksdb-v11.8.0/db/db_impl/db_impl_sync_and_async.h#L133)）：`SuperVersion* sv = cfd->GetReferencedSuperVersion(this);`（通过引用计数/线程本地缓存保护读视图）。
     - **快照序号获取**（[Line 163](file:///home/wam/grad/rocksdb-v11.8.0/db/db_impl/db_impl_sync_and_async.h#L163)）：若无指定快照，获取最新发布序号：`snapshot = GetLastPublishedSequence();`。
     - **构造查询键**（[Line 187](file:///home/wam/grad/rocksdb-v11.8.0/db/db_impl/db_impl_sync_and_async.h#L187)）：`LookupKey lkey(key, snapshot, read_options.timestamp);`。
     - **初始化覆盖序号**（[Line 233](file:///home/wam/grad/rocksdb-v11.8.0/db/db_impl/db_impl_sync_and_async.h#L233)）：`SequenceNumber max_covering_tombstone_seq = 0;`。
     - **检索 Active MemTable**（[Line 240](file:///home/wam/grad/rocksdb-v11.8.0/db/db_impl/db_impl_sync_and_async.h#L240)）：
       调用 `sv->mem->Get(lkey, value, ..., &max_covering_tombstone_seq, ...)`（见后文详细分析）。
     - **检索 Immutable MemTables**（[Line 264](file:///home/wam/grad/rocksdb-v11.8.0/db/db_impl/db_impl_sync_and_async.h#L264)）：
       若活跃 MemTable 未命中且未终止，调用 `sv->imm->Get(lkey, value, ..., &max_covering_tombstone_seq, ...)`。
     - **检索 LSM SST 文件**（[Line 301](file:///home/wam/grad/rocksdb-v11.8.0/db/db_impl/db_impl_sync_and_async.h#L301)）：
       若内存层未决，调用 `sv->current->Get(read_options, lkey, value, ..., &max_covering_tombstone_seq, ...)` 进入 `Version::Get`。
     - **释放 SuperVersion**（[Line 210](file:///home/wam/grad/rocksdb-v11.8.0/db/db_impl/db_impl_sync_and_async.h#L210)）：利用 RAII 守卫 `CleanupSuperVersion(sv)` 释放引用。

---

### 1.2 固定范围 Scan 调用链（Bounded Scan）
- **特征**：指定了扫描边界（如设置了 `read_options.iterate_upper_bound`），或在前台以 `while (iter->Valid() && iter->key() < end_key)` 约束终止。
- **源码文件**：[db/db_impl/db_impl.cc](file:///home/wam/grad/rocksdb-v11.8.0/db/db_impl/db_impl.cc)、[db/arena_wrapped_db_iter.cc](file:///home/wam/grad/rocksdb-v11.8.0/db/arena_wrapped_db_iter.cc)、[db/db_iter.cc](file:///home/wam/grad/rocksdb-v11.8.0/db/db_iter.cc)
- **调用路径**：
  1. `DBImpl::NewIterator`（[db_impl.cc:4116](file:///home/wam/grad/rocksdb-v11.8.0/db/db_impl/db_impl.cc#L4116)）
     - 获取当前 `SuperVersion* sv = cfd->GetReferencedSuperVersion(this);`（[Line 4153](file:///home/wam/grad/rocksdb-v11.8.0/db/db_impl/db_impl.cc#L4153)）。
     - 调用 `NewArenaWrappedDbIterator(...)`（[Line 4261](file:///home/wam/grad/rocksdb-v11.8.0/db/db_impl/db_impl.cc#L4261)）构建 `ArenaWrappedDBIter`，此时内部底层迭代器采用延迟初始化（Deferred Init）。
  2. `ArenaWrappedDBIter::Seek(target)`（[arena_wrapped_db_iter.cc:130](file:///home/wam/grad/rocksdb-v11.8.0/db/arena_wrapped_db_iter.cc#L130)）
     - 首次调用触发 `EnsureInternalIteratorInitialized`（[Line 79](file:///home/wam/grad/rocksdb-v11.8.0/db/arena_wrapped_db_iter.cc#L79)）。
     - 调用 `DBImpl::NewInternalIterator`（[db_impl.cc:2572](file:///home/wam/grad/rocksdb-v11.8.0/db/db_impl/db_impl.cc#L2572)）：
       - 创建 `MergeIteratorBuilder merge_iter_builder`，传入 `read_options.iterate_upper_bound`。
       - 收集活跃 MemTable 的 point iter 与 tombstone iter（[Line 2597-2612](file:///home/wam/grad/rocksdb-v11.8.0/db/db_impl/db_impl.cc#L2597-L2612)）。
       - 收集不可变 MemTable 的迭代器列表（[Line 2625](file:///home/wam/grad/rocksdb-v11.8.0/db/db_impl/db_impl.cc#L2625)）。
       - 收集 SST 文件迭代器（[Line 2635](file:///home/wam/grad/rocksdb-v11.8.0/db/db_impl/db_impl.cc#L2635)）。
       - 调用 `merge_iter_builder.Finish(db_iter)` 生成多路归并的 `MergingIterator`。
  3. `DBIter::Seek(target)`（[db_iter.cc:1967](file:///home/wam/grad/rocksdb-v11.8.0/db/db_iter.cc#L1967)）
     - 底层多路归并定位：`iter_.Seek(saved_key_.GetInternalKey());`（[Line 2046](file:///home/wam/grad/rocksdb-v11.8.0/db/db_iter.cc#L2046)）。
     - 寻找首个可见有效键：`FindNextUserEntry(false);`（[Line 2063](file:///home/wam/grad/rocksdb-v11.8.0/db/db_iter.cc#L2063)）。
       在 `FindNextUserEntryInternal` 中比对 `iterate_upper_bound`（[Line 521-532](file:///home/wam/grad/rocksdb-v11.8.0/db/db_iter.cc#L521-L532)），一旦超出直接终止。
  4. 推进循环：`DBIter::Next()`（[db_iter.cc:213](file:///home/wam/grad/rocksdb-v11.8.0/db/db_iter.cc#L213)）
     - 底层推进：`iter_.Next();`（[Line 241](file:///home/wam/grad/rocksdb-v11.8.0/db/db_iter.cc#L241)），触发 `MergingIterator` 堆重排。
     - 上层过滤：`FindNextUserEntry(true);`（[Line 249](file:///home/wam/grad/rocksdb-v11.8.0/db/db_iter.cc#L249)），跳过被覆盖键与已删除键，并再次检查是否到达 `iterate_upper_bound`。

---

### 1.3 Limit-K Scan 调用链（Unbounded/K-Item Scan）
- **特征**：上层未设置 `iterate_upper_bound`，由客户端维护计数器，在循环调用 $K$ 次 `iter->Next()` 后主动跳出（例如 E8 的 `ScanIntersect`：扫描 50 条有效键）。
- **与固定范围 Scan 的底层差异**：
  1. **构建期**：`MergeIteratorBuilder` 中的 `iterate_upper_bound` 为 `nullptr`，`MergingIterator` 不能将上界下推至范围墓碑堆或 SST 块寻址；
  2. **墓碑入堆**：在 `MergingIterator::InsertRangeTombstoneToMinHeap`（[table/merging_iterator.cc:171](file:///home/wam/grad/rocksdb-v11.8.0/table/merging_iterator.cc#L171)）中，由于没有 `iterate_upper_bound_`，所有范围墓碑的 `end_key` 均必须无条件进入堆，无法在上界提前剔除；
  3. **跳出逻辑**：每一次 `iter->Next()` 执行相同的 `MergingIterator::SkipNextDeleted()` 与 `DBIter::FindNextUserEntryInternal`，由上层循环计数达到 $K$ 时析构 `Iterator`，通过 `CleanupSuperVersionHandle` 归还底层读视图资源。

---

## 2. 范围墓碑对象、迭代器、缓存与生命周期（按层级划分）

### 2.1 Active MemTable
- **存储对象**：
  - 类：`MemTable`（[db/memtable.h:60](file:///home/wam/grad/rocksdb-v11.8.0/db/memtable.h#L60)）；
  - 范围删除物理容器：`std::unique_ptr<MemTableRep> range_del_table_`（[Line 204](file:///home/wam/grad/rocksdb-v11.8.0/db/memtable.h#L204)）；
  - 默认为无锁 `InlineSkipList`。普通键写入 `table_`，范围删除写入 `range_del_table_`，二者物理隔离。
- **缓存对象**：
  - `CoreLocalArray<std::shared_ptr<FragmentedRangeTombstoneListCache>> cached_range_tombstone_`（[db/memtable.h:207](file:///home/wam/grad/rocksdb-v11.8.0/db/memtable.h#L207)）；
  - 每个 CPU Core 独立持有指针，内部包装 `FragmentedRangeTombstoneListCache`（包含 `reader_mutex`、`initialized` 原子布尔标志、以及 `std::unique_ptr<FragmentedRangeTombstoneList> tombstones`）。
- **迭代器对象**：
  - `FragmentedRangeTombstoneIterator`（[db/range_tombstone_fragmenter.h:134](file:///home/wam/grad/rocksdb-v11.8.0/db/range_tombstone_fragmenter.h#L134)）；
  - 持有 `std::shared_ptr<FragmentedRangeTombstoneListCache>` 的引用。
- **生命周期边界**：
  - 当 MemTable 处于 Active 状态时，每次前台写入 `DeleteRange` 均会导致 `cached_range_tombstone_` 被重置（见第 3 节）；
  - 当 MemTable 被 Freeze/Switch 转换为不可变状态时，调用 `ConstructFragmentedRangeTombstones()`（[db/memtable.cc:868](file:///home/wam/grad/rocksdb-v11.8.0/db/memtable.cc#L868)）将其分段墓碑列表持久冻结为 `fragmented_range_tombstone_list_`，此后不再失效。

---

### 2.2 Immutable MemTable (Imm)
- **存储与迭代器对象**：
  - 容器：`MemTableListVersion`（[db/memtable_list.h:79](file:///home/wam/grad/rocksdb-v11.8.0/db/memtable_list.h#L79)）；
  - 每个只读 MemTable 的 `is_range_del_table_empty_` 状态恒定；
  - 迭代器通过 `MemTable::NewRangeTombstoneIterator` 生成，传入 `immutable_memtable = true`（[db/memtable.cc:920](file:///home/wam/grad/rocksdb-v11.8.0/db/memtable.cc#L920)）。
- **缓存与生命周期**：
  - **直接复用**已冻结的 `fragmented_range_tombstone_list_`（[db/memtable.cc:924](file:///home/wam/grad/rocksdb-v11.8.0/db/memtable.cc#L924)）；
  - **无互斥锁，零分段重构**；
  - 生命周期由包含它的 `MemTableListVersion` 及引用它的 `SuperVersion` 控制，直到 Flush 完毕并在全部读迭代器释放后析构。

---

### 2.3 L0 与 SST 文件
- **存储对象**：
  - 范围删除不写在普通 Data Block 中，而是单独写入 SST 的 Meta Block，块名称为 `kRangeDelBlockName`（`"rocksdb.range-del"`，[include/rocksdb/table.h:29](file:///home/wam/grad/rocksdb-v11.8.0/include/rocksdb/table.h#L29)）。
- **读取与缓存对象**：
  - 类：`BlockBasedTable`（[table/block_based/block_based_table_reader.cc](file:///home/wam/grad/rocksdb-v11.8.0/table/block_based/block_based_table_reader.cc)）；
  - 句柄：`rep_->fragmented_range_dels`（[Line 1620](file:///home/wam/grad/rocksdb-v11.8.0/table/block_based/block_based_table_reader.cc#L1620)），类型为 `std::shared_ptr<FragmentedRangeTombstoneList>`；
  - 当打开 SST（`BlockBasedTable::Open`）或首次读取元数据时一次性从 Meta Block 读取并构建完成，**不可变常驻内存**。
- **迭代器对象**：
  - `BlockBasedTable::NewRangeTombstoneIterator`（[Line 2630](file:///home/wam/grad/rocksdb-v11.8.0/table/block_based/block_based_table_reader.cc#L2630)）创建 `FragmentedRangeTombstoneIterator`；
  - 扫描时由 `TruncatedRangeDelIterator`（[db/range_del_aggregator.h:184](file:///home/wam/grad/rocksdb-v11.8.0/db/range_del_aggregator.h#L184)）包装，将其边界严格截断至该 SST 文件的 `[smallest, largest]` 物理键范围内。
- **生命周期**：
  - 绑定于 `TableReader` 及其在 `TableCache` 中的缓存生命周期；文件未被 Compaction 物理删除且 Version 存活期间始终有效。

---

## 3. 墓碑分段视图的构建、失效、锁竞争与复用机制

### 3.1 源码实现事实（Source Code Facts）
1. **写端失效路径**：
   - 文件：[db/memtable.cc:1248-1269](file:///home/wam/grad/rocksdb-v11.8.0/db/memtable.cc#L1248-L1269)
   - 函数：`MemTable::Add`
   - 代码事实：每次插入类型为 `kTypeRangeDeletion` 时，分配一个全新的 `new_cache = std::make_shared<FragmentedRangeTombstoneListCache>()`（其内部 `initialized` 初始值为 `false`），并以原子存储循环替换所有 CPU Core 槽位中的本地缓存指针：
     ```cpp
     for (size_t i = 0; i < size; ++i) {
       std::shared_ptr<FragmentedRangeTombstoneListCache>* local_cache_ref_ptr =
           cached_range_tombstone_.AccessAtCore(i);
       ...
       AtomicSharedPtrStore(local_cache_ref_ptr, std::move(aliased_ptr), std::memory_order_relaxed);
     }
     ```
2. **读端构建与锁竞争路径**：
   - 文件：[db/memtable.cc:917-949](file:///home/wam/grad/rocksdb-v11.8.0/db/memtable.cc#L917-L949)
   - 函数：`MemTable::NewRangeTombstoneIteratorInternal`
   - 代码事实：读线程获取当前 Core 的 cache。若 `!cache->initialized.load(std::memory_order_acquire)`，执行：
     ```cpp
     cache->reader_mutex.lock();
     if (!cache->tombstones) {
       auto* unfragmented_iter = new MemTableIterator(
           MemTableIterator::kRangeDelEntries, *this, read_options);
       cache->tombstones.reset(new FragmentedRangeTombstoneList(
           std::unique_ptr<InternalIterator>(unfragmented_iter),
           comparator_.comparator));
       cache->initialized.store(true, std::memory_order_release);
     }
     cache->reader_mutex.unlock();
     ```
3. **分段重构算法**：
   - 文件：[db/range_tombstone_fragmenter.cc:89-180](file:///home/wam/grad/rocksdb-v11.8.0/db/range_tombstone_fragmenter.cc#L89-L180)
   - 函数：`FragmentedRangeTombstoneList::FragmentTombstones`
   - 代码事实：利用红黑树 `std::set<ParsedInternalKey, ParsedInternalKeyComparator> cur_end_keys` 追踪重叠区间，将无序/重叠的墓碑切分为互不相交的 `RangeTombstoneStack` 栈。
4. **不可变复用**：
   - 代码事实：一旦 MemTable 转入 Immutable 或下刷成 SST，其分段视图不再重新构建，只读复用已有结构。

### 3.2 待验证候选性能机制（Candidate Performance Mechanisms）
- **候选机制 C1**：在写读混合（持续有 DeleteRange 写入）场景下，写线程高频重置 `cached_range_tombstone_`，导致读线程反复陷入冷启动重建（Cold Build），并发读线程在 `reader_mutex` 上产生锁等待排队。
- **候选机制 C2**：当活跃 MemTable 累积墓碑达到数万量级时，`FragmentTombstones` 单次红黑树切分的 CPU 耗时显著拉长，成为该次读请求的极端长尾延迟来源。
- **待验证判定标准**：需依赖第二阶段最小探针分别记录冷构建（`memtable_fragment_build_count/time`）与稳态命中，判断是否存在高频重建。

---

## 4. Point Get 中的范围墓碑覆盖判定与序列号可见性检查

### 4.1 源码实现事实（Source Code Facts）
1. **活跃 MemTable 点查拦截入口**：
   - 文件：[db/memtable.cc:1584-1600](file:///home/wam/grad/rocksdb-v11.8.0/db/memtable.cc#L1584-L1600)
   - 函数：`MemTable::Get`
   - 代码事实：在查询普通跳表前，若 `!is_range_del_table_empty_`，无条件执行：
     ```cpp
     std::unique_ptr<FragmentedRangeTombstoneIterator> range_del_iter(
         NewRangeTombstoneIterator(read_opts, GetInternalKeySeqno(key.internal_key()), immutable_memtable));
     if (range_del_iter != nullptr) {
       SequenceNumber covering_seq = range_del_iter->MaxCoveringTombstoneSeqnum(key.user_key());
       if (covering_seq > *max_covering_tombstone_seq) {
         *max_covering_tombstone_seq = covering_seq;
       }
     }
     ```
2. **覆盖查询算法**：
   - 文件：[db/range_tombstone_fragmenter.cc:484-491](file:///home/wam/grad/rocksdb-v11.8.0/db/range_tombstone_fragmenter.cc#L484-L491)
   - 函数：`FragmentedRangeTombstoneIterator::MaxCoveringTombstoneSeqnum`
   - 代码事实：调用 `SeekToCoveringTombstone(target_user_key)`，在分段数组 `tombstones_` 上使用 `std::upper_bound` 进行二分查找，返回覆盖该 user_key 且其 `sequence <= upper_bound_`（读快照序号）的最大墓碑序列号；若无覆盖则返回 0。
3. **序列号可见性比对与类型改写**：
   - 文件：[table/get_context.cc:401-412](file:///home/wam/grad/rocksdb-v11.8.0/table/get_context.cc#L401-L412)
   - 函数：`GetContext::SaveValue`
   - 代码事实：无论是在 MemTable 跳表中查到点，还是在底层 SST 数据块中查到点，只要提取出的点键序号满足 `parsed_key.sequence < *max_covering_tombstone_seq_`，代码强制将该记录类型改写：
     ```cpp
     type = kTypeRangeDeletion;
     ```
     随后进入 `case kTypeRangeDeletion:`（[Line 593](file:///home/wam/grad/rocksdb-v11.8.0/table/get_context.cc#L593)），将状态置为 `state_ = kDeleted`，并直接返回 `false` 终止查找，向上层返回 NotFound。
4. **SST 文件级短路**：
   - 文件：[db/version_set_sync_and_async.h:65-68](file:///home/wam/grad/rocksdb-v11.8.0/db/version_set_sync_and_async.h#L65-L68)
   - 函数：`Version::Get`
   - 代码事实：当 `*max_covering_tombstone_seq > 0` 且后续 SST 文件的最大序号小于该墓碑序号时，循环直接 `break`，不再检查更深层的文件。

### 4.2 待验证候选性能机制（Candidate Performance Mechanisms）
- **候选机制 C3**：在稳态读（Warm Steady）且无写冲突情况下，即使墓碑分段视图已建好，每一次点查仍然必须执行 `MaxCoveringTombstoneSeqnum` 的二分查找；在高密度墓碑下，点查的指令开销增量（如 E8 观测到的 $+13.8\%$ 指令增量）主要由此稳态二分查找及 CPU Cache Miss 贡献。
- **待验证判定标准**：需通过探针分别度量稳态下 `memtable_tombstone_query_count/time` 的净耗时，并核实其占点查总耗时的比重。

---

## 5. Scan 中的迭代器构建、Seek、Next、边界推进与职责分工

### 5.1 职责分工与架构分层（Source Code Facts）
RocksDB 扫描体系采用明确的职责分层：
- **`ArenaWrappedDBIter`**：包装层，负责持有 Arena 内存池、维护 `SuperVersion` 生命周期并在需要时执行延迟初始化或自动 Refresh；
- **`DBIter`**：面向用户的顶层迭代器，负责可见性过滤（`IsVisible`）、点删除过滤（`kTypeDeletion`）、多版本同键折叠（Duplicate Key Skip）及边界检查；
- **`MergingIterator`**：底层的多路堆归并引擎，**承载范围墓碑的全部堆推进、区间相交判断与级联跳跃（Cascading Seek）职责**。

### 5.2 范围墓碑在 `MergingIterator` 中的堆驱动状态机（Source Code Facts）
1. **离散事件驱动模型**：
   - 文件：[table/merging_iterator.cc:155-240](file:///home/wam/grad/rocksdb-v11.8.0/table/merging_iterator.cc#L155-L240)
   - 代码事实：每个层级（Active MemTable、各 Imm MemTable、各 L0 文件、各 L1+ concatenated run）若包含范围墓碑，其墓碑分段 $[start, end)$ 被拆为两个离散事件进入堆：
     - 起始事件：`HeapItem::Type::DELETE_RANGE_START`
     - 结束事件：`HeapItem::Type::DELETE_RANGE_END`
2. **边界推进机制**：
   - 函数：`PopDeleteRangeStart`（[table/merging_iterator.cc:231](file:///home/wam/grad/rocksdb-v11.8.0/table/merging_iterator.cc#L231)）
     当 `minHeap_` 堆顶为 `DELETE_RANGE_START` 时，弹出该项，向活跃集合 `active_` 插入该层级（`active_.insert(level)`），并将对应的 `DELETE_RANGE_END` 作为新堆项压入 `minHeap_`。
   - 函数：`SkipNextDeleted`（[table/merging_iterator.cc:943](file:///home/wam/grad/rocksdb-v11.8.0/table/merging_iterator.cc#L943)）
     当堆顶为 `DELETE_RANGE_END` 时，说明当前活跃墓碑区间已走完，从 `active_` 移除该层级（`active_.erase(level)`），推进该层墓碑迭代器（`range_tombstone_iters_[level]->Next()`），若后续仍有墓碑则将其新的 `DELETE_RANGE_START` 重新推入堆。
3. **级联跳跃（Cascading Seek）**：
   - 文件：[table/merging_iterator.cc:1034-1045](file:///home/wam/grad/rocksdb-v11.8.0/table/merging_iterator.cc#L1034-L1045)
   - 代码事实：当堆顶为普通点键（来自层级 $L$）时，检查 `active_` 集合中是否存在更年轻层级的墓碑 $i < L$。若存在，该点键必然被覆盖，`MergingIterator` 执行：
     ```cpp
     std::string target;
     AppendInternalKey(&target, range_tombstone_iters_[i]->end_key());
     SeekImpl(target, current->level, true /* range_tombstone_reseek */);
     ```
     直接把层级 $L$ 的子迭代器向前 Seek 到该墓碑的 `end_key`，避免底层扫描做无效的逐键遍历。
4. **同层覆盖比对**：
   - 文件：[table/merging_iterator.cc:1046-1067](file:///home/wam/grad/rocksdb-v11.8.0/table/merging_iterator.cc#L1046-L1067)
   - 代码事实：若 $i == L$（同在活跃 MemTable 或同在一个 L0 文件），比对 `pik.sequence < range_tombstone_iters_[L]->seq()`，若满足则调用 `current->iter.Next()` 跳过。

### 5.3 待验证候选性能机制（Candidate Performance Mechanisms）
- **候选机制 C4**：扫描跨越密集墓碑区域时，墓碑边界频繁出入堆（`PopDeleteRangeStart` 与 `DELETE_RANGE_END` 弹出），引起多次 `minHeap_` 堆重排（堆调整复杂度 $O(\log M)$，其中 $M$ 为参与归并的迭代器及墓碑总数）。
- **候选机制 C5**：Cascading Seek 虽然避免了底层键扫描，但每次底层 `SeekImpl` 会带来 SST 内部的 Index Block 二分与 Data Block 重新解包，其开销可能在特定区间长度下超过单纯的连续 `Next`。
- **待验证判定标准**：需通过探针统计 Reseek 次数与边界推进次数，对照 E8 相交扫描的 Cycles 增量。

---

## 6. Flush 后范围墓碑进入 SST 的编码、读取与迭代器构建路径

### 6.1 下刷编码路径（Source Code Facts）
1. **聚合与转储**：
   - 文件：[db/flush_job.cc:473-478](file:///home/wam/grad/rocksdb-v11.8.0/db/flush_job.cc#L473-L478)
   - 函数：`FlushJob::WriteLevel0Table`
   - 代码事实：从即将下刷的 MemTable 收集所有的 `range_del_iters`，填入 `CompactionRangeDelAggregator` 进行合并。
2. **写入专用 Meta Block**：
   - 文件：[db/builder.cc:336-363](file:///home/wam/grad/rocksdb-v11.8.0/db/builder.cc#L336-L363)
   - 函数：`BuildTable`
   - 代码事实：遍历 `range_del_agg->NewIterator()`，调用 `builder->Add(kv.first.Encode(), kv.second)`。在 `BlockBasedTableBuilder` 内部，检测到 `kTypeRangeDeletion` 时将其分流写入独立的 RangeDel Block（`kRangeDelBlockName`）。
3. **计算补偿尺寸**：
   - 文件：[db/builder.cc:355](file:///home/wam/grad/rocksdb-v11.8.0/db/builder.cc#L355)
   - 代码事实：调用 `versions->ApproximateSize` 估算墓碑覆盖历史层级的数据量，累加至 `meta->compensated_range_deletion_size`。

### 6.2 读取与迭代器构建路径（Source Code Facts）
1. **SST 打开与元数据解析**：
   - 文件：[table/block_based/block_based_table_reader.cc:1620](file:///home/wam/grad/rocksdb-v11.8.0/table/block_based/block_based_table_reader.cc#L1620)
   - 函数：`BlockBasedTable::PrefetchIndexAndFilterBlocks`
   - 代码事实：从磁盘/缓存读取 RangeDel Block，直接构造成只读的 `rep_->fragmented_range_dels`（`std::shared_ptr<FragmentedRangeTombstoneList>`）。
2. **扫描迭代器装配**：
   - 文件：[db/version_set.cc:250-272](file:///home/wam/grad/rocksdb-v11.8.0/db/version_set.cc#L250-L272)
   - 函数：`AddTableIteratorForLevel`
   - 代码事实：L0 文件每个文件生成一个 `TableIterator`，并提取其 `tombstone_iter`（包装为 `TruncatedRangeDelIterator`），分别加入 `MergeIteratorBuilder`。

### 6.3 待验证候选性能机制（Candidate Performance Mechanisms）
- **候选机制 C6**：下刷到 L0 后，墓碑在内存中变成了只读列表，消除了 MemTable 的锁竞争和分段重建开销（解释了 E8 中 `PostFlush` 点查 cycles 相比 `PreFlush` 下降）；但每个 L0 文件作为独立有序流引入 `MergingIterator`，增加了扫描时的堆归并路数与树深度（解释了 E8 中 `PostFlush` 相交扫描 cycles 相比 `PreFlush` 进一步上升）。
- **待验证判定标准**：比对三态下 `iterator_construct_time` 与扫描事件计数的分布差异。

---

## 7. 核心机制审计对照总表（源码事实 vs 待验证候选机制）

| 序号 | 机制主题 | 关键源码位置（文件与精确行号） | 源码实现事实（Confirmed Fact） | 待验证候选性能机制（Candidate Hypothesis） |
| :---: | :--- | :--- | :--- | :--- |
| **M1** | 活跃 MemTable 墓碑跳表隔离 | [db/memtable.h:204](file:///home/wam/grad/rocksdb-v11.8.0/db/memtable.h#L204)<br>[db/memtable.cc:1248](file:///home/wam/grad/rocksdb-v11.8.0/db/memtable.cc#L1248) | 范围删除不进普通跳表，进独立的 `range_del_table_` 无锁跳表。 | 物理隔离避免了点键跳表污染，但导致任何读操作必须分头查询两个跳表/结构。 |
| **M2** | 写端缓存失效风暴 | [db/memtable.cc:1255-1269](file:///home/wam/grad/rocksdb-v11.8.0/db/memtable.cc#L1255-L1269) | 每次写 `DeleteRange`，所有 CPU Core 的 `cached_range_tombstone_` 均被原子替换为空缓存。 | 写入频密时读线程缓存命中率骤降，频繁触发重建是动态负载读性能劣化的候选主因。 |
| **M3** | 读端按需分段与互斥锁 | [db/memtable.cc:934-945](file:///home/wam/grad/rocksdb-v11.8.0/db/memtable.cc#L934-L945)<br>[db/range_tombstone_fragmenter.cc:89](file:///home/wam/grad/rocksdb-v11.8.0/db/range_tombstone_fragmenter.cc#L89) | 缓存未初始化时加 `reader_mutex` 锁，遍历跳表并调用 `FragmentTombstones` 红黑树切分。 | 锁竞争与高开销红黑树分段可能构成 Cold-build 读操作的长尾延迟尖刺。 |
| **M4** | 点查无条件二分判定 | [db/memtable.cc:1584-1600](file:///home/wam/grad/rocksdb-v11.8.0/db/memtable.cc#L1584-L1600)<br>[db/range_tombstone_fragmenter.cc:484](file:///home/wam/grad/rocksdb-v11.8.0/db/range_tombstone_fragmenter.cc#L484) | 即使 BloomFilter 判定键不存在，只要墓碑表非空，点查均执行 `MaxCoveringTombstoneSeqnum` 二分。 | 稳态读（Warm Steady）下点查 CPU cycles 增量的主要来源是该固定二分查找与访存。 |
| **M5** | 点键序号改写与短路 | [table/get_context.cc:406-412](file:///home/wam/grad/rocksdb-v11.8.0/table/get_context.cc#L406-L412) | 若点键序号小于墓碑序号，类型强行改写为 `kTypeRangeDeletion`，直接置 `state_ = kDeleted`。 | 成功遮蔽后能避免底层多余的磁盘/块缓存读取，属于有益短路。 |
| **M6** | 扫描离散墓碑事件堆驱动 | [table/merging_iterator.cc:155-240](file:///home/wam/grad/rocksdb-v11.8.0/table/merging_iterator.cc#L155-L240) | 墓碑区间被拆为 `START` 和 `END` 作为独立项进堆，维护 `active_` 活跃集合。 | 墓碑边界频繁出入堆引发频繁堆重排，是相交扫描吞吐下降的候选原因。 |
| **M7** | 跨层级联 Seek (Cascading Seek) | [table/merging_iterator.cc:1034-1045](file:///home/wam/grad/rocksdb-v11.8.0/table/merging_iterator.cc#L1034-L1045) | 若年轻层级墓碑覆盖年老层级点键，直接将年老层级子迭代器 Seek 到墓碑 `end_key`。 | 在墓碑范围较大时节省顺序 Next，但在微小重叠区间下重复 Seek 可能反增开销。 |
| **M8** | 下刷入 SST 的只读不可变化 | [table/block_based/block_based_table_reader.cc:1620](file:///home/wam/grad/rocksdb-v11.8.0/table/block_based/block_based_table_reader.cc#L1620) | SST 的 `fragmented_range_dels` 在文件打开时构建一次，后续只读复用，无锁无重建。 | 解释了为什么 POSTFLUSH 状态下点查 cycles 会从 PreFlush 的高位回落。 |
| **M9** | 补偿尺寸驱动压实评分 | [db/builder.cc:355](file:///home/wam/grad/rocksdb-v11.8.0/db/builder.cc#L355) | 下刷时计算估算的 `compensated_range_deletion_size` 并累加至 L0 文件属性中。 | 导致含有墓碑的 L0 即使物理尺寸很小也会触发 Compaction，解释写放大升高的潜在诱因。 |

---

## 8. 第一阶段审计结论与对第二阶段探针设计的指导约束

1. **已确认的源码架构事实**：
   - 官方 RocksDB `v11.8.0` 对范围墓碑的处理分为“内存动态分段”与“SST 只读不可变分段”两种截然不同的形态；
   - 点查中的覆盖判断由 `MaxCoveringTombstoneSeqnum` 承担，扫描中的跳过由 `MergingIterator` 堆驱动承担；
   - 内存层存在显式的写端缓存失效与读端加锁分段路径。
2. **对第二阶段最小探针设计的严格约束**：
   - 不得把上述 M2/M3/M4/M6 直接定性为确定瓶颈；
   - 探针指标必须严格正交对应上述候选机制：
     1. `memtable_fragment_build_count/time` $\to$ 量化 M2/M3（冷构建与锁竞争代价）；
     2. `memtable_tombstone_query_count/time` $\to$ 量化 M4（稳态点查二分查找代价）；
     3. `iterator_construct_count/time`（分 Active/Imm/SST） $\to$ 量化 M8（SST 迭代器多路装配代价）；
     4. Scan 事件计数（Reseek 次数、边界推进次数、底层 Next 次数） $\to$ 量化 M6/M7（堆驱动与级联 Seek 代价）。
