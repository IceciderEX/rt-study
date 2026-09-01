# RangeDelete 开题验证实验负载构造与执行逻辑设计规范 (notes/workload-generation-and-execution-logic.md)

**文档属性**：项目组审阅规范文档（Workload Generation & Execution Logic Specification）  
**审计修订**：2026-08-21（已根据项目组核验意见完成全面口径收紧与语义修正）

---

## 目录
1. [二进制 Trace 底层存储协议](#1-二进制-trace-底层存储协议)
2. [静态负载体系生成逻辑 (P1 ~ P5)](#2-静态负载体系生成逻辑-p1--p5)
3. [动态多阶段负载体系生成逻辑 (P6 / E3 / B2)](#3-动态多阶段负载体系生成逻辑-p6--e3--b2)
4. [自然 Flush 边界与反事实负载逻辑 (B1 / B1-CF)](#4-自然-flush-边界与反事实负载逻辑-b1--b1-cf)
5. [写密集无删除对照负载逻辑 (WB-CleanWrite)](#5-写密集无删除对照负载逻辑-wb-cleanwrite)
6. [24GiB 大规模负载生成逻辑 (LS24 & LS24-Clean)](#6-24gib-大规模负载生成逻辑-ls24--ls24-clean)
7. [24GiB 40% 密度保持负载与同轨迹 Clean 逻辑 (LS24-DensityPreserved)](#7-24gib-40-密度保持负载与同轨迹-clean-逻辑-ls24-densitypreserved)
8. [C++ 驱动端重放机制、LIMIT 扫描语义与状态校验实现](#8-c-驱动端重放机制limit-扫描语义与状态校验实现)

---

## 1. 二进制 Trace 底层存储协议

为避免前台压测过程中读取文本文件的额外 I/O 开销，所有负载均预先生成为定长二进制文件（17 字节/操作）。

### 1.1 数据结构定义
```text
+-------------------+--------------------+--------------------+
| op_type (1 byte)  |   key1 (8 bytes)   |   key2 (8 bytes)   |
|   uint8_t (<B)    |    uint64_t (<Q)   |    uint64_t (<Q)   |
+-------------------+--------------------+--------------------+
```

- **`op_type` 操作码映射与语义**：
  - `0`：**Get 点查**（`key1` = 目标 Key ID，`key2` = 0；统计标记为“按 Trace 最终删除范围标记的点查请求”）；
  - `1`：**Scan 范围扫描**（`key1` = 起始 Key ID，`key2` = **返回可见有效键上限 LIMIT**，默认 100）；
  - `2`：**Put 写入/更新**（`key1` = 目标 Key ID，`key2` = 0 或指定 Value 大小）；
  - `3`：**DeleteRange 范围删除**（`key1` = 起始 Key ID，`key2` = 结束 Key ID，左闭右开区间 $[k_1, k_2)$）；
  - `4`：**No-op 空操作**（**保持 Get/Scan/Put 请求不变、去除 DeleteRange 调用的功能性无范围删除对照专用**：保留原调度时间点与参数，但不调用 RocksDB 写入接口）。

---

## 2. 静态负载体系生成逻辑 (P1 ~ P5)

### 2.1 P1 比例扫描基线 (Range Delete Ratio)
- **目的**：测试不同 DeleteRange 操作比例（0% ~ 100%）对前台读写延迟的基准影响；
- **总操作数**：300,000 ops，Key 空间：500,000 keys；
- **生成策略**：按比例抽取 DeleteRange（跨度 100）、Get、LIMIT 100 Scan 与 Put 操作。

### 2.2 P2 墓碑规模基线 (Tombstone Count)
- **参数控制**：固定读写比例，分别注入 100、500、2,000、10,000 个墓碑，观察 MemTable 跳表遍历退化。

---

## 3. 动态多阶段负载体系生成逻辑 (P6 / E3 / B2)

### 3.1 三阶段时变协议定义
- **Phase A (0% ~ 33% 进度，0 ~ 100k ops)**：**读敏感阶段 (Read Sensitive)**
  - 构成：Get 60%, LIMIT 100 Scan 25%, Put 5%, DeleteRange 10%
  - 特征：前台读为主，开始缓慢注入初始墓碑；
- **Phase B (34% ~ 66% 进度，100k ~ 200k ops)**：**写突发与删除风暴 (Write & Tombstone Burst)**
  - 构成：Put 60%, Get 25%, LIMIT 100 Scan 10%, DeleteRange 5%
  - 特征：大量写入与删除并发，测试 MemTable 压力极限；
- **Phase C (67% ~ 100% 进度，200k ~ 300k ops)**：**读恢复阶段 (Read Recovery)**
  - 构成：Get 65%, LIMIT 100 Scan 25%, Put 5%, DeleteRange 5%
  - 特征：写突发停止，观察系统在无高频写入下读性能表现。

---

## 4. 自然 Flush 边界与反事实负载逻辑 (B1 / B1-CF)

### 4.1 B1 写入量临界点探测负载
- **设计逻辑**：固定初始 2,000 个 DeleteRanges（span 100，覆盖 200k 键），改变 Put 写入量为写缓冲容量（64MB）的 `0.25x (16MB)`、`0.50x (32MB)`、`0.75x (48MB)`、`1.25x (80MB)`，探测自然 Flush 的临界点。

### 4.2 B1-CF 反事实实验负载
- **设计逻辑**：**100% 固定单一请求流与终态**（131,072 个 Put 写入，逻辑写入量恰为 32.0MB），仅改变配置 `write_buffer_size = 16MiB / 64MiB / 128MiB`。
- **作用**：提供自然 Flush 发生与读性能恢复高度相关的强反事实证据。

---

## 5. 写密集无删除对照负载逻辑 (WB-CleanWrite)

- **构成**：Put 80%, Get 10%, LIMIT 100 Scan 10%, DeleteRange 0%；
- **逻辑写入量**：1,048,576 个 Puts $\times$ 256B = **256.0 MiB**，确保 16MB/64MB/128MB 三档容量均跨越自然 Flush 边界；
- **目的**：验证小写缓冲（16MiB）在写密集负载下会引发频繁 Flush、写放大激增与毫秒级写停顿，确认其不能作为无代价的通用静态解。

---

## 6. 24GiB 大规模负载生成逻辑 (LS24 & LS24-Clean)

- **LS24 稀释版**：100.66M 键（24GiB），20,000 个墓碑（span 100，覆盖率 1.97%）；
- **LS24-Clean 稀释版基线**：保留原 LS24 动态轨迹中的所有读写请求参数，仅将 DeleteRange 替换为 No-op，测定稀释版原轨迹的纯读基线。

---

## 7. 24GiB 40% 密度保持负载与同轨迹 Clean 逻辑 (LS24-DensityPreserved)

### 7.1 数学模型与实测审计
- 目标覆盖率：40.0% $\pm$ 1.0%；
- 单墓碑跨度：$\text{Span} = 2,013$ 键，桶宽 $W = 5,033$ 键；
- **实测审计结果**：
  - 生成墓碑数：**19,856 条**（名义 20,000 条）；
  - 唯一互斥区间：**19,856 个**（**区间重叠率 0.00%**）；
  - 实际物理失效键数：**39,970,128 键**（**实际并集覆盖率 39.71%**，符合验收标准）；
  - 点查删除命中率：**39.76%**，扫描重叠率：**41.89%**。

### 7.2 同轨迹 DP-Clean 纯读基准
- 将 `ls24_density_phases.bin` 中的 19,856 个 DeleteRange 替换为 No-op，提供完全同构的纯读基准对照。

---

## 8. C++ 驱动端重放机制、LIMIT 扫描语义与状态校验实现

### 8.1 多线程原子并发重放与 LIMIT 扫描源码
```cpp
// 多线程通过原子游标领取操作（统计确定性并发重放）
std::atomic<uint64_t> op_cursor{0};

void WorkerThread() {
    while (true) {
        uint64_t idx = op_cursor.fetch_add(1, std::memory_order_relaxed);
        if (idx >= total_trace_ops) break;

        const auto& op = trace_ops_[idx];
        auto t_start = std::chrono::high_resolution_clock::now();

        if (op.op_type == 0) { // Get
            std::string key = TVReferenceModel::FormatKey(op.key1);
            std::string val;
            rocksdb::Status s = db_->Get(read_opts, key, &val);
            auto t_end = std::chrono::high_resolution_clock::now();
            uint64_t lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count();
            stats_.RecordGet(static_cast<int>(ref_model_.ClassifyGet(op.key1)), lat_ns);
        } else if (op.op_type == 1) { // LIMIT 扫描 (Limit-K Valid Keys Scan)
            std::string start_key = TVReferenceModel::FormatKey(op.key1);
            std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(read_opts));
            it->Seek(start_key);
            uint64_t keys_found = 0;
            // 语义：连续遍历底层数据，直到找到 op.key2 (100) 个可见有效键或迭代结束
            while (it->Valid() && keys_found < op.key2) {
                keys_found++;
                it->Next();
            }
            auto t_end = std::chrono::high_resolution_clock::now();
            uint64_t lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count();
            stats_.RecordScan(lat_ns, op.key2, keys_found);
        } else if (op.op_type == 2) { // Put
            std::string key = TVReferenceModel::FormatKey(op.key1);
            std::string val = TVReferenceModel::GenerateValue(op.key1, 2, config_.value_size);
            rocksdb::Status s = db_->Put(write_opts, key, val);
            auto t_end = std::chrono::high_resolution_clock::now();
            uint64_t lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count();
            stats_.RecordPut(lat_ns);
        } else if (op.op_type == 3) { // DeleteRange
            std::string start_key = TVReferenceModel::FormatKey(op.key1);
            std::string end_key = TVReferenceModel::FormatKey(op.key2);
            rocksdb::Status s = db_->DeleteRange(write_opts, db_->DefaultColumnFamily(), start_key, end_key);
            auto t_end = std::chrono::high_resolution_clock::now();
            uint64_t lat_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count();
            stats_.RecordDeleteRange(lat_ns);
        } else if (op.op_type == 4) { // No-op (功能性 Clean 基准专用)
            // 不调用 RocksDB 写入接口，作为调度占位
        }
    }
}
```

### 8.2 全库可见键值遍历 SHA-256 摘要一致性验证
```cpp
bool TVReferenceModel::FullScanAndComputeSha256(
    rocksdb::DB* db,
    uint64_t& out_live_keys,
    uint64_t& out_payload_bytes,
    std::string& out_sha256_hex) const 
{
    SHA256_CTX sha_ctx;
    SHA256_Init(&sha_ctx);

    rocksdb::ReadOptions read_opts;
    read_opts.fill_cache = false; // 绕过 Block Cache
    read_opts.total_order_seek = true;

    std::unique_ptr<rocksdb::Iterator> it(db->NewIterator(read_opts));
    out_live_keys = 0;
    out_payload_bytes = 0;

    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        rocksdb::Slice k_slice = it->key();
        rocksdb::Slice v_slice = it->value();

        out_live_keys++;
        out_payload_bytes += (k_slice.size() + v_slice.size());

        // 零拷贝流式更新哈希摘要
        SHA256_Update(&sha_ctx, k_slice.data(), k_slice.size());
        SHA256_Update(&sha_ctx, v_slice.data(), v_slice.size());
    }

    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_Final(hash, &sha_ctx);

    char hex_buf[65];
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(hex_buf + (i * 2), "%02x", hash[i]);
    }
    hex_buf[64] = '\0';
    out_sha256_hex = std::string(hex_buf);

    return it->status().ok();
}
```

---

## 9. 审阅结论与学术边界总结

1. **学术定性与工况定位**：
   - **小规模 B2 动态 Trace**：实际并集覆盖率达 **98.36%**，定位于“近乎全 Key 空间被范围删除覆盖的高压力极端工况（Extreme Stress Baseline）”，有力证明了极端墓碑积压时的性能崩塌风险；
   - **大规模 LS24-DensityPreserved**：实际注入 19,856 条互斥范围删除，实际并集覆盖率为 **39.71%**（区间重叠率 0.00%），作为主叙事中标准的 40% 覆盖率验证证据；
2. **时序与调度边界**：
   - 驱动采用**固定 Trace 内容的并发重放**，实际操作的线性化执行顺序受 OS 与 CPU 线程调度影响，阶段切换平滑过渡；
3. **扫描语义与点查定义**：
   - Scan 算子严格定义为 **LIMIT 100 有效可见键扫描**；
   - 原 `Get(Del)` 严格定义为 **“按 Trace 最终删除范围标记的点查请求”**，不再使用“实时命中已删除键”；
4. **对照基准定义**：
   - Clean 组定义为 **“保持 Get/Scan/Put 请求不变、去除 DeleteRange 调用的功能性无范围删除对照”**；
5. **覆盖指标定义**：
   - 一律采用 **“范围删除并集覆盖键数”** 代替“实际物理失效键数”。
