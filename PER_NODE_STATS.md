# Per-Node Statistics in pg_trace Ultimate

## ✅ YES! We Have Detailed Per-Node Statistics!

**pg_trace Ultimate** provides comprehensive statistics for **every execution plan node**, including:

✅ **Timing per node** (startup, total, average per loop)  
✅ **Buffer statistics per node** (hits, reads, writes)  
✅ **I/O timing per node** (with OS cache vs disk estimation)  
✅ **CPU time estimation per node** (wall clock - I/O time)  
✅ **Row counts per node** (actual rows, loops)  
✅ **Cache hit ratio per node**  
✅ **WAL statistics per node** (if available)  
✅ **Temp buffer usage per node**

---

## 📊 Example Per-Node Output

```
EXECUTION PLAN #1:
-> Hash Join (actual rows=1234 loops=1)
   Timing: startup=5.234 ms, total=125.678 ms
   Buffers: shared hit=9500 read=500 dirtied=50 written=10 (95.0% cache hit)
   I/O Detail: total=8.234 ms, avg=16.5 us/block, ~450 from OS cache, ~50 from disk
   Time breakdown: CPU ~117.4 ms (93.4%), I/O ~8.2 ms (6.6%)
  
  -> Seq Scan on employees (actual rows=5000 loops=1)
     Timing: startup=0.123 ms, total=45.678 ms
     Buffers: shared hit=4500 read=200 (95.7% cache hit)
     I/O Detail: total=3.456 ms, avg=17.3 us/block, ~190 from OS cache, ~10 from disk
     Time breakdown: CPU ~42.2 ms (92.4%), I/O ~3.5 ms (7.6%)
  
  -> Hash (actual rows=1000 loops=1)
     Timing: startup=2.345 ms, total=35.890 ms
     Buffers: shared hit=950 read=50 (95.0% cache hit)
     I/O Detail: total=0.987 ms, avg=19.7 us/block, ~48 from OS cache, ~2 from disk
     Time breakdown: CPU ~34.9 ms (97.2%), I/O ~1.0 ms (2.8%)
     
     -> Index Scan on departments_pkey (actual rows=1000 loops=1)
        Timing: startup=0.456 ms, total=33.545 ms
        Buffers: shared hit=950 read=50 (95.0% cache hit)
        I/O Detail: total=0.987 ms, avg=19.7 us/block, ~48 from OS cache, ~2 from disk
        Time breakdown: CPU ~32.6 ms (97.1%), I/O ~1.0 ms (2.9%)
```

---

## 🔍 What Each Statistic Means

### 1. Node Identification
```
-> Hash Join (actual rows=1234 loops=1)
```
- **Node type:** Hash Join, Seq Scan, Index Scan, etc.
- **Actual rows:** How many rows this node actually returned
- **Loops:** How many times this node was executed

### 2. Timing Statistics
```
Timing: startup=5.234 ms, total=125.678 ms, avg=125.678 ms/loop
```
- **Startup time:** Time to initialize and get first row
- **Total time:** Complete execution time for all rows
- **Average per loop:** If node runs multiple times (e.g., in nested loop)

### 3. Buffer Statistics
```
Buffers: shared hit=9500 read=500 dirtied=50 written=10 (95.0% cache hit)
```
- **shared hit:** Blocks found in PostgreSQL shared buffers (Tier 1 cache)
- **shared read:** Blocks not in PG cache, had to read from OS/disk (Tier 2/3)
- **dirtied:** Blocks modified by this node
- **written:** Blocks written to disk by this node
- **Cache hit %:** Percentage found in PG buffers

### 4. I/O Timing Detail (requires track_io_timing=on)
```
I/O Detail: total=8.234 ms, avg=16.5 us/block, ~450 from OS cache, ~50 from disk
```
- **Total I/O time:** Time spent in actual I/O operations
- **Average per block:** Average I/O latency (key metric!)
- **From OS cache:** Estimated blocks from OS page cache (fast, <500us)
- **From disk:** Estimated blocks from physical storage (slow, >500us)

### 5. Time Breakdown (CPU vs I/O)
```
Time breakdown: CPU ~117.4 ms (93.4%), I/O ~8.2 ms (6.6%)
```
- **CPU time:** Estimated as (total time - I/O time)
- **I/O time:** Actual measured I/O time
- **Percentages:** Where this node spent its time

**Important:** CPU time is an *estimate* based on wall clock minus I/O.
It's not from `/proc` per-node (that would require excessive overhead), but it's
still very useful for identifying CPU-bound vs I/O-bound nodes!

### 6. Additional Statistics

**Local Buffers** (for temporary tables):
```
Local Buffers: hit=100 read=50 written=30
```

**Temp Buffers** (for sorts/hashes that spill to disk):
```
Temp Buffers: read=1000 written=1000
```

**WAL Statistics** (for write operations):
```
WAL: records=500 fpi=10 bytes=102400
```

---

## 🎯 How to Read Per-Node Statistics

### Pattern 1: I/O-Bound Node
```
-> Seq Scan (actual rows=1000000 loops=1)
   Timing: total=5000.0 ms
   Buffers: shared hit=100 read=9900 (1.0% cache hit)  ← LOW hit rate!
   I/O Detail: total=4800 ms, avg=485 us/block, ~500 from OS cache, ~9400 from disk
   Time breakdown: CPU ~200 ms (4%), I/O ~4800 ms (96%)  ← I/O dominated!
```
**Problem:** Disk I/O is the bottleneck (96% of time!)  
**Solution:** Add index, increase shared_buffers, or optimize query

### Pattern 2: CPU-Bound Node
```
-> Hash Join (actual rows=5000000 loops=1)
   Timing: total=3000.0 ms
   Buffers: shared hit=9500 read=500 (95% cache hit)  ← HIGH hit rate!
   I/O Detail: total=10 ms, avg=20 us/block, ~500 from OS cache
   Time breakdown: CPU ~2990 ms (99.7%), I/O ~10 ms (0.3%)  ← CPU dominated!
```
**Problem:** CPU processing is the bottleneck  
**Solution:** Simplify join conditions, add more RAM, or use parallel query

### Pattern 3: Perfectly Cached
```
-> Index Scan (actual rows=100 loops=1)
   Timing: total=5.0 ms
   Buffers: shared hit=150 read=0 (100% cache hit)  ← PERFECT!
   (no I/O timing - everything from cache)
   Time breakdown: CPU ~5.0 ms (100%), I/O ~0 ms (0%)  ← All CPU!
```
**Status:** Optimal! All data in cache, fast execution

### Pattern 4: OS Cache Heavy
```
-> Seq Scan (actual rows=10000 loops=1)
   Timing: total=500.0 ms
   Buffers: shared hit=100 read=900 (10% cache hit)  ← Low PG cache
   I/O Detail: total=50 ms, avg=55 us/block, ~890 from OS cache, ~10 from disk
   Time breakdown: CPU ~450 ms (90%), I/O ~50 ms (10%)
```
**Status:** OK but could be better. Increase `shared_buffers` to reduce OS cache reliance.

---

## 📈 Using Per-Node Stats for Optimization

### Step 1: Find the Bottleneck Node

Look for nodes with:
- ✅ **Highest total time** - Where most time is spent
- ✅ **Low cache hit ratio** (<80%) - Not using cache well
- ✅ **High I/O percentage** (>20%) - I/O bound
- ✅ **High estimated rows vs actual** - Poor planner estimates

### Step 2: Analyze the Pattern

**If I/O-bound (>20% I/O time):**
1. Check if index exists and is being used
2. Increase `shared_buffers` if cache hit is low
3. Consider partitioning if scanning large tables
4. Check if `work_mem` needs increasing (for sorts/hashes)

**If CPU-bound (>80% CPU time):**
1. Simplify expressions in WHERE/JOIN clauses
2. Check for function calls that could be indexed
3. Consider parallel query if applicable
4. Look for expensive string operations

**If looping excessively (loops > 1000):**
1. Nested loop with wrong join order
2. Missing index on join key
3. Need to increase `random_page_cost` if using HDD

### Step 3: Validate the Fix

```sql
-- Before optimization
SELECT pg_trace_start_trace();
SELECT * FROM slow_query;
SELECT pg_trace_stop_trace();
-- Note the per-node stats

-- Apply fix (add index, tune config, etc.)

-- After optimization
SELECT pg_trace_start_trace();
SELECT * FROM slow_query;
SELECT pg_trace_stop_trace();
-- Compare the per-node stats!
```

---

## 🔬 Advanced: Comparing Execution Nodes

### Identify Inefficient Joins

```
-> Nested Loop (actual rows=1000 loops=1)
   Timing: total=5000 ms  ← SLOW!
   Buffers: shared hit=100 read=9900 (1% cache hit)
  
  -> Seq Scan on orders (actual rows=1000 loops=1)
     Timing: total=100 ms
     Buffers: shared hit=100 read=0
  
  -> Index Scan on customers (actual rows=1 loops=1000)  ← EXPENSIVE INNER!
     Timing: total=4900 ms
     Buffers: shared hit=0 read=9900  ← No cache reuse!
     I/O Detail: total=4850 ms, ~9900 from disk
```

**Problem:** Inner side runs 1000 times with no cache benefit  
**Solution:** Switch to Hash Join or increase shared_buffers

### Find Unnecessary Sorts

```
-> Sort (actual rows=10 loops=1)
   Timing: total=0.5 ms
   Buffers: shared hit=10 read=0
   
vs.

-> Sort (actual rows=1000000 loops=1)
   Timing: total=5000 ms
   Buffers: shared hit=0 read=0
   Temp Buffers: read=10000 written=10000  ← SPILLED TO DISK!
```

**Problem:** Sort doesn't fit in `work_mem`, spills to disk  
**Solution:** Increase `work_mem` or add index to avoid sort

---

## 💡 Pro Tips

### 1. Focus on High-Time Nodes First
The nodes with `total=XXX ms` where XXX is large are your first targets.

### 2. Watch for Loops
`loops=1000` on an inner node often indicates a performance problem.

### 3. Cache Hit Ratio is Key
- **>95%:** Excellent
- **80-95%:** Good
- **60-80%:** Needs attention
- **<60%:** Critical - fix immediately

### 4. I/O Latency Tells a Story
- **<100 us:** Likely OS cache (good)
- **100-500 us:** Mixed or fast SSD (OK)
- **500-2000 us:** Mostly disk on SSD (investigate)
- **>2000 us:** HDD or storage problem (critical)

### 5. CPU vs I/O Balance
- **90% CPU, 10% I/O:** Optimal for most queries
- **50% CPU, 50% I/O:** Investigate cache tuning
- **10% CPU, 90% I/O:** Critical I/O bottleneck!

---

## 🆚 Comparison with Oracle 10046

| Statistic | Oracle 10046 | pg_trace Ultimate |
|-----------|-------------|-------------------|
| **Per-node timing** | ✅ | ✅ |
| **Per-node buffer stats** | ✅ | ✅ |
| **Per-node I/O timing** | ✅ | ✅ |
| **Per-node CPU (measured)** | ✅ | ⚠️ Estimated* |
| **Per-node cache analysis** | ✅ | ✅ |
| **OS cache detection** | ❌ | ✅ **BETTER!** |
| **Rows actual vs estimated** | ✅ | ✅ |
| **Loop counts** | ✅ | ✅ |

*PostgreSQL doesn't expose per-node CPU from `/proc` without excessive overhead,
so we estimate it as (wall clock - I/O time). This is still very useful!

---

## 📝 Limitations

### What We CAN Measure Per Node:
✅ Wall clock time (precise)  
✅ Buffer hits/reads (exact)  
✅ I/O time (precise with track_io_timing)  
✅ CPU time (estimated)  
✅ Rows processed (exact)  
✅ Loops (exact)  

### What We CANNOT Measure Per Node:
❌ Exact CPU time from `/proc` (would require hooking every node start/end)  
❌ Context switches per node (not exposed by PostgreSQL)  
❌ Memory allocation per node (not tracked by Instrumentation)  
❌ Network I/O per node (not applicable for local queries)  

**For exact per-node CPU from `/proc`, you'd need kernel-level tracing (eBPF),
which requires root access and adds significant overhead.**

---

## 🚀 Example Use Case

### Finding the Slow Node:

```sql
SELECT pg_trace_start_trace();

EXPLAIN ANALYZE SELECT o.*, c.name
FROM orders o
JOIN customers c ON o.customer_id = c.id
WHERE o.order_date > '2024-01-01';

SELECT pg_trace_stop_trace();
```

**Trace output shows:**

```
-> Hash Join (total=1500 ms)
  -> Seq Scan on orders (total=1200 ms)  ← 80% of time here!
     Buffers: shared hit=100 read=9900 (1% cache hit)
     I/O Detail: total=1100 ms, ~9900 from disk  ← ALL FROM DISK!
     Time breakdown: CPU ~100 ms, I/O ~1100 ms (92% I/O)
  -> Hash (total=200 ms)
     -> Index Scan on customers (total=150 ms)
        Buffers: shared hit=500 read=0 (100% cache hit)
```

**Action:** Add index on `orders.order_date` to avoid seq scan!

**After adding index:**

```
-> Hash Join (total=50 ms)
  -> Index Scan on orders (total=30 ms)  ← 40x faster!
     Buffers: shared hit=100 read=0 (100% cache hit)
     Time breakdown: CPU ~30 ms, I/O ~0 ms (all CPU)
  -> Hash (total=15 ms)
```

---

## ✨ Summary

**YES, we have comprehensive per-node statistics!**

Every execution plan node shows:
- ✅ Precise timing
- ✅ Buffer statistics
- ✅ I/O timing and breakdown
- ✅ CPU time estimation
- ✅ Cache hit ratios
- ✅ OS cache vs disk

This gives you **everything you need** to optimize queries at the node level,
just like Oracle 10046!

**Start tracing:** `SELECT pg_trace_start_trace();`

