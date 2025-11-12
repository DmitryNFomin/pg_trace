# /proc Filesystem Approach vs eBPF

## TL;DR

**You can get 90% of what you need from `/proc` without eBPF or root!**

## What `/proc` Gives You

### ✅ Available from `/proc`

| Statistic | Source | Accuracy | Notes |
|-----------|--------|----------|-------|
| **User CPU time** | `/proc/[pid]/stat` | ✅ Exact | Per-process cumulative |
| **System CPU time** | `/proc/[pid]/stat` | ✅ Exact | Per-process cumulative |
| **Storage I/O bytes** | `/proc/[pid]/io` | ✅ Exact | read_bytes, write_bytes |
| **I/O syscalls** | `/proc/[pid]/io` | ✅ Exact | syscr, syscw counts |
| **Total I/O** | `/proc/[pid]/io` | ✅ Exact | Including cache |
| **Memory usage** | `/proc/[pid]/status` | ✅ Exact | RSS, Peak, Size |
| **Context switches** | `/proc/[pid]/status` | ✅ Exact | voluntary, nonvoluntary |

### ❌ NOT Available from `/proc`

| Statistic | Why Not | Alternative |
|-----------|---------|-------------|
| **Wait event names** | Not in /proc | eBPF or sampling |
| **Wait durations** | Not tracked | eBPF or timestamps |
| **Per-block I/O** | Aggregated only | eBPF system calls |
| **Network I/O details** | Limited | eBPF socket tracing |

## Comparison: /proc vs eBPF

### /proc Approach (Enhanced MVP)

**Pros:**
- ✅ **No root required**
- ✅ **Works everywhere** (any Linux)
- ✅ **Simple** - just file reads
- ✅ **Accurate** - kernel provides exact stats
- ✅ **Low overhead** - ~0.1% for reading /proc
- ✅ **Reliable** - stable interface

**Cons:**
- ❌ **Aggregated** - cumulative stats, not per-operation
- ❌ **No wait events** - can't see individual waits
- ❌ **Polling required** - sample before/after
- ❌ **No breakdown** - can't attribute to specific operations

**What you get:**
```
EXEC #1
EXEC: c=1234567,e=1.234567,r=1000
  OS CPU: user=0.850s sys=0.384s total=1.234s
  OS I/O: read=8192000 bytes write=0 bytes
STAT #1: cr=1000 pr=1000 pw=0 dirtied=0
```

### eBPF Approach (Original MVP)

**Pros:**
- ✅ **Precise timing** - nanosecond resolution
- ✅ **Per-event detail** - every wait logged
- ✅ **Wait event names** - know what you're waiting for
- ✅ **Real-time** - events as they happen
- ✅ **Low overhead** - <1% even when tracing

**Cons:**
- ❌ **Root required** - or CAP_BPF capability
- ❌ **Linux only** - kernel 4.9+
- ❌ **Complex setup** - BCC installation, scripts
- ❌ **Binary requirements** - needs symbols
- ❌ **Potential instability** - kernel-dependent

**What you get:**
```
WAIT #1: nam='DataFileRead' ela=156 us file#=16384 block#=12345
WAIT #1: nam='DataFileRead' ela=142 us file#=16384 block#=12346
WAIT #1: nam='LockManager' ela=1234 us
```

## Hybrid Approach (Recommended!)

**Combine both for maximum insight:**

### Base Layer: /proc (Always On)
- Provides OS-level CPU and I/O stats
- Works everywhere, no special setup
- Gives you the "big picture"

### Optional Layer: eBPF (When Needed)
- Adds detailed wait event tracing
- Use for deep troubleshooting
- Requires setup but gives precise timing

## Example Output Comparison

### Extension Only (Basic MVP)
```sql
SELECT pg_trace_start_trace();
SELECT * FROM large_table WHERE id > 1000;
SELECT pg_trace_stop_trace();
```

**Output:**
```
PARSE #1
SQL: SELECT * FROM large_table WHERE id > 1000
BINDS: (none)
EXEC: e=2.456,r=5000
STAT #1: cr=10000 pr=500 pw=0
PLAN:
  -> Seq Scan (rows=5000)
```

**Missing:** CPU time, I/O bytes, wait events

### Extension + /proc (Enhanced MVP)
```sql
SELECT pg_trace_start_trace();
SELECT * FROM large_table WHERE id > 1000;
SELECT pg_trace_stop_trace();
```

**Output:**
```
PARSE #1
SQL: SELECT * FROM large_table WHERE id > 1000
BINDS: (none)
EXEC: c=2100000,e=2.456,r=5000
  OS CPU: user=1.850s sys=0.606s total=2.456s
  OS I/O: read=4096000 bytes write=0 bytes
STAT #1: cr=10000 pr=500 pw=0
PLAN:
  -> Seq Scan (c=2100000,e=2456,r=5000)
     cr=10000 pr=500
MEMORY: rss=45678 KB peak=50000 KB
```

**Added:** CPU breakdown, I/O bytes, memory

### Extension + /proc + eBPF (Complete)
```sql
-- Same queries, plus eBPF tracer running
```

**Output:**
```
PARSE #1
SQL: SELECT * FROM large_table WHERE id > 1000
BINDS: (none)
EXEC: c=2100000,e=2.456,r=5000
  OS CPU: user=1.850s sys=0.606s total=2.456s
  OS I/O: read=4096000 bytes write=0 bytes

WAIT #1: nam='DataFileRead' ela=156 us file#=16384 block#=12345
WAIT #1: nam='DataFileRead' ela=142 us file#=16384 block#=12346
WAIT #1: nam='DataFileRead' ela=167 us file#=16388 block#=5432
... (500 waits total)

STAT #1: cr=10000 pr=500 pw=0
PLAN:
  -> Seq Scan (c=2100000,e=2456,r=5000)
     cr=10000 pr=500
```

**Added:** Individual wait events with timing

## Implementation Details

### Reading /proc in Extension

```c
// Before query execution
ProcStats stats_start;
proc_read_all_stats(MyProcPid, &stats_start);

// ... execute query ...

// After query execution  
ProcStats stats_end;
proc_read_all_stats(MyProcPid, &stats_end);

// Calculate differences
ProcCpuStats cpu_diff;
proc_cpu_stats_diff(&stats_start.cpu, &stats_end.cpu, &cpu_diff);

// Write to trace
trace_printf("OS CPU: user=%.3fs sys=%.3fs total=%.3fs\n",
             cpu_diff.utime_sec, 
             cpu_diff.stime_sec, 
             cpu_diff.total_sec);
```

### Performance Impact

| Operation | Overhead | Notes |
|-----------|----------|-------|
| Read `/proc/[pid]/stat` | ~10 µs | One small file |
| Read `/proc/[pid]/io` | ~10 µs | One small file |
| Read `/proc/[pid]/status` | ~20 µs | Slightly larger |
| **Total per query** | ~40 µs | 0.00004 seconds |
| **% of 1ms query** | 4% | Negligible |
| **% of 100ms query** | 0.04% | Essentially zero |

### What About Wait Events?

**Option 1: Sample from pg_stat_activity**
```c
// Sample current wait event
uint32 wait_event = MyProc->wait_event_info;
const char *wait_name = pgstat_get_wait_event(wait_event);
```

**Pros:** No eBPF needed  
**Cons:** Point-in-time only, no duration

**Option 2: Use eBPF (full solution)**
```python
# Trace wait functions with timing
sudo python3 pg_trace_waits.py -p <pid>
```

**Pros:** True timing  
**Cons:** Requires eBPF setup

**Option 3: Infer from I/O stats**
```c
// High I/O syscalls but low bytes = small I/O waits
// High read_bytes = disk I/O waits
if (io_diff.read_bytes > 1000000)
    trace_printf("LIKELY WAIT: Disk I/O (read %llu bytes)\n", 
                 io_diff.read_bytes);
```

**Pros:** Simple approximation  
**Cons:** Not precise

## Recommendation by Use Case

### Development/Testing
**Use:** Enhanced MVP (/proc only)
- Fast to set up
- Good enough detail
- No root needed

### Production Troubleshooting
**Use:** Enhanced MVP + optional eBPF
- Start with /proc stats
- Add eBPF if you need wait details
- Can enable eBPF temporarily

### Deep Performance Analysis
**Use:** Full solution (Extension + /proc + eBPF)
- Complete picture
- All available data
- Worth the eBPF setup effort

## Migration Path

**Phase 1:** Basic extension
- SQL, binds, plans, buffers
- ✅ Done in basic MVP

**Phase 2:** Add /proc stats  
- CPU time, I/O bytes, memory
- ✅ Done in enhanced MVP
- **← YOU ARE HERE (this is enough for most!)**

**Phase 3:** Optional eBPF
- Wait event timing
- For advanced users
- Available in original MVP

## Summary Table

| Feature | Extension Only | + /proc | + eBPF |
|---------|---------------|---------|--------|
| SQL text | ✅ | ✅ | ✅ |
| Bind variables | ✅ | ✅ | ✅ |
| Execution plan | ✅ | ✅ | ✅ |
| Buffer stats (cr/pr/pw) | ✅ | ✅ | ✅ |
| Elapsed time | ✅ | ✅ | ✅ |
| **CPU time (user/sys)** | ❌ | ✅ | ✅ |
| **I/O bytes** | ❌ | ✅ | ✅ |
| **Memory usage** | ❌ | ✅ | ✅ |
| **Wait event names** | ⚠️ | ⚠️ | ✅ |
| **Wait durations** | ❌ | ❌ | ✅ |
| **Root required** | ❌ | ❌ | ✅ |
| **Setup complexity** | Low | Low | High |
| **Overhead** | ~2% | ~2% | ~3% |

## Conclusion

**For most users, the Enhanced MVP with /proc stats is the sweet spot:**

✅ No root required  
✅ Works everywhere  
✅ Gives you CPU and I/O details  
✅ Easy to set up  
✅ Low overhead  

**Add eBPF only when you need:**
- Precise wait event timing
- Per-event breakdown
- Deep system analysis

The `/proc` approach gives you **90% of Oracle 10046** with **10% of the complexity**!

---

**Build the Enhanced MVP first, add eBPF later if needed.**

