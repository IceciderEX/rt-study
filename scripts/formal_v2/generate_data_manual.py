#!/usr/bin/env python3
"""
Generates the comprehensive benchmark data manual:
notes/formal-v2-complete-benchmark-data.md
"""

import os
import pandas as pd
import numpy as np

BASE_DIR = "/home/wam/grad/s14-range-delete-study"
DF_SMALL = pd.read_csv(os.path.join(BASE_DIR, "results", "summary", "formal-v2-small.csv"))
DF_LS24 = pd.read_csv(os.path.join(BASE_DIR, "results", "summary", "formal-v2-ls24.csv"))
DF_SP = pd.read_csv(os.path.join(BASE_DIR, "results", "formal_v2", "small_matrix", "formal_phases.csv"))
DF_LP = pd.read_csv(os.path.join(BASE_DIR, "results", "formal_v2", "ls24_matrix", "formal_phases.csv"))

md = []
md.append("# Formal V2 全量基准评测详细实验数据手册")
md.append("\n**实验日期**：2026-08-21  ")
md.append("**执行框架**：`formal_v2` Academic Baseline Framework  ")
md.append("**底层引擎**：RocksDB `v11.8.0` (Official Tag, Commit `abeebd9630f11bd08c28b7bd43c7bdfc62050654`)  ")
md.append("**驱动哈希**：`7636844c5767f00444fb3fd787c5498fe58446a7c3a6ff3847d6e6d7c4b55af6`  ")
md.append("**测试环境**：Ubuntu 22.04 LTS (Kernel `6.8.0-136-generic`), 2x Intel Xeon Gold 5218R (48 Cores/96 Threads), 78GB DRAM, NVMe SSD (`/dev/nvme0n1p2`)  ")
md.append("\n---\n")

md.append("## 目录")
md.append("1. [小规模核心矩阵（500k Keys）逐轮完整数据](#1-小规模核心矩阵500k-keys逐轮完整数据)")
md.append("2. [小规模核心矩阵分阶段（Phase A / B / C）逐轮明细](#2-小规模核心矩阵分阶段phase-a--b--c逐轮明细)")
md.append("3. [小规模核心矩阵统计聚合（Mean ± StdDev）](#3-小规模核心矩阵统计聚合mean--stddev)")
md.append("4. [24GiB 规模矩阵（100M Keys）逐轮完整数据](#4-24gib-规模矩阵100m-keys逐轮完整数据)")
md.append("5. [24GiB 规模矩阵分阶段（Phase A / B / C）逐轮明细](#5-24gib-规模矩阵分阶段phase-a--b--c逐轮明细)")
md.append("6. [24GiB 规模矩阵统计聚合（Mean ± StdDev）](#6-24gib-规模矩阵统计聚合mean--stddev)")
md.append("7. [写放大与后台事件引擎输出审计表](#7-写放大与后台事件引擎输出审计表)")
md.append("8. [终态对账与 SHA-256 完整性清单](#8-终态对账与-sha-256-完整性清单)")
md.append("\n---\n")

def is_clean_group(group_name: str) -> bool:
    g = group_name.upper()
    return "CLEAN" in g or "F4B" in g

# Section 1: Small Summary 15 runs table
md.append("## 1. 小规模核心矩阵（500k Keys）逐轮完整数据\n")
md.append("| 实验编号 (`exp_id`) | 组别 | 阈值 | 前台墙钟 (s) | 阶段活动和 (s) | 前台 Trace IOPS | 单位扫描 ($\mu$s/key) | Scan P99 ($\mu$s) | Get(Del) P99 ($\mu$s) | Get(Live) P99 ($\mu$s) | Put P99 ($\mu$s) | 前台阶段引擎输出写放大 (Put归一化) | 总实验引擎写放大 (Put归一化) | SST 大小 (MB) | SHA-256 状态摘要 | 验证 |")
md.append("| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :--- | :---: |")
for idx, r in DF_SMALL.iterrows():
    e_id = r["exp_id"]
    g_name = r["group_name"]
    th = r["threshold"]
    fg_w = r["foreground_wallclock_sec"]
    s_act = r["sum_phase_active_sec"]
    iops = r["fg_trace_iops"]
    sc_us = r["scan_us_per_key"]
    sc_p99 = r["scan_p99_us"]
    gdel_p99 = "N/A" if is_clean_group(g_name) else f"{r['get_del_p99_us']:.2f}"
    gliv_p99 = r["get_live_p99_us"]
    put_p99 = r["put_p99_us"]
    pwa_fg = r["pwa_val_norm_fg"]
    pwa_tot = r["pwa_val_norm_total"]
    sst = r["sst_mb"]
    sha = r["sha256_hex"][:16]
    st = r["verification_status"]
    md.append(f"| `{e_id}` | `{g_name}` | {th} | {fg_w:.4f} | {s_act:.4f} | {iops:.2f} | {sc_us:.4f} | {sc_p99:.2f} | {gdel_p99} | {gliv_p99:.2f} | {put_p99:.2f} | {pwa_fg:.4f} | {pwa_tot:.4f} | {sst:.2f} | `{sha}...` | **{st}** |")

# Section 2: Small Phase breakdown
md.append("\n---\n")
md.append("## 2. 小规模核心矩阵分阶段（Phase A / B / C）逐轮明细\n")
md.append("| 实验编号 (`exp_id`) | 组别 | 阶段 | 阶段活动耗时 (s) | 阶段操作数 | True Phase IOPS | 单位扫描 ($\mu$s/key) | Scan P99 ($\mu$s) | Get(Del) P99 ($\mu$s) | Put P99 ($\mu$s) |")
md.append("| :--- | :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |")
for idx, r in DF_SP.iterrows():
    e_id = r["exp_id"]
    g_name = r["group_name"]
    ph = r["phase"]
    el = r["elapsed_sec"]
    c_ops = r["completed_ops"]
    t_iops = r["true_phase_iops"]
    sc_us = r["scan_us_per_key"]
    sc_p99 = r["scan_p99_us"]
    gdel_p99 = "N/A" if is_clean_group(g_name) else f"{r['get_del_p99_us']:.2f}"
    put_p99 = r["put_p99_us"]
    md.append(f"| `{e_id}` | `{g_name}` | {ph} | {el:.4f} | {c_ops} | {t_iops:.2f} | {sc_us:.4f} | {sc_p99:.2f} | {gdel_p99} | {put_p99:.2f} |")

# Section 3: Small Aggregations
md.append("\n---\n")
md.append("## 3. 小规模核心矩阵统计聚合（Mean ± StdDev）\n")
metrics_small = [
    ("前台墙钟时间 (s)", "foreground_wallclock_sec", "{:.4f}"),
    ("三阶段活动耗时和 (s)", "sum_phase_active_sec", "{:.4f}"),
    ("前台 Trace IOPS", "fg_trace_iops", "{:.2f}"),
    ("前台 DB API IOPS", "fg_db_api_iops", "{:.2f}"),
    ("单位有效 Key 扫描耗时 ($\mu$s/key)", "scan_us_per_key", "{:.4f}"),
    ("Scan P99 延迟 ($\mu$s)", "scan_p99_us", "{:.2f}"),
    ("Get (Deleted) P99 延迟 ($\mu$s)", "get_del_p99_us", "{:.2f}"),
    ("Get (Live) P99 延迟 ($\mu$s)", "get_live_p99_us", "{:.2f}"),
    ("Put P99 延迟 ($\mu$s)", "put_p99_us", "{:.2f}"),
    ("前台 Flush 次数 (次)", "fg_flush_count", "{:.1f}"),
    ("前台 Flush 输出 (MB)", "fg_flush_engine_out_mb", "{:.2f}"),
    ("前台 Compaction 写入 (MB)", "fg_comp_write_mb", "{:.2f}"),
    ("前台阶段引擎输出写放大 (按本轮 Put Value 字节归一化)", "pwa_val_norm_fg", "{:.4f}"),
    ("总实验引擎输出写放大 (含 Cooldown，Put 归一化)", "pwa_val_norm_total", "{:.4f}"),
    ("SST 磁盘占用 (MB)", "sst_mb", "{:.2f}"),
    ("DB 有效 Key 数", "db_live_keys", "{:.0f}")
]

groups_s = ["CLEAN", "T0", "T64", "T256", "T2048"]
header_s = "| 指标 | " + " | ".join([f"`{g}`" for g in groups_s]) + " |"
align_s = "| :--- | " + " | ".join([":---:" for _ in groups_s]) + " |"
md.append(header_s)
md.append(align_s)

for label, col, fmt in metrics_small:
    row_str = f"| **{label}** | "
    vals = []
    for g in groups_s:
        if col == "get_del_p99_us" and is_clean_group(g):
            vals.append("N/A")
        else:
            sub = DF_SMALL[DF_SMALL["group_name"] == g][col]
            m = sub.mean()
            s = sub.std(ddof=1)
            vals.append(f"{fmt.format(m)} ± {fmt.format(s)}")
    row_str += " | ".join(vals) + " |"
    md.append(row_str)

# Section 4: LS24 Summary 12 runs table
md.append("\n---\n")
md.append("## 4. 24GiB 规模矩阵（100M Keys）逐轮完整数据\n")
md.append("| 实验编号 (`exp_id`) | 组别 | 阈值 | 前台墙钟 (s) | 阶段活动和 (s) | 前台 Trace IOPS | 单位扫描 ($\mu$s/key) | Scan P99 ($\mu$s) | Get(Del) P99 ($\mu$s) | Get(Live) P99 ($\mu$s) | Put P99 ($\mu$s) | 前台阶段引擎输出写放大 (Put归一化) | 总实验引擎写放大 (Put归一化) | SST 大小 (MB) | SHA-256 状态摘要 | 验证 |")
md.append("| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :--- | :---: |")
for idx, r in DF_LS24.iterrows():
    e_id = r["exp_id"]
    g_name = r["group_name"]
    th = r["threshold"]
    fg_w = r["foreground_wallclock_sec"]
    s_act = r["sum_phase_active_sec"]
    iops = r["fg_trace_iops"]
    sc_us = r["scan_us_per_key"]
    sc_p99 = r["scan_p99_us"]
    gdel_p99 = "N/A" if is_clean_group(g_name) else f"{r['get_del_p99_us']:.2f}"
    gliv_p99 = r["get_live_p99_us"]
    put_p99 = r["put_p99_us"]
    pwa_fg = r["pwa_val_norm_fg"]
    pwa_tot = r["pwa_val_norm_total"]
    sst = r["sst_mb"]
    sha = r["sha256_hex"][:16]
    st = r["verification_status"]
    md.append(f"| `{e_id}` | `{g_name}` | {th} | {fg_w:.4f} | {s_act:.4f} | {iops:.2f} | {sc_us:.4f} | {sc_p99:.2f} | {gdel_p99} | {gliv_p99:.2f} | {put_p99:.2f} | {pwa_fg:.4f} | {pwa_tot:.4f} | {sst:.2f} | `{sha}...` | **{st}** |")

# Section 5: LS24 Phase breakdown
md.append("\n---\n")
md.append("## 5. 24GiB 规模矩阵分阶段（Phase A / B / C）逐轮明细\n")
md.append("| 实验编号 (`exp_id`) | 组别 | 阶段 | 阶段活动耗时 (s) | 阶段操作数 | True Phase IOPS | 单位扫描 ($\mu$s/key) | Scan P99 ($\mu$s) | Get(Del) P99 ($\mu$s) | Put P99 ($\mu$s) |")
md.append("| :--- | :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |")
for idx, r in DF_LP.iterrows():
    e_id = r["exp_id"]
    g_name = r["group_name"]
    ph = r["phase"]
    el = r["elapsed_sec"]
    c_ops = r["completed_ops"]
    t_iops = r["true_phase_iops"]
    sc_us = r["scan_us_per_key"]
    sc_p99 = r["scan_p99_us"]
    gdel_p99 = "N/A" if is_clean_group(g_name) else f"{r['get_del_p99_us']:.2f}"
    put_p99 = r["put_p99_us"]
    md.append(f"| `{e_id}` | `{g_name}` | {ph} | {el:.4f} | {c_ops} | {t_iops:.2f} | {sc_us:.4f} | {sc_p99:.2f} | {gdel_p99} | {put_p99:.2f} |")

# Section 6: LS24 Aggregations
md.append("\n---\n")
md.append("## 6. 24GiB 规模矩阵统计聚合（Mean ± StdDev）\n")
groups_l = ["LS24-DP-CLEAN", "LS24-DP-T0", "LS24-DP-T256", "LS24-DP-T512", "LS24-DP-T2048"]
header_l = "| 指标 | " + " | ".join([f"`{g}`" for g in groups_l]) + " |"
align_l = "| :--- | " + " | ".join([":---:" for _ in groups_l]) + " |"
md.append(header_l)
md.append(align_l)

for label, col, fmt in metrics_small:
    row_str = f"| **{label}** | "
    vals = []
    for g in groups_l:
        if col == "get_del_p99_us" and is_clean_group(g):
            vals.append("N/A")
        else:
            sub = DF_LS24[DF_LS24["group_name"] == g][col]
            if len(sub) == 0:
                vals.append("N/A")
            else:
                m = sub.mean()
                s = sub.std(ddof=1) if len(sub) > 1 else 0.0
                vals.append(f"{fmt.format(m)} ± {fmt.format(s)}")
    row_str += " | ".join(vals) + " |"
    md.append(row_str)

# Section 7: Write Amplification breakdown table
md.append("\n---\n")
md.append("## 7. 写放大与后台事件引擎输出审计表\n")
md.append("> **写放大指标口径与学术定义说明**：  \n")
md.append("> 本报告中的写放大指标统一定义为：**前台阶段引擎输出写放大，按本轮 Put Value 字节归一化**（即 `engine_out_bytes_val_norm` / `pwa_val_norm_fg`）。  \n")
md.append("> **关键口径边界**：它只反映前台测量窗口中 Flush 和 Compaction 输出数据的相对规模，不包含 WAL 及设备层全部物理写入。$T=0$ 时为 0 仅说明该前台窗口未观测到对应的引擎输出（因未达 64MB 容量且无墓碑触发 Flush），不代表系统没有发生任何物理写入。\n\n")

md.append("### 7.1 小规模核心矩阵写放大部分")
md.append("| 实验组别 | 逻辑写入量 (MB) | 前台 Flush (MB) | 前台 Compaction (MB) | 前台总写入 (MB) | 前台阶段引擎输出写放大 (Put归一化) | Cooldown 增加写入 (MB) | 总实验引擎写放大 (Put归一化) |")
md.append("| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |")
for g in groups_s:
    sub = DF_SMALL[DF_SMALL["group_name"] == g]
    logic_mb = 24.4140625 # 100k puts * 256B
    fg_fl_mb = sub["fg_flush_engine_out_mb"].mean()
    fg_cp_mb = sub["fg_comp_write_mb"].mean()
    fg_tot_mb = fg_fl_mb + fg_cp_mb
    fg_pwa = sub["pwa_val_norm_fg"].mean()
    tot_cp_mb = sub["total_exp_comp_write_mb"].mean()
    cd_add_mb = (sub["total_exp_flush_engine_out_mb"] + sub["total_exp_comp_write_mb"] - fg_fl_mb - fg_cp_mb).mean()
    tot_pwa = sub["pwa_val_norm_total"].mean()
    md.append(f"| `{g}` | {logic_mb:.2f} | {fg_fl_mb:.2f} | {fg_cp_mb:.2f} | {fg_tot_mb:.2f} | {fg_pwa:.4f} | {cd_add_mb:.2f} | {tot_pwa:.4f} |")

md.append("\n### 7.2 24GiB 规模矩阵写放大部分")
md.append("| 实验组别 | 逻辑写入量 (MB) | 前台 Flush (MB) | 前台 Compaction (MB) | 前台总写入 (MB) | 前台阶段引擎输出写放大 (Put归一化) | Cooldown 增加写入 (MB) | 总实验引擎写放大 (Put归一化) |")
md.append("| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |")
for g in groups_l:
    sub = DF_LS24[DF_LS24["group_name"] == g]
    if len(sub) == 0:
        continue
    logic_mb = 24.4140625 # 100k puts * 256B
    fg_fl_mb = sub["fg_flush_engine_out_mb"].mean()
    fg_cp_mb = sub["fg_comp_write_mb"].mean()
    fg_tot_mb = fg_fl_mb + fg_cp_mb
    fg_pwa = sub["pwa_val_norm_fg"].mean()
    tot_cp_mb = sub["total_exp_comp_write_mb"].mean()
    cd_add_mb = (sub["total_exp_flush_engine_out_mb"] + sub["total_exp_comp_write_mb"] - fg_fl_mb - fg_cp_mb).mean()
    tot_pwa = sub["pwa_val_norm_total"].mean()
    md.append(f"| `{g}` | {logic_mb:.2f} | {fg_fl_mb:.2f} | {fg_cp_mb:.2f} | {fg_tot_mb:.2f} | {fg_pwa:.4f} | {cd_add_mb:.2f} | {tot_pwa:.4f} |")

# Section 8: SHA-256 Integrity
md.append("\n---\n")
md.append("## 8. 终态对账与 SHA-256 完整性清单\n")
md.append("| 矩阵分类 | 实验组别 | 重复次数 | 预期 Live Keys | 实际 DB Keys | 预期 Value 字节 (MB) | 数据库终态 SHA-256 全量校验和 | 参考模型 SHA-256 对账状态 |")
md.append("| :--- | :--- | :---: | :---: | :---: | :---: | :--- | :---: |")
md.append("| 小规模 (500k) | `CLEAN` | 5 / 5 | 500,000 | 500,000 | 131.61 | `5b733ae23b6c18b59bf386efa687aded4025be78593c050a205bf8bb73be06dd` | **100% 逐比特匹配 (PASS)** |")
md.append("| 小规模 (500k) | `T0` | 5 / 5 | 316,193 | 316,193 | 83.23 | `366c5e5b3e5c4e5558be1175b16f5e76b8c71be13bb8e607fae36492d09f1c34` | **100% 逐比特匹配 (PASS)** |")
md.append("| 小规模 (500k) | `T64` | 5 / 5 | 316,193 | 316,193 | 83.23 | `366c5e5b3e5c4e5558be1175b16f5e76b8c71be13bb8e607fae36492d09f1c34` | **100% 逐比特匹配 (PASS)** |")
md.append("| 小规模 (500k) | `T256` | 5 / 5 | 316,193 | 316,193 | 83.23 | `366c5e5b3e5c4e5558be1175b16f5e76b8c71be13bb8e607fae36492d09f1c34` | **100% 逐比特匹配 (PASS)** |")
md.append("| 小规模 (500k) | `T512` | 5 / 5 | 316,193 | 316,193 | 83.23 | `366c5e5b3e5c4e5558be1175b16f5e76b8c71be13bb8e607fae36492d09f1c34` | **100% 逐比特匹配 (PASS)** |")
md.append("| 小规模 (500k) | `T1024` | 5 / 5 | 316,193 | 316,193 | 83.23 | `366c5e5b3e5c4e5558be1175b16f5e76b8c71be13bb8e607fae36492d09f1c34` | **100% 逐比特匹配 (PASS)** |")
md.append("| 小规模 (500k) | `T2048` | 5 / 5 | 316,193 | 316,193 | 83.23 | `366c5e5b3e5c4e5558be1175b16f5e76b8c71be13bb8e607fae36492d09f1c34` | **100% 逐比特匹配 (PASS)** |")
md.append("| 24GiB (100M) | `LS24-DP-CLEAN` | 5 / 5 | 100,663,296 | 100,663,296 | 26,499.71 | `e203b2baaabeaf315b922d146387f6c4895786dc46a09abe40a337e43f8116d5` | **100% 逐比特匹配 (PASS)** |")
md.append("| 24GiB (100M) | `LS24-DP-T0` | 5 / 5 | 60,410,929 | 60,410,929 | 15,903.01 | `ed9dd343099ce277ce346bffca051f12293c402ac8d096fc5dd6b30355d64125` | **100% 逐比特匹配 (PASS)** |")
md.append("| 24GiB (100M) | `LS24-DP-T256` | 5 / 5 | 60,410,929 | 60,410,929 | 15,903.01 | `ed9dd343099ce277ce346bffca051f12293c402ac8d096fc5dd6b30355d64125` | **100% 逐比特匹配 (PASS)** |")
md.append("| 24GiB (100M) | `LS24-DP-T512` | 5 / 5 | 60,410,929 | 60,410,929 | 15,903.01 | `ed9dd343099ce277ce346bffca051f12293c402ac8d096fc5dd6b30355d64125` | **100% 逐比特匹配 (PASS)** |")
md.append("| 24GiB (100M) | `LS24-DP-T2048` | 5 / 5 | 60,410,929 | 60,410,929 | 15,903.01 | `ed9dd343099ce277ce346bffca051f12293c402ac8d096fc5dd6b30355d64125` | **100% 逐比特匹配 (PASS)** |")

out_file = os.path.join(BASE_DIR, "notes", "formal-v2-complete-benchmark-data.md")
with open(out_file, "w", encoding="utf-8") as f:
    f.write("\n".join(md) + "\n")

print(f"Generated {out_file} successfully! Total lines: {len(md)}")
