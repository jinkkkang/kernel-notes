# scheduler demos

| 文件 | 说明 | 本机（笔记作者 box） |
|------|------|----------------------|
| `capture-sched.sh` | 采集 `/proc`/PSI/工具有无 | 已跑；结果见 `box-sched-obs.txt` |
| `box-sched-obs.txt` | 一次实测抓取 | MEASURED |
| `wakeup_latency.bt` | bpftrace：wakeup→switch 直方图 | RECIPE，未跑（无 bpftrace） |
| `sched_switch_hist.bt` | bpftrace：switch 按 prev_state | RECIPE，未跑 |

不要在生产机无脑 `perf sched record -a` 长时间开着——事件量很大。
