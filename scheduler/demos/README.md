# scheduler demos

| 文件 | 说明 | 本机（笔记作者 box） |
|------|------|----------------------|
| `capture-sched.sh` | 采集 `/proc`、debugfs `sched/`、PSI、工具有无 | 脚本已按 v6.6.160 的接口改过；仓库里的 `box-sched-obs.txt` 是改脚本之前的样本 |
| `box-sched-obs.txt` | 一次只读抓取 | MEASURED，sampled on 6.12.94+。笔记源码钉 v6.6.160，数字没有在 6.6 上重测 |
| `wakeup_latency.bt` | bpftrace：wakeup→switch 直方图 | RECIPE，未跑（无 bpftrace）。字段对照 v6.6.160 的 `sched_wakeup` / `sched_switch` |
| `sched_switch_hist.bt` | bpftrace：switch 按 prev_state | RECIPE，未跑。`prev_state` 的编码按 v6.6.160 |

不要在生产机无脑 `perf sched record -a` 长时间开着——事件量很大。
