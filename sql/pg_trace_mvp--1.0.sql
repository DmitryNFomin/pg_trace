/* pg_trace_mvp--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_trace_mvp" to load this file. \quit

-- Enable tracing for current session
CREATE FUNCTION pg_trace_start_trace()
RETURNS text
AS 'MODULE_PATHNAME', 'pg_trace_start_trace'
LANGUAGE C STRICT;

-- Disable tracing for current session
CREATE FUNCTION pg_trace_stop_trace()
RETURNS text
AS 'MODULE_PATHNAME', 'pg_trace_stop_trace'
LANGUAGE C STRICT;

-- Get current trace file path
CREATE FUNCTION pg_trace_get_tracefile()
RETURNS text
AS 'MODULE_PATHNAME', 'pg_trace_get_tracefile'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_trace_start_trace() IS 'Start Oracle 10046-style tracing for current session';
COMMENT ON FUNCTION pg_trace_stop_trace() IS 'Stop tracing and return trace file path';
COMMENT ON FUNCTION pg_trace_get_tracefile() IS 'Get current trace file path';


