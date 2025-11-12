/* pg_trace_ultimate--1.0.sql */

\echo Use "CREATE EXTENSION pg_trace_ultimate" to load this file. \quit

-- Start tracing for current session
CREATE FUNCTION pg_trace_start_trace()
RETURNS text
AS 'MODULE_PATHNAME', 'pg_trace_start_trace'
LANGUAGE C STRICT;

-- Stop tracing
CREATE FUNCTION pg_trace_stop_trace()
RETURNS text
AS 'MODULE_PATHNAME', 'pg_trace_stop_trace'
LANGUAGE C STRICT;

-- Get current trace file path
CREATE FUNCTION pg_trace_get_tracefile()
RETURNS text
AS 'MODULE_PATHNAME', 'pg_trace_get_tracefile'
LANGUAGE C STRICT;

-- Set OS cache threshold (microseconds)
CREATE FUNCTION pg_trace_set_cache_threshold(threshold_us integer)
RETURNS integer
AS 'MODULE_PATHNAME', 'pg_trace_set_cache_threshold'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_trace_start_trace() IS 'Start Oracle 10046-style tracing with per-block I/O detail';
COMMENT ON FUNCTION pg_trace_stop_trace() IS 'Stop tracing and return trace file path';
COMMENT ON FUNCTION pg_trace_get_tracefile() IS 'Get current trace file path';
COMMENT ON FUNCTION pg_trace_set_cache_threshold(integer) IS 'Set threshold in microseconds to distinguish OS cache from disk (default 500)';

