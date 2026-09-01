# LS24：24GiB 级状态与公平性审计报告 (notes/ls24-state-audit.md)

**审计时间**：2026-08-20 22:40  
**实验机器**：`s14.servers.hustpdsl.cn` (80 vCPUs, 78GB RAM)  
**底层介质**：`/dev/nvme0n1p2` (KIOXIA EXCERIA PRO NVMe SSD)  
**RocksDB 版本**：官方 Tag `v11.8.0`（commit `abeebd9630f11bd08c28b7bd43c7bdfc62050654`）

---

## 一、24GiB 数据规模与存储空间三层口径对账

| 数据维度 | 计量口径 | 实测数值 | 说明 |
| :--- | :--- | :---: | :--- |
| **初始 Key 数量** | 逻辑 Key 总数 | **100,663,296 (1.006 亿)** | 流式确定性预载 |
| **单 Key Value 大小** | 逻辑 Value 字节 | **256 B** | 固定定长 Value |
| **有效逻辑 Payload** | 纯数据内容（不含元数据） | **24.0 GiB (25,769.8 MB)** | $100,663,296 \times 256\text{ B}$ |
| **终态存活 Key 数量** | 经 300k ops 动态删除/写入后 | **98,685,155 (98.685M)** | 100% 全库遍历对账 |
| **实际删除 Key 数量** | 被 20,000 个 RangeDelete 覆盖 | **1,978,141 (1.978M)** | 实际覆盖率 **1.965%** |
| **终态逻辑数据体积** | 存活 Key+Value 逻辑大小 | **25,598.87 MB (25.0 GiB)** | 校验遍历计算 |
| **RocksDB SST 物理体积** | RocksDB 内部 SST 统计 | **26,303 ~ 26,312 MB (25.7 GiB)** | `total_sst_mb` (无压缩) |
| **文件系统物理占用** | 文件系统 `du -sh` 瞬时占用 | **~26.5 GB** | 包含 SST、MANIFEST、WAL |

---

## 二、全量运行 SHA-256 逐比特状态对账表

对 LS24 的 1 轮 Pilot 与 9 轮正式生命周期运行进行全库遍历与 SHA-256 逐比特比对：

| 实验轮次 ID | 配置组别 | 阈值 $T$ | 存活 Key 数量 | 逻辑 Payload (MB) | 全库完整 64 位 SHA-256 校验和 | 采样校验 | 全库校验 | 状态判定 |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **ls24_pilot_t0256** | Pilot | 256 | 98,685,155 | 25,598.87 | `973eefce9c581480abf9a8b557d0fbfbd2af88382268b4265b755bb13d3c3b8a` | PASS | PASS | **PASS** |
| **ls24_t0000_r01** | Formal | 0 | 98,685,155 | 25,598.87 | `973eefce9c581480abf9a8b557d0fbfbd2af88382268b4265b755bb13d3c3b8a` | PASS | PASS | **PASS** |
| **ls24_t0000_r02** | Formal | 0 | 98,685,155 | 25,598.87 | `973eefce9c581480abf9a8b557d0fbfbd2af88382268b4265b755bb13d3c3b8a` | PASS | PASS | **PASS** |
| **ls24_t0000_r03** | Formal | 0 | 98,685,155 | 25,598.87 | `973eefce9c581480abf9a8b557d0fbfbd2af88382268b4265b755bb13d3c3b8a` | PASS | PASS | **PASS** |
| **ls24_t0256_r01** | Formal | 256 | 98,685,155 | 25,598.87 | `973eefce9c581480abf9a8b557d0fbfbd2af88382268b4265b755bb13d3c3b8a` | PASS | PASS | **PASS** |
| **ls24_t0256_r02** | Formal | 256 | 98,685,155 | 25,598.87 | `973eefce9c581480abf9a8b557d0fbfbd2af88382268b4265b755bb13d3c3b8a` | PASS | PASS | **PASS** |
| **ls24_t0256_r03** | Formal | 256 | 98,685,155 | 25,598.87 | `973eefce9c581480abf9a8b557d0fbfbd2af88382268b4265b755bb13d3c3b8a` | PASS | PASS | **PASS** |
| **ls24_t2048_r01** | Formal | 2048 | 98,685,155 | 25,598.87 | `973eefce9c581480abf9a8b557d0fbfbd2af88382268b4265b755bb13d3c3b8a` | PASS | PASS | **PASS** |
| **ls24_t2048_r02** | Formal | 2048 | 98,685,155 | 25,598.87 | `973eefce9c581480abf9a8b557d0fbfbd2af88382268b4265b755bb13d3c3b8a` | PASS | PASS | **PASS** |
| **ls24_t2048_r03** | Formal | 2048 | 98,685,155 | 25,598.87 | `973eefce9c581480abf9a8b557d0fbfbd2af88382268b4265b755bb13d3c3b8a` | PASS | PASS | **PASS** |

---

## 三、审计结论

1. **同组内一致性**：`T=0`、`T=256` 与 `T=2048` 的各 3 次重复在存活 Key 数（98,685,155）、有效载荷大小（25,598.87 MB）及 64 位 SHA-256 校验和上**100% 达成严格确定性逐比特一致**。
2. **跨配置一致性**：由于 LS24 所有配置采用相同确定性操作序列（`ls24_dynamic_phases.bin`）且初始预载完全相同，跨 $T=0, 256, 2048$ 的全部 10 轮运行终态 SHA-256 亦完全一致，证实了不同静态阈值未引入任何数据丢失、状态错乱或语义偏差。
