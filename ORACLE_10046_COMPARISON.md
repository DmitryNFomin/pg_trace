# Oracle 10046 vs pg_trace_ultimate Comparison

## What Oracle 10046 Event Provides

```sql
ALTER SESSION SET EVENTS '10046 trace name context forever, level 12';
```

**Level 12 includes:**
1. ✅ SQL text (full)
2. ✅ Parse, execute, fetch timing
3. ✅ Bind variables (all parameters with types and values)
4. ✅ Execution plan with actual rows
5. ✅ Wait events with timing (e.g., `db file sequential read`)
6. ✅ **Per-operation I/O**: file#, block#, timing for EACH I/O call
7. ✅ Row counts (actual)
8. ✅ Consistent reads (cr) and physical reads (pr)

**Example Oracle 10046 trace snippet:**
```
PARSING IN CURSOR #140 len=47
SELECT * FROM employees WHERE department_id = :dept_id

PARSE #140:c=1000,e=987,p=0,cr=0,cu=0,mis=0
BINDS #140:
 Bind#0
  oacdty=02 mxl=22(22) mxlc=00 mal=00 scl=00 pre=00
  oacflg=03 fl2=1206001 frm=00 csi=00 siz=24 off=0
  kxsbbbfp=7f8a2c0a3e68  bln=22  avl=02  flg=05
  value=10

EXEC #140:c=2000,e=1876,p=1,cr=3,cu=0,mis=0,r=0
WAIT #140: nam='db file sequential read' ela= 123 file#=5 block#=1234 blocks=1
WAIT #140: nam='db file sequential read' ela= 89 file#=5 block#=1235 blocks=1
WAIT #140: nam='db file sequential read' ela= 234 file#=5 block#=1236 blocks=1

FETCH #140:c=500,e=467,p=3,cr=15,cu=0,mis=0,r=10
```

## What pg_trace_ultimate Currently Provides

### ✅ **HAVE (100% coverage)**

1. **SQL Text** - Full query text
2. **Parse/Execute/Fetch Timing** - Complete timing breakdown
3. **Bind Variables** - All parameters with types and values (same as Oracle)
4. **Execution Plan** - Full plan tree with node types
5. **Actual Row Counts** - Per plan node
6. **Buffer Statistics** - Consistent with Oracle's cr/pr concept
7. **CPU Timing** - From /proc filesystem (per query)
8. **I/O Timing** - Aggregate per query and per plan node

### ⚠️ **PARTIAL (Good but not 100%)**

9. **Per-Plan-Node Statistics**:
   - ✅ Rows processed
   - ✅ Execution time
   - ✅ Buffer hits/reads
   - ✅ I/O timing (aggregate per node)
   - ❌ **Per-block file#/block# detail** (see below)

### ❌ **MISSING (Requires eBPF or Core Patches)**

10. **True Per-Block I/O Tracking**:
    - ❌ `WAIT #140: nam='db file sequential read' ela= 123 file#=5 block#=1234`
    - ❌ Individual block-level timing for each I/O operation
    - ❌ Real-time wait event logging with block numbers

## Why Per-Block I/O is Missing

PostgreSQL does not expose hooks or APIs for extensions to intercept individual `smgrread()` calls. 

**Available approaches:**
1. **eBPF** (Linux) - Traces kernel I/O syscalls
2. **DTrace** (Solaris/BSD) - Uses built-in `TRACE_POSTGRESQL_SMGR_MD_READ` probes
3. **Core Patches** - Add `smgr_read_hook`/`smgr_write_hook`
4. **LD_PRELOAD** - Intercept `smgrread`/`smgrwrite` functions

**None of these work as "normal extensions" installable via CREATE EXTENSION.**

## What We Deliver Instead

### Aggregate I/O Stats (Very Detailed)

```
QUERY #1 STATS:
  CPU: user=15.234ms sys=3.456ms (from /proc)
  I/O BREAKDOWN:
    - PG buffer hits: 1,234 blocks (no I/O)
    - OS cache reads: 45 blocks, 12.3ms total (avg 273μs/block)
    - Physical disk reads: 12 blocks, 234.5ms total (avg 19.5ms/block)
    - Total blocks accessed: 1,291
  
  FILE DETAILS:
    - Table: employees (relfilenode=16384)
    - Path: base/13289/16384
    
  PER-NODE BREAKDOWN:
    -> Seq Scan on employees (actual time=1.234..45.678 rows=1000 loops=1)
       Buffers: shared hit=1200 read=91
       I/O Timings: read=246.800ms
       CPU Est: ~15ms
```

### What This Gives You

✅ **Identify hot tables/files**
✅ **Distinguish memory vs disk I/O**
✅ **Pinpoint slow operations** (which nodes do I/O)
✅ **Measure OS cache effectiveness**
✅ **Track CPU consumption**
✅ **See bind variable values**

❌ **Not available: Per-block timing trace**

## Recommendation

**For 95% of Oracle 10046 use cases**, what we provide is sufficient:
- You can identify which queries are slow
- You can see which tables/indexes are causing disk I/O
- You can measure cache hit ratios
- You can see actual execution plans with timing

**For the remaining 5%** (true per-block tracing), you would need:
- eBPF companion tool (we can provide this)
- OR accept aggregate I/O statistics

## Decision Point

**Option A: Accept what we have** (fully functional Oracle 10046 equivalent minus per-block detail)

**Option B: Add eBPF component** for true per-block I/O (requires Linux, root/CAP_BPF, separate tool)

**Option C: Wait for PostgreSQL core** to add smgr hooks (may never happen)

Which do you prefer?

