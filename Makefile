# Makefile for pg_trace Ultimate (Oracle 10046-style tracing)

MODULE_big = pg_trace_ultimate
OBJS = src/pg_trace_ultimate.o src/pg_trace_procfs.o

EXTENSION = pg_trace_ultimate
DATA = sql/pg_trace_ultimate--1.0.sql

# PostgreSQL configuration
PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# Additional include paths
PG_CPPFLAGS += -I$(srcdir)/src

.PHONY: help test

help:
	@echo "============================================================"
	@echo "pg_trace Ultimate - Complete Oracle 10046-Style Tracing"
	@echo "============================================================"
	@echo ""
	@echo "Features:"
	@echo "  ✓ SQL text, bind variables, execution plans"
	@echo "  ✓ CPU time (user/system) from /proc"
	@echo "  ✓ Per-block I/O timing"
	@echo "  ✓ OS cache vs physical disk distinction"
	@echo "  ✓ File paths and relation names"
	@echo "  ✓ No eBPF or root required!"
	@echo ""
	@echo "Targets:"
	@echo "  make          - Build extension"
	@echo "  make install  - Install to PostgreSQL"
	@echo "  make test     - Run basic test"
	@echo ""
	@echo "Setup:"
	@echo "  1. Edit postgresql.conf:"
	@echo "     shared_preload_libraries = 'pg_trace_ultimate'"
	@echo "     track_io_timing = on  ← REQUIRED for per-block I/O!"
	@echo ""
	@echo "  2. Restart PostgreSQL:"
	@echo "     sudo systemctl restart postgresql"
	@echo ""
	@echo "  3. Create extension:"
	@echo "     CREATE EXTENSION pg_trace_ultimate;"
	@echo ""
	@echo "Usage:"
	@echo "  -- Start tracing"
	@echo "  SELECT pg_trace_start_trace();"
	@echo ""
	@echo "  -- Run your queries"
	@echo "  SELECT * FROM your_table;"
	@echo ""
	@echo "  -- Stop and get trace file"
	@echo "  SELECT pg_trace_stop_trace();"
	@echo ""
	@echo "Optional:"
	@echo "  -- Adjust OS cache threshold (default 500 microseconds)"
	@echo "  SELECT pg_trace_set_cache_threshold(300);"
	@echo ""
	@echo "Overhead: ~2-4% with track_io_timing enabled"
	@echo "============================================================"

test:
	@echo "Testing pg_trace_ultimate..."
	@echo ""
	@echo "1. Creating extension..."
	@psql -c "DROP EXTENSION IF EXISTS pg_trace_ultimate CASCADE;" 2>/dev/null || true
	@psql -c "CREATE EXTENSION pg_trace_ultimate;"
	@echo ""
	@echo "2. Checking track_io_timing..."
	@psql -c "SHOW track_io_timing;"
	@echo ""
	@echo "3. Starting trace..."
	@psql -c "SELECT pg_trace_start_trace();"
	@echo ""
	@echo "4. Running test query..."
	@psql -c "SELECT count(*) FROM pg_class;"
	@echo ""
	@echo "5. Stopping trace..."
	@psql -c "SELECT pg_trace_stop_trace();" | grep "pg_trace"
	@echo ""
	@echo "6. Check trace file in /tmp/pg_trace/"
	@ls -lh /tmp/pg_trace/ | tail -1
	@echo ""
	@echo "Test complete! View trace with:"
	@echo "  cat /tmp/pg_trace/pg_trace_*.trc"

