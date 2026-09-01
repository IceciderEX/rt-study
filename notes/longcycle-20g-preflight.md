# FormalV2-LongCycle-20GiB：环境预检与空间预算报告（Preflight Report）

## 1. 宿主机与硬件环境审计（P0 环境预检）

- **服务器主机名**：`s14.servers.hustpdsl.cn`；
- **CPU 架构**：Dual Intel(R) Xeon(R) Gold 5218R @ 2.10GHz（双路 40 物理核 / 80 逻辑核）；
- **系统内核**：Ubuntu 22.04 LTS（Linux `6.8.0-136-generic`）；
- **内存容量**：78 GiB 物理内存（空闲可用内存 64 GiB）；
- **存储设备与挂载点**：
  - 设备：`/dev/nvme0n1p2`（NVMe 高性能 SSD，挂载于 `/`，文件系统 ext4）；
  - 总容量：`1.8 TiB`；
  - 已用空间：`1.1 TiB`（`65%` 使用率）；
  - **当前可用空间：`614 GiB`**（远高于准入要求的 $\ge 250\text{ GiB}$）；
- **当前设备 I/O 状态**：
  - NVMe `nvme0n1` 当前利用率（%util）：**`0.00%`**；
  - 读写带宽：**`0.00 MB/s`**（无并发重载 I/O 干扰）。

---

## 2. 软件构建与驱动版本固化

- **RocksDB 版本**：Tag `v11.8.0`（Commit `abeebd9630f11bd08c28b7bd43c7bdfc62050654`）；
- **编译器版本**：`g++ (Ubuntu 11.4.0-1ubuntu1~22.04.3) 11.4.0`；
- **编译参数**：`-O2 -g -fno-omit-frame-pointer -std=c++20 -pthread -fno-rtti`；
- **驱动二进制**：`bin/longcycle_driver`。

---

## 3. 存储空间预算与安全余量评估

### 3.1 单轮 20GiB 实验物理占用预算
- **逻辑数据规模**：$20.48\text{M}$ 可见 Keys $\times 1\text{KiB} = 20.00\text{ GiB}$；
- **累计 Put 写入量**：$30.6\text{M} \times 1\text{KiB} = 30.6\text{ GiB}$；
- **SST 物理占用估算**：$22 \sim 32\text{ GiB}$（无压缩，包含 SST 块索引、BloomFilter 与多版本残留）；
- **WAL 与临时空间**：约 $2 \sim 4\text{ GiB}$；
- **单轮运行峰值占用**：$\le 36\text{ GiB}$。

### 3.2 核心 9 轮实验最坏空间预算与清理策略
- **保留策略**：
  - 保留 1 轮代表性 `LC20-T0` 完整数据库实体（约 $28\text{ GiB}$）；
  - 保留 1 轮代表性 `LC20-T512` 完整数据库实体（约 $26\text{ GiB}$）；
  - 其余轮次在完成全库 SHA-256 审计、SST 元数据快照、时间序列与汇总 CSV 归档后清理 DB 实体；
- **峰值并发占用**：单次仅运行 1 轮实验，运行期峰值占用仅增加 $\approx 35\text{ GiB}$；
- **磁盘安全余量计算**：
  - 当前可用空间：`614 GiB`；
  - 实验完成后预计剩余空间：$\ge 550\text{ GiB}$；
  - 磁盘使用率预计保持在：$\le 67\%$（完全满足“保留至少 30% 磁盘可用空间 / 使用率低于 70%”的准入红线）。

---

## 4. 2GiB 缩放 Pilot 实验目标与验证清单

正式启动 20GiB 矩阵前，必须先执行 **2GiB 缩放 Pilot**（10% 规模）：
1. **`LC20-PILOT-T0`**（`threshold=0`）；
2. **`LC20-PILOT-T512`**（`threshold=512`）。

### Pilot 核心验证项目：
- [ ] 四阶段（Phase A/B/C/D）负载比例切换与硬 Barrier 协同无异常；
- [ ] 5,000 条 DeleteRange 注入、20% 并集覆盖率与 20% 重叠率几何符合 Manifest；
- [ ] 每 5 秒时间序列采集平稳，前台 P50/P95/P99 与内部 LSM Level 文件数正常输出；
- [ ] LSM 层级自然形成 L0、L1、L2；
- [ ] 10 分钟 Cooldown 后全库 2.56M Key 遍历与 SHA-256 对账 100% 吻合；
- [ ] 实测单轮耗时与物理膨胀系数，为 20GiB 正式运行提供精确基准。
