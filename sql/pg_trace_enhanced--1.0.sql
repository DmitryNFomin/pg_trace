/* pg_trace_enhanced--1.0.sql */

\echo Use "CREATE EXTENSION pg_trace_enhanced" to load this file. \quit

-- Same interface as MVP, but with OS statistics
CREATE FUNCTION pg_trace_start_trace()
RETURNS text
AS 'MODULE_PATHNAME', 'pg_trace_start_trace'
LANGUAGE C STRICT;

CREATE FUNCTION pg_trace_stop_trace()
RETURNS text
AS 'MODULE_PATHNAME', 'pg_trace_stop_trace'
LANGUAGE C STRICT;

CREATE FUNCTION pg_trace_get_tracefile()
RETURNS text
AS 'MODULE_PATHNAME', 'pg_trace_get_tracefile'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_trace_start_trace() IS 'Start tracing with OS-level CPU and I/O statistics';
COMMENT ON FUNCTION pg_trace_stop_trace() IS 'Stop tracing and return trace file path';
COMMENT ON FUNCTION pg_trace_get_tracefile() IS 'Get current trace file path';

