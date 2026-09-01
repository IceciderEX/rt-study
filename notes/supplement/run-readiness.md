# 补充实验执行准入与冻结方案报告 (notes/supplement/run-readiness.md)

**生成时间**：2026-08-20  
**实验机器节点**：`s14.servers.hustpdsl.cn` (80 vCPUs, 78GB RAM)  
**底层存储介质**：`/dev/nvme0n1p2` (KIOXIA-EXCERIA PRO 1.8TB NVMe SSD, ext4)  
**引擎基准版本**：官方纯净 RocksDB `v11.8.0` (commit `abeebd9630f11bd08c28b7bd43c7bdfc62050654`, Release `-O2`)  
**当前状态**：**已完成全部配置与驱动编译，处于严格冻结状态，等待您的确认指令后再行启动正式运行**。

---

## 一、证据校正实验（S1 ~ S3）设计架构与目标

为彻底消除原 P1 ~ P5 预实验中的测量干扰与不可比性因素，本补充实验套件严格执行以下三组校正实验：

| 实验组别 | 实验名称 | 核心校正目标与隔离设计 | 运行轮次 |
| :--- | :--- | :--- | :---: |
| **S1** | **静态墓碑读开销隔离实验** | 彻底分离“DeleteRange 执行时的写/刷盘开销”与“静态墓碑对读路径的纯粹影响”：<br>1. 全局数据量 1,000,000 Keys × 1KiB = **1.0 GiB**（远超 128MB BlockCache，施加真实 NVMe 缺页压力）；<br>2. 预先固定全局删除并集 $U$（覆盖 400,000 Keys，占 40%）；<br>3. `S1-clean` 与 `S1-tombstone` 最终逻辑存活键数（600,000 Keys）与键值内容**逐字节 100% 相同**；<br>4. 测量阶段为纯只读（Point-Heavy: Get 95%, Scan 5% 与 Scan-Heavy: Get 10%, Scan 90%），输入完全相同的请求流与种子。 | 2状态 × 2负载 × 3重复 = **12 轮** |
| **S2** | **公平的范围墓碑碎片化实验** | 修正原 P2 中“短范围组与长范围组删除覆盖率不同”的缺陷：<br>1. 统一 100 万键空间与 128MB BlockCache；<br>2. 严格固定删除并集 $U$ 覆盖率恒为 **40%（400,000 Keys）**，存活键恒为 **600,000 Keys**；<br>3. 仅改变 $U$ 被均匀切分的离散段数：<br> - `S2-seg-20`（20 段，每段 20,000 Keys）<br> - `S2-seg-200`（200 段，每段 2,000 Keys）<br> - `S2-seg-2000`（2,000 段，每段 200 Keys）；<br>4. 测量阶段纯只读，记录实际墓碑保留数、Get P99 与 Scan 单位有效键遍历开销。 | 3粒度 × 2负载 × 3重复 = **18 轮** |
| **S3** | **受控回收前台时间序列实验** | 修正原 P5 缺乏时间序列的缺陷，检验物理回收的瞬时前台干扰与中长期收益：<br>1. 以 `S2-seg-2000` 静态墓碑库为起点；<br>2. 前台运行持续混合负载（Get 75%, Scan 20%, Put 5%）；<br>3. 后台控制线程在 $t=20\text{s}$ 触发一次局部 `CompactRange()`（Hot-Reclaim: 0~200k 热区，Cold-Reclaim: 800k~1M 冷区，Control: 不触发）；<br>4. **1 秒级时间序列采样**：记录吞吐、各操作 P99、Compaction 读写量增量、WriteStall 增量、L0 文件数与 BlockCache 命中率。 | 3工况 × 3重复 = **9 轮** |

**补充实验总运行轮次**：$12 + 18 + 9 = \mathbf{39\text{ 轮}}$。

---

## 二、资源预算与单次/全套耗时估算

### 2.1 数据规模与磁盘预算
- **单库逻辑规模**：1,000,000 Keys × 1 KiB = **1.0 GiB**。
- **单库物理峰值占用**：~1.2 GiB (含 SST 与 WAL)。
- **磁盘占用与清理策略**：
  - 每次运行使用独立隔离子目录：`/home/wam/grad/s14-range-delete-study/run-db/supplement/db_${run_id}`；
  - 运行结束后自动对账、归档原始日志并**立即释放当前 DB 目录**；
  - 任意时刻活动磁盘占用峰值 $\le \mathbf{2.5\text{ GiB}}$（当前可用 NVMe 空间为 **585 GiB**，余量充裕率 > 99%）。

### 2.2 耗时预估
- **单轮运行**：
  - 数据预置（Preload 1GB + 2次 Flush） $\approx 6 \sim 10\text{ s}$；
  - 测量阶段（200k Ops 或 60s 压测） $\approx 8 \sim 60\text{ s}$；
  - 离线全量一致性对账（10k 抽样 + 全库 600k 键遍历） $\approx 3\text{ s}$；
  - 单轮总计：$\approx 20 \sim 75\text{ s}$。
- **全套 39 轮总耗时**：
  - S1 (12 轮): $\approx 5\text{ 分钟}$
  - S2 (18 轮): $\approx 8\text{ 分钟}$
  - S3 (9 轮, 每轮固定 60s): $\approx 12\text{ 分钟}$
  - **全套总预估耗时**：$\approx \mathbf{25 \sim 30\text{ 分钟}}$。

---

## 三、冻结配置与参数全景明细

所有配置文件均已生成并存放于 `/home/wam/grad/s14-range-delete-study/configs/supplement/`：

```text
configs/supplement/
├── s1_clean_point_heavy.ini
├── s1_clean_scan_heavy.ini
├── s1_tombstone_point_heavy.ini
├── s1_tombstone_scan_heavy.ini
├── s2_seg20_point_heavy.ini
├── s2_seg20_scan_heavy.ini
├── s2_seg200_point_heavy.ini
├── s2_seg200_scan_heavy.ini
├── s2_seg2000_point_heavy.ini
├── s2_seg2000_scan_heavy.ini
├── s3_control.ini
├── s3_hot_reclaim.ini
└── s3_cold_reclaim.ini
```

### 控制变量与固定参数列表
- `total_keys = 1000000`
- `value_size = 1024` (1 KiB)
- `block_cache_size = 134217728` (128 MiB)
- `write_buffer_size = 67108864` (64 MiB)
- `max_write_buffer_number = 4`
- `max_background_jobs = 8`
- `level0_file_num_compaction_trigger = 4`
- `scan_len = 100`
- `random_seed`: S1 固定 10001, S2 固定 20001, S3 固定 30001

---

## 四、度量项与字段全景映射

| 度量维度 | 统计字段 | 采集途径 | 核心分析作用 |
| :--- | :--- | :--- | :--- |
| **Get 细分性能** | `get_control_p50/p95/p99` | 驱动高精计时 | 远离墓碑的纯净存活键基准 |
| | `get_aff_p50/p95/p99` | 驱动高精计时 | 位于墓碑边界/缝隙的受影响存活键长尾 |
| | `get_del_p50/p95/p99` | 驱动高精计时 | NotFound 点查路径开销 |
| **RangeScan 性能** | `scan_avg_keys_returned` | 迭代器返回计数 | 记录实际有效键数，消除有效数据量偏差 |
| | `scan_p50/p95/p99` | 驱动高精计时 | 总体扫描耗时分布 |
| | `scan_us_per_key` | $\frac{\text{Mean Latency}}{\text{Avg Returned Keys}}$ | **核心指标**：单位有效键遍历开销 |
| **缓存与 I/O 效率** | `block_cache_hits/misses` | `Tickers::BLOCK_CACHE_HIT/MISS` | 检验 1GB 工作集下的物理缓存命中率 |
| | `compaction_read/write_mb` | `Tickers::COMPACT_READ/WRITE_BYTES` | 确保 S1/S2 测量阶段无后台 Compaction 干扰 |
| | `stall_micros` | `Tickers::STALL_MICROS` | 前台阻塞微秒数 |
| **S3 时间序列** | 1秒采样时间序列 | 独立后台采样线程 | 输出每秒吞吐、P99、Compaction 增量与写停顿 |
| **终态一致性校验** | `sample_verified_ok/notfound`<br>`full_scan_ok` | 10k 随机抽样<br>全库 600k 键遍历 | 确保 Clean 与 Tombstone 终态完全一致 |

---

## 五、严格停止与熔断条件

执行过程中出现以下任一情况，调度脚本将立即终止并报错：
1. **终态状态不一致**：`S1-clean` 与 `S1-tombstone`（或 S2 各组）全库扫描有效键数不等于 600,000 或校验未通过；
2. **读请求流不一致**：对照组之间目标 Key 或 RangeScan 期望返回结果存在偏差；
3. **测量阶段异常 Compaction**：S1 或 S2 测量期间发生非预期的 Compaction 读写；
4. **路径偏离**：数据库脱离 `/dev/nvme0n1p2` NVMe 文件系统；
5. **空间余量红线**：磁盘可用空间低于 50 GB；
6. **参数污染**：私自修改固定配置或剔除测试数据。

---

## 六、执行准入声明

- [x] 现有 P1 ~ P5 结果逐次数据审计完成并生成 [notes/supplement/existing-result-audit.md](file:///home/wam/grad/s14-range-delete-study/notes/supplement/existing-result-audit.md)
- [x] S1 ~ S3 补充驱动代码已编写并通过构建（`bin/supp_workload_driver`）
- [x] S1 ~ S3 全部 13 个配置文件已生成并冻结（`configs/supplement/`）
- [x] S1 ~ S3 运行脚本与主调度脚本已生成（`scripts/supplement/`）
- [x] **所有补充实验代码与配置均处于冻结就绪状态，等待您的确认指令后再行启动正式执行**。
