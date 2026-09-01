# WB-CleanWrite：无 DeleteRange 写密集负载下 Write Buffer Size 对照总结报告 (notes/wb-cleanwrite-summary.md)

**实验时间**：2026-08-20 23:28  
**实验机器**：`s14.servers.hustpdsl.cn` (80 vCPUs, 78GB RAM)  
**底层介质**：`/dev/nvme0n1p2` (KIOXIA EXCERIA PRO NVMe SSD)  
**RocksDB 版本**：官方 Tag `v11.8.0`（commit `abeebd9630f11bd08c28b7bd43c7bdfc62050654`）  
**实验设计**：
- **目标**：验证 `write_buffer_size=16MiB` 在无 DeleteRange 的写密集负载下是否伴随额外物理代价，验证其是否能作为通用静态配置；
- **配置**：固定 256B Value，`memtable_max_range_deletions=0`，`DeleteRange=0`，线程数 8，Block Cache 128MiB，无压缩；
- **负载构成**：写密集负载（**Put 80%、Get 10%、Scan 10%**），总前台操作 1,310,720 ops（其中 1,048,576 个 Put，产生 256.0 MiB 纯逻辑前台写入量，确保三种容量均跨越自然 Flush 边界）；
- **实验矩阵**：`16MiB / 64MiB / 128MiB` $\times$ 3 次独立重复（共 9 轮完整生命周期实测）。

---

## 一、实测核心指标对比表 (Mean ± Std)

| 写入缓存容量 | 总体吞吐 (kIOPS) | **Put P99 (μs)** | **Get P99 (μs)** | **Scan 单位开销 (μs/key)** | **Scan P99 (μs)** | **Flush 次数** | Flush 写入量 (MB) | Compaction 写 (MB) | **CWA** | **PWA** | **前台写停顿 (Write Stall)** | 全库 SHA-256 校验 |
| :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **16 MiB 组** | $281.7 \pm 15.2$ | $32.39 \pm 3.55$ | $27.25 \pm 2.57$ | $\mathbf{1.10 \pm 0.03}$ | $208.9 \pm 22.8$ | $\mathbf{19.0 \pm 0.0}$ | $\mathbf{259.3 \pm 1.7}$ | $\mathbf{574.0 \pm 111.0}$ | $\mathbf{2.24 \pm 0.43}$ | $\mathbf{3.26 \pm 0.43}$ | $\mathbf{69,339.7 \pm 53519.5\text{ μs}}$ <br>(**69.3 ms**) | `2f6422d3...` (PASS) |
| **64 MiB 组** | $304.4 \pm 8.8$ | $29.45 \pm 1.51$ | $22.58 \pm 0.39$ | $\mathbf{0.96 \pm 0.01}$ | $171.1 \pm 3.3$ | $\mathbf{4.0 \pm 0.0}$ | $\mathbf{206.0 \pm 0.0}$ | $\mathbf{261.0 \pm 0.0}$ | $\mathbf{1.02 \pm 0.00}$ | $\mathbf{1.82 \pm 0.00}$ | **0.0 μs** | `2f6422d3...` (PASS) |
| **128 MiB 组**| $\mathbf{325.9 \pm 6.5}$ | $\mathbf{29.13 \pm 1.33}$ | $\mathbf{19.52 \pm 0.39}$ | $\mathbf{0.80 \pm 0.01}$ | $\mathbf{134.1 \pm 4.4}$ | $\mathbf{2.0 \pm 0.0}$ | $\mathbf{187.0 \pm 0.0}$ | $\mathbf{0.0 \pm 0.0}$ | $\mathbf{0.00 \pm 0.00}$ | $\mathbf{0.73 \pm 0.00}$ | **0.0 μs** | `2f6422d3...` (PASS) |

---

## 二、核心判定与因果分析

1. **Flush 频次与 I/O 放大**：
   - 面对 256MB 的写密集数据流，`16MiB` 组触发了 **19 次自然 Flush**，而 `64MiB` 组仅触发 4 次，`128MiB` 组仅触发 2 次。
2. **写放大（PWA）恶化**：
   - `16MiB` 组的 Compaction 写入量达到 **574.0 MB**，总物理写放大（PWA）达 **3.26x**；
   - 相比之下，`128MiB` 组 PWA 仅为 **0.73x**（`16MiB` 的总物理写放大增加了 **4.47 倍**）。
3. **前台写停顿（Write Stall）涌现**：
   - `16MiB` 组在频繁下刷与 L0 堆叠压力下，产生了平均 **69.3 ms 的前台写停顿（Write Stall）**；而 `64MiB` 与 `128MiB` 组全程写停顿为 **0.0 μs**。
4. **读性能微幅退化**：
   - 由于 16MiB 生成了更多零碎 SST 文件，Scan 单位有效 Key 开销从 128MB 的 $0.80\text{ μs/key}$ 上升至 $1.10\text{ μs/key}$（+37.5%）。

---

## 三、结论

**`write_buffer_size=16MiB` 不能作为一种通用的静态配置解**。在无 DeleteRange 的写密集场景下，激进的小写缓冲配置会直接导致 **Flush 频次激增 9.5 倍**、**物理写放大增加 4.5 倍** 并引发 **毫秒级 Write Stall 写停顿**。这确立了：写缓冲大小的静态设置本质上仍受制于 LSM-tree 原生写放大与写停顿权衡，无法替代对范围墓碑压力的感知与自适应调控。
