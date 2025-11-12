/* pg_trace--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_trace" to load this file. \quit

-- Function to enable session tracing
CREATE FUNCTION pg_trace_session_trace_enable()
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_trace_session_trace_enable'
LANGUAGE C STRICT;

-- Function to disable session tracing
CREATE FUNCTION pg_trace_session_trace_disable()
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_trace_session_trace_disable'
LANGUAGE C STRICT;

-- Function to set trace level
CREATE FUNCTION pg_trace_set_level(level integer)
RETURNS integer
AS 'MODULE_PATHNAME', 'pg_trace_set_level'
LANGUAGE C STRICT;

-- Function to get current trace file name
CREATE FUNCTION pg_trace_get_tracefile()
RETURNS text
AS 'MODULE_PATHNAME', 'pg_trace_get_tracefile'
LANGUAGE C STRICT;

-- Convenience view for trace information
CREATE VIEW pg_trace_info AS
SELECT 
    current_setting('pg_trace.trace_level')::integer AS trace_level,
    current_setting('pg_trace.trace_file_directory') AS trace_file_directory,
    current_setting('pg_trace.trace_waits')::boolean AS trace_waits,
    current_setting('pg_trace.trace_bind_variables')::boolean AS trace_bind_variables,
    current_setting('pg_trace.trace_buffer_stats')::boolean AS trace_buffer_stats,
    pg_trace_get_tracefile() AS current_trace_file;

COMMENT ON EXTENSION pg_trace IS 'Oracle 10046-style session tracing for PostgreSQL';
COMMENT ON FUNCTION pg_trace_session_trace_enable() IS 'Enable tracing for the current session';
COMMENT ON FUNCTION pg_trace_session_trace_disable() IS 'Disable tracing for the current session';
COMMENT ON FUNCTION pg_trace_set_level(integer) IS 'Set trace level (0=off, 1=basic, 4=binds, 8=waits, 12=full)';
COMMENT ON FUNCTION pg_trace_get_tracefile() IS 'Get the current session trace file path';
COMMENT ON VIEW pg_trace_info IS 'Display current trace configuration';

