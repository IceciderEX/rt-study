# Formal V2 可重复性审计清单与环境快照清单

版本：v2.0-Formal（2026-08-21）  
适用目标：作为学术论文与开题报告的完整可复现性审计基准。

---

## 1. 软件快照与二进制哈希

- **RocksDB 版本**：Official Release Tag `v11.8.0`
- **RocksDB 完整 Git Commit**：`abeebd9630f11bd08c28b7bd43c7bdfc62050654`（Worktree Clean, 0 局部修改）
- **驱动构建参数**：`g++ -O2 -g -std=c++20 -fno-rtti -Wall -Wextra -pthread`
- **编译器版本**：`g++ (Ubuntu 11.4.0-1ubuntu1~22.04.3) 11.4.0`
- **`formal_driver` 二进制 SHA-256**：  
  `7636844c5767f00444fb3fd787c5498fe58446a7c3a6ff3847d6e6d7c4b55af6`
- **`generate_trace_v2.py` 生成器 SHA-256**：  
  `72e59b5a6575f2201102ba90c7906c31001d688e2cae2ae49d2df5a012c71991`

---

## 2. 硬件与物理环境快照

- **测试节点**：`s14.servers.hustpdsl.cn`
- **操作系统**：Ubuntu 22.04.5 LTS (Linux Kernel `6.8.0-136-generic`)
- **处理器架构**：Dual Intel(R) Xeon(R) Gold 5218R CPU @ 2.10GHz
- **CPU 核心拓扑**：48 Physical Cores / 96 Logical Threads, 2 NUMA Nodes
- **物理内存**：78 GB DRAM
- **存储介质**：KIOXIA EXCERIA PRO NVMe SSD
- **存储挂载点**：`/dev/nvme0n1p2 on / type ext4 (rw,relatime)`
- **可用存储空间**：634 GB Available

---

## 3. 标准 Trace 数据集 Manifest 摘要清单

| Workload ID | 数据量 (Keys) | 记录总数 (Ops) | 删除覆盖率 | 重叠率 | 跨分片冲突 | Manifest SHA-256 |
| :--- | :---: | :---: | :---: | :---: | :---: | :--- |
| `small_dynamic_500k_limit` | 500,000 | 300,000 | 40.00% | 0.00% | 0 | `3fa4bbc2a9a059ff10a2f36b21e5bedb8127d58bee2ab02fb387ca27cb08e6ba` |
| `small_dynamic_500k_clean` | 500,000 | 300,000 | 0.00% | 0.00% | 0 | `5616823e3cc02b0c74ebf43d0ff35a0f0644dee9089f84c6af54e26e2f9e224d` |
| `small_dynamic_500k_range` | 500,000 | 300,000 | 40.00% | 0.00% | 0 | `855f2e4a575d1640ed37277634eb003ea0876e5957ac764a2f7b8275b3b9bc7e` |
| `ls24_density_preserved` | 100,663,296 | 300,000 | 40.00% | 0.00% | 0 | `44d61ec759a4142e3d62786d56eeae6ace6880f7c30a5b707c2e056a751e6e30` |
| `ls24_density_clean` | 100,663,296 | 300,000 | 0.00% | 0.00% | 0 | `9729badf681bf841bcf4bb65e74047b333605f421dc9e0b3ecfd3251d79d97ac` |
| `wb_cleanwrite_256mb` | 1,000,000 | 1,310,720 | 0.00% | 0.00% | 0 | `01b92cd17797c406f407075cb4a63824b9d4d05fa77f9a149096a6db3f28f2dc` |

---

## 4. 运行纪律与准入机制

1. **零脏库准入**：每轮实验必须在全新且此前不存在的 `db_path` 路径下执行；
2. **全量哈希自检**：启动前必须解析 `manifest.json` 并对 24 个分片二进制文件逐一进行 SHA-256 比对；
3. **比特级终态对账**：每轮结束后执行全库顺序扫描计算规范化 SHA-256，必须与在线状态模型计算结果 100% 逐比特一致；
4. **异常停机机制**：任何引擎内部错误、准入校验失败、模型对账失败均直接置该轮为 `INVALID` 并中止执行，严禁静默重试或人为剔除。
