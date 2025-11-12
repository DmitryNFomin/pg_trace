# Quick Start Guide - pg_trace MVP

Get Oracle 10046-style traces in PostgreSQL in 5 minutes!

## Prerequisites Check

```bash
# Check PostgreSQL
psql --version

# Check Python3
python3 --version

# Check BCC (for eBPF)
python3 -c "import bcc" && echo "BCC OK" || echo "Need to install BCC"
```

## 1. Build and Install (2 minutes)

```bash
cd /Users/dmitryfomin/work/git/pg_trace

# Build
make -f Makefile.mvp clean
make -f Makefile.mvp

# Install (requires sudo)
sudo make -f Makefile.mvp install
```

## 2. Configure PostgreSQL (1 minute)

```bash
# Add to postgresql.conf
sudo bash -c "cat >> $(pg_config --sysconfdir)/postgresql.conf << EOF
shared_preload_libraries = 'pg_trace_mvp'
pg_trace.output_directory = '/tmp/pg_trace'
EOF"

# Create trace directory
sudo mkdir -p /tmp/pg_trace
sudo chmod 777 /tmp/pg_trace

# Restart PostgreSQL
sudo systemctl restart postgresql
# OR on macOS:
# brew services restart postgresql
```

## 3. Enable Extension (30 seconds)

```bash
# Connect to your database
psql -d your_database

# Create extension
CREATE EXTENSION pg_trace_mvp;

# Verify
\dx pg_trace_mvp
```

## 4. First Trace (1 minute)

### Option A: Basic Trace (No eBPF)

```sql
-- Start trace
SELECT pg_trace_start_trace();
-- Returns: /tmp/pg_trace/pg_trace_12345_1699186215.trc

-- Run your query
SELECT * FROM pg_class WHERE relkind = 'r';

-- Stop trace
SELECT pg_trace_stop_trace();

-- View it
\! cat /tmp/pg_trace/pg_trace_*.trc
```

### Option B: Full Trace with Wait Events

**Terminal 1** (psql):
```sql
-- Get your PID
SELECT pg_backend_pid();  -- e.g., 12345

-- Start trace
SELECT pg_trace_start_trace();
```

**Terminal 2** (root shell):
```bash
# Start wait event tracer
sudo python3 ebpf/pg_trace_waits.py -p 12345
```

**Terminal 1** (psql):
```sql
-- Run queries
SELECT count(*) FROM pg_class;
VACUUM ANALYZE pg_class;

-- Stop trace
SELECT pg_trace_stop_trace();
```

**Terminal 2**: Press Ctrl-C

**View trace**:
```bash
cat /tmp/pg_trace/pg_trace_12345_*.trc
```

## Example Output

You should see something like:

```
=====================================================================
PARSE #1
SQL: SELECT count(*) FROM pg_class
PARSE TIME: 2025-11-05 10:30:15.125 to 2025-11-05 10:30:15.127
---------------------------------------------------------------------
EXEC #1 at 2025-11-05 10:30:15.128
EXEC TIME: ela=0.012456 sec rows=1
---------------------------------------------------------------------
STATS #1:
  BUFFER STATS: cr=5 pr=0 pw=0 dirtied=0
---------------------------------------------------------------------
EXECUTION PLAN #1:
-> Aggregate [Node 1]
   Rows: planned=1 actual=1 loops=1
   Time: startup=0.123 total=1.234 ms
   Buffers: shared hit=5 read=0 dirtied=0 written=0
  -> SeqScan [Node 2]
     Rows: planned=400 actual=389 loops=1
     Time: startup=0.012 total=1.123 ms
     Buffers: shared hit=5 read=0 dirtied=0 written=0
=====================================================================
```

With eBPF, you'll also see:
```
WAIT #1: nam='DataFileRead' ela=156 us tim=2025-11-05 10:30:15.128123
```

## Troubleshooting

### "ERROR: extension not found"
```bash
# Check installation
ls $(pg_config --pkglibdir)/pg_trace_mvp.so

# If missing, reinstall:
sudo make -f Makefile.mvp install
```

### "ERROR: must be loaded via shared_preload_libraries"
```bash
# Check config
psql -c "SHOW shared_preload_libraries;"

# Should show: pg_trace_mvp

# If not, add to postgresql.conf and restart
```

### eBPF tracer errors
```bash
# Install BCC
# Ubuntu/Debian:
sudo apt-get install python3-bpfcc

# RHEL/Rocky:
sudo yum install python3-bcc

# Verify
python3 -c "import bcc; print('BCC OK')"
```

### No wait events
- Ensure eBPF tracer starts BEFORE queries run
- Check you're using correct PID
- Try a query that does I/O: `SELECT * FROM pg_class;`

## What's Next?

1. **Test with your workload** - Trace your actual queries
2. **Compare with EXPLAIN ANALYZE** - See the difference
3. **Analyze bottlenecks** - Look for expensive waits
4. **Optimize** - Use insights to improve queries

## Tips

- Trace files can be large - clean up `/tmp/pg_trace` regularly
- Stop traces when done - don't leave them running
- eBPF has minimal overhead - safe for production testing
- Start simple (no eBPF) then add wait tracing

## Need Help?

Check `README_MVP.md` for detailed documentation.

---

**You're ready!** Run your first trace and see PostgreSQL internals like never before. 🚀


