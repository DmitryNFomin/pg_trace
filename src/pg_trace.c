/*-------------------------------------------------------------------------
 *
 * pg_trace.c
 *    PostgreSQL extension for Oracle 10046-style session tracing
 *
 * This extension provides comprehensive per-session tracing similar to
 * Oracle's 10046 trace, including:
 * - SQL statement execution with timing
 * - Parse, bind, execute, and fetch phases
 * - Wait events tracking
 * - Buffer I/O statistics per operation
 * - Row counts and execution statistics
 * - Detailed plan execution with per-node statistics
 *
 * Copyright (c) 2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    pg_trace/src/pg_trace.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

#include "access/xact.h"
#include "catalog/pg_authid.h"
#include "commands/explain.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "optimizer/planner.h"
#include "pgstat.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

#include "../include/pg_trace.h"

PG_MODULE_MAGIC;

/*---- Module callbacks ----*/
void _PG_init(void);
void _PG_fini(void);

/*---- GUC variables ----*/
static int trace_level = 0;                      /* 0=off, 1=basic, 4=binds, 8=waits, 12=full */
static char *trace_file_directory = NULL;
static int trace_buffer_size = 1000;
static bool trace_waits = true;
static bool trace_bind_variables = true;
static bool trace_buffer_stats = true;
static int trace_file_max_size = 10 * 1024;      /* 10MB default */

/*---- Local state ----*/
static bool trace_enabled_this_session = false;
static FILE *trace_file_handle = NULL;
static char trace_filename[MAXPGPATH];
static int64 trace_event_sequence = 0;
static TimestampTz session_start_time;

/* Query execution context */
typedef struct QueryTraceContext
{
    uint64 query_id;
    char sql_id[16];
    TimestampTz start_time;
    TimestampTz parse_time;
    TimestampTz plan_time;
    TimestampTz bind_time;
    TimestampTz exec_start_time;
    BufferUsage buffer_usage_start;
    WalUsage wal_usage_start;
    int64 rows_fetched;
    uint32 wait_event_info;
    TimestampTz last_wait_start;
    double total_wait_time;
    List *wait_events;                            /* List of wait event records */
} QueryTraceContext;

static QueryTraceContext *current_query_context = NULL;

/* Wait event record */
typedef struct WaitEventRecord
{
    char *wait_event_name;
    uint32 wait_event_info;
    TimestampTz start_time;
    TimestampTz end_time;
    double duration_ms;
    int64 p1;                                     /* Wait event parameter 1 */
    int64 p2;                                     /* Wait event parameter 2 */
    int64 p3;                                     /* Wait event parameter 3 */
} WaitEventRecord;

/*---- Saved hook values ----*/
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static planner_hook_type prev_planner_hook = NULL;
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;

/*---- Shared memory state ----*/
PgTraceSharedState *pg_trace_shared_state = NULL;

/*---- Forward declarations ----*/
static void pg_trace_shmem_startup(void);
static Size pg_trace_memsize(void);
static void trace_write(const char *fmt, ...) pg_attribute_printf(1, 2);
static void trace_write_header(void);
static void trace_write_query_start(QueryDesc *queryDesc);
static void trace_write_query_end(QueryDesc *queryDesc, QueryTraceContext *ctx);
static void trace_write_plan_node(PlanState *planstate, int level, QueryTraceContext *ctx);
static void trace_write_wait_event(WaitEventRecord *wait_event);
static void trace_write_buffer_stats(BufferUsage *start, BufferUsage *end, const char *operation);
static char *get_wait_event_name(uint32 wait_event_info);
static char *generate_sql_id(const char *query_text);
static void open_trace_file(void);
static void close_trace_file(void);
static void recurse_plan_tree(PlanState *planstate, int level, QueryTraceContext *ctx);

/* Hook implementations */
static PlannedStmt *pg_trace_planner_hook(Query *parse, const char *query_string,
                                          int cursorOptions, ParamListInfo boundParams);
static void pg_trace_ExecutorStart_hook(QueryDesc *queryDesc, int eflags);
static void pg_trace_ExecutorRun_hook(QueryDesc *queryDesc, ScanDirection direction,
                                      uint64 count, bool execute_once);
static void pg_trace_ExecutorFinish_hook(QueryDesc *queryDesc);
static void pg_trace_ExecutorEnd_hook(QueryDesc *queryDesc);

/* SQL functions */
PG_FUNCTION_INFO_V1(pg_trace_session_trace_enable);
PG_FUNCTION_INFO_V1(pg_trace_session_trace_disable);
PG_FUNCTION_INFO_V1(pg_trace_set_level);
PG_FUNCTION_INFO_V1(pg_trace_get_tracefile);

/*
 * Module load callback
 */
void
_PG_init(void)
{
    if (!process_shared_preload_libraries_in_progress)
    {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("pg_trace must be loaded via shared_preload_libraries")));
    }

    /* Define custom GUC variables */
    DefineCustomIntVariable("pg_trace.trace_level",
                            "Sets the tracing level (0=off, 1=basic, 4=binds, 8=waits, 12=full)",
                            "Similar to Oracle 10046 trace levels",
                            &trace_level,
                            0,
                            0, 16,
                            PGC_USERSET,
                            0,
                            NULL, NULL, NULL);

    DefineCustomStringVariable("pg_trace.trace_file_directory",
                               "Directory where trace files are written",
                               NULL,
                               &trace_file_directory,
                               "/tmp",
                               PGC_SUSET,
                               0,
                               NULL, NULL, NULL);

    DefineCustomIntVariable("pg_trace.trace_buffer_size",
                            "Size of the trace event buffer",
                            NULL,
                            &trace_buffer_size,
                            1000,
                            100, 100000,
                            PGC_POSTMASTER,
                            0,
                            NULL, NULL, NULL);

    DefineCustomBoolVariable("pg_trace.trace_waits",
                             "Track wait events in trace",
                             NULL,
                             &trace_waits,
                             true,
                             PGC_USERSET,
                             0,
                             NULL, NULL, NULL);

    DefineCustomBoolVariable("pg_trace.trace_bind_variables",
                             "Include bind variables in trace",
                             NULL,
                             &trace_bind_variables,
                             true,
                             PGC_USERSET,
                             0,
                             NULL, NULL, NULL);

    DefineCustomBoolVariable("pg_trace.trace_buffer_stats",
                             "Include buffer I/O statistics in trace",
                             NULL,
                             &trace_buffer_stats,
                             true,
                             PGC_USERSET,
                             0,
                             NULL, NULL, NULL);

    DefineCustomIntVariable("pg_trace.trace_file_max_size",
                            "Maximum trace file size in KB",
                            NULL,
                            &trace_file_max_size,
                            10 * 1024,
                            1024, 1024 * 1024,
                            PGC_USERSET,
                            GUC_UNIT_KB,
                            NULL, NULL, NULL);

    /* Request shared memory */
    RequestAddinShmemSpace(pg_trace_memsize());
    RequestNamedLWLockTranche("pg_trace", 1);

    /* Install hooks */
    prev_shmem_startup_hook = shmem_startup_hook;
    shmem_startup_hook = pg_trace_shmem_startup;

    prev_planner_hook = planner_hook;
    planner_hook = pg_trace_planner_hook;

    prev_ExecutorStart = ExecutorStart_hook;
    ExecutorStart_hook = pg_trace_ExecutorStart_hook;

    prev_ExecutorRun = ExecutorRun_hook;
    ExecutorRun_hook = pg_trace_ExecutorRun_hook;

    prev_ExecutorFinish = ExecutorFinish_hook;
    ExecutorFinish_hook = pg_trace_ExecutorFinish_hook;

    prev_ExecutorEnd = ExecutorEnd_hook;
    ExecutorEnd_hook = pg_trace_ExecutorEnd_hook;

    session_start_time = GetCurrentTimestamp();
}

/*
 * Module unload callback
 */
void
_PG_fini(void)
{
    /* Restore hooks */
    shmem_startup_hook = prev_shmem_startup_hook;
    planner_hook = prev_planner_hook;
    ExecutorStart_hook = prev_ExecutorStart;
    ExecutorRun_hook = prev_ExecutorRun;
    ExecutorFinish_hook = prev_ExecutorFinish;
    ExecutorEnd_hook = prev_ExecutorEnd;

    /* Close trace file if open */
    close_trace_file();
}

/*
 * Estimate shared memory space needed
 */
static Size
pg_trace_memsize(void)
{
    Size size;

    size = MAXALIGN(sizeof(PgTraceSharedState));
    size = add_size(size, mul_size(trace_buffer_size, sizeof(TraceEvent)));

    return size;
}

/*
 * Initialize shared memory
 */
static void
pg_trace_shmem_startup(void)
{
    bool found;

    if (prev_shmem_startup_hook)
        prev_shmem_startup_hook();

    /* Reset in case this is a restart within the postmaster */
    pg_trace_shared_state = NULL;

    /* Create or attach to the shared memory state */
    LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

    pg_trace_shared_state = ShmemInitStruct("pg_trace",
                                            pg_trace_memsize(),
                                            &found);

    if (!found)
    {
        /* First time through ... */
        pg_trace_shared_state->lock = &(GetNamedLWLockTranche("pg_trace")->lock);
        SpinLockInit(&pg_trace_shared_state->mutex);

        pg_trace_shared_state->trace_buffer = (TraceEvent *)
            ShmemAlloc(trace_buffer_size * sizeof(TraceEvent));
        pg_trace_shared_state->trace_buffer_size = trace_buffer_size;
        pg_trace_shared_state->trace_write_pos = 0;
        pg_trace_shared_state->trace_read_pos = 0;

        pg_trace_shared_state->total_events = 0;
        pg_trace_shared_state->dropped_events = 0;
        pg_trace_shared_state->active_queries = 0;

        pg_trace_shared_state->trace_level = 0;
        pg_trace_shared_state->enable_sql_monitor = false;
        pg_trace_shared_state->enable_plan_viz = false;
    }

    LWLockRelease(AddinShmemInitLock);
}

/*
 * Generate SQL ID (similar to Oracle's SQL_ID)
 * Uses a hash of the query text
 */
static char *
generate_sql_id(const char *query_text)
{
    uint64 hash;
    char *sql_id;

    if (!query_text)
        return pstrdup("0000000000000");

    hash = DatumGetUInt64(hash_any((unsigned char *) query_text, strlen(query_text)));

    sql_id = palloc(14);
    snprintf(sql_id, 14, "%013lx", (unsigned long) hash);

    return sql_id;
}

/*
 * Get wait event name from wait_event_info
 */
static char *
get_wait_event_name(uint32 wait_event_info)
{
    const char *wait_name;

    if (wait_event_info == 0)
        return pstrdup("CPU");

    /* Get wait event name from pgstat */
    wait_name = pgstat_get_wait_event(wait_event_info);
    
    if (wait_name)
        return pstrdup(wait_name);
    
    return psprintf("WAIT:0x%08x", wait_event_info);
}

/*
 * Open trace file for current session
 */
static void
open_trace_file(void)
{
    struct stat st;

    if (trace_file_handle != NULL)
        return;

    /* Create trace filename: <dir>/pg_trace_<pid>_<timestamp>.trc */
    snprintf(trace_filename, MAXPGPATH, "%s/pg_trace_%d_%ld.trc",
             trace_file_directory,
             MyProcPid,
             (long) time(NULL));

    trace_file_handle = fopen(trace_filename, "w");
    
    if (trace_file_handle == NULL)
    {
        ereport(WARNING,
                (errcode_for_file_access(),
                 errmsg("could not open trace file \"%s\": %m", trace_filename)));
        return;
    }

    /* Make trace file unbuffered for real-time viewing */
    setvbuf(trace_file_handle, NULL, _IONBF, 0);

    trace_write_header();
}

/*
 * Close trace file
 */
static void
close_trace_file(void)
{
    if (trace_file_handle != NULL)
    {
        TimestampTz now = GetCurrentTimestamp();
        long secs;
        int microsecs;

        TimestampDifference(session_start_time, now, &secs, &microsecs);

        trace_write("*** SESSION END at %s\n",
                    timestamptz_to_str(now));
        trace_write("*** Total session duration: %ld.%06d seconds\n",
                    secs, microsecs);
        trace_write("*** Trace file closed\n");

        fclose(trace_file_handle);
        trace_file_handle = NULL;
    }
}

/*
 * Write trace header (similar to Oracle 10046)
 */
static void
trace_write_header(void)
{
    trace_write("***********************************************************************\n");
    trace_write("*** PostgreSQL Session Trace (10046-style)\n");
    trace_write("*** Trace File: %s\n", trace_filename);
    trace_write("*** Session Start: %s\n", timestamptz_to_str(session_start_time));
    trace_write("*** Process ID: %d\n", MyProcPid);
    trace_write("*** User: %s\n", GetUserNameFromId(GetUserId(), false));
    trace_write("*** Database: %s\n", get_database_name(MyDatabaseId));
    trace_write("*** Trace Level: %d\n", trace_level);
    trace_write("*** Options: waits=%s binds=%s buffers=%s\n",
                trace_waits ? "true" : "false",
                trace_bind_variables ? "true" : "false",
                trace_buffer_stats ? "true" : "false");
    trace_write("***********************************************************************\n");
    trace_write("\n");
}

/*
 * Write formatted output to trace file
 */
static void
trace_write(const char *fmt, ...)
{
    va_list args;
    char buffer[8192];

    if (trace_file_handle == NULL)
        return;

    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    fputs(buffer, trace_file_handle);
}

/*
 * Write buffer statistics (similar to Oracle 10046 wait events)
 */
static void
trace_write_buffer_stats(BufferUsage *start, BufferUsage *end, const char *operation)
{
    BufferUsage diff;

    if (!trace_buffer_stats)
        return;

    /* Calculate differences */
    diff.shared_blks_hit = end->shared_blks_hit - start->shared_blks_hit;
    diff.shared_blks_read = end->shared_blks_read - start->shared_blks_read;
    diff.shared_blks_dirtied = end->shared_blks_dirtied - start->shared_blks_dirtied;
    diff.shared_blks_written = end->shared_blks_written - start->shared_blks_written;
    diff.local_blks_hit = end->local_blks_hit - start->local_blks_hit;
    diff.local_blks_read = end->local_blks_read - start->local_blks_read;
    diff.local_blks_dirtied = end->local_blks_dirtied - start->local_blks_dirtied;
    diff.local_blks_written = end->local_blks_written - start->local_blks_written;
    diff.temp_blks_read = end->temp_blks_read - start->temp_blks_read;
    diff.temp_blks_written = end->temp_blks_written - start->temp_blks_written;

    if (diff.shared_blks_hit > 0 || diff.shared_blks_read > 0 ||
        diff.shared_blks_dirtied > 0 || diff.shared_blks_written > 0 ||
        diff.local_blks_hit > 0 || diff.local_blks_read > 0 ||
        diff.temp_blks_read > 0 || diff.temp_blks_written > 0)
    {
        trace_write("BUFFER STATS: %s\n", operation);
        trace_write("  shared blocks: hit=%ld read=%ld dirtied=%ld written=%ld\n",
                    diff.shared_blks_hit, diff.shared_blks_read,
                    diff.shared_blks_dirtied, diff.shared_blks_written);
        trace_write("  local blocks:  hit=%ld read=%ld dirtied=%ld written=%ld\n",
                    diff.local_blks_hit, diff.local_blks_read,
                    diff.local_blks_dirtied, diff.local_blks_written);
        trace_write("  temp blocks:   read=%ld written=%ld\n",
                    diff.temp_blks_read, diff.temp_blks_written);
    }
}

/*
 * Planner hook - track planning time
 */
static PlannedStmt *
pg_trace_planner_hook(Query *parse, const char *query_string,
                      int cursorOptions, ParamListInfo boundParams)
{
    PlannedStmt *result;
    TimestampTz start_time, end_time;
    long secs;
    int microsecs;

    if (!trace_enabled_this_session || trace_level < TRACE_LEVEL_BASIC)
    {
        if (prev_planner_hook)
            return prev_planner_hook(parse, query_string, cursorOptions, boundParams);
        else
            return standard_planner(parse, query_string, cursorOptions, boundParams);
    }

    start_time = GetCurrentTimestamp();

    /* Call the standard or previous planner */
    if (prev_planner_hook)
        result = prev_planner_hook(parse, query_string, cursorOptions, boundParams);
    else
        result = standard_planner(parse, query_string, cursorOptions, boundParams);

    end_time = GetCurrentTimestamp();
    TimestampDifference(start_time, end_time, &secs, &microsecs);

    /* Write parse/plan phase */
    trace_write("=====================================================================\n");
    trace_write("PARSE #%lld\n", (long long) ++trace_event_sequence);
    trace_write("SQL: %s\n", query_string ? query_string : "<null>");
    trace_write("SQL_ID: %s\n", generate_sql_id(query_string));
    trace_write("PARSE TIME: %ld.%06d seconds\n", secs, microsecs);
    trace_write("---------------------------------------------------------------------\n");

    return result;
}

/*
 * ExecutorStart hook
 */
static void
pg_trace_ExecutorStart_hook(QueryDesc *queryDesc, int eflags)
{
    /* Call previous hook or standard function */
    if (prev_ExecutorStart)
        prev_ExecutorStart(queryDesc, eflags);
    else
        standard_ExecutorStart(queryDesc, eflags);

    if (!trace_enabled_this_session || trace_level < TRACE_LEVEL_BASIC)
        return;

    /* Enable instrumentation for detailed statistics */
    if (queryDesc->estate)
    {
        queryDesc->estate->es_instrument = INSTRUMENT_ALL;
    }

    /* Create query context */
    current_query_context = (QueryTraceContext *) palloc0(sizeof(QueryTraceContext));
    current_query_context->start_time = GetCurrentTimestamp();
    current_query_context->query_id = queryDesc->plannedstmt->queryId;
    
    if (queryDesc->sourceText)
    {
        char *sql_id = generate_sql_id(queryDesc->sourceText);
        strncpy(current_query_context->sql_id, sql_id, sizeof(current_query_context->sql_id) - 1);
        pfree(sql_id);
    }

    /* Capture starting buffer usage */
    current_query_context->buffer_usage_start = pgBufferUsage;
    current_query_context->wal_usage_start = pgWalUsage;

    trace_write_query_start(queryDesc);
}

/*
 * Write query start trace
 */
static void
trace_write_query_start(QueryDesc *queryDesc)
{
    trace_write("=====================================================================\n");
    trace_write("EXEC #%lld\n", (long long) ++trace_event_sequence);
    trace_write("SQL: %s\n", queryDesc->sourceText ? queryDesc->sourceText : "<null>");
    trace_write("SQL_ID: %s\n", current_query_context->sql_id);
    trace_write("QUERY_ID: %lld\n", (long long) current_query_context->query_id);
    trace_write("START TIME: %s\n", timestamptz_to_str(current_query_context->start_time));

    /* Write bind variables if enabled */
    if (trace_bind_variables && trace_level >= TRACE_LEVEL_BIND && queryDesc->params)
    {
        ParamListInfo params = queryDesc->params;
        int i;

        trace_write("BIND VARIABLES:\n");
        for (i = 0; i < params->numParams; i++)
        {
            ParamExternData *param = &params->params[i];
            
            if (!param->isnull)
            {
                Oid typoutput;
                bool typIsVarlena;
                char *val_str;

                getTypeOutputInfo(param->ptype, &typoutput, &typIsVarlena);
                val_str = OidOutputFunctionCall(typoutput, param->value);

                trace_write("  BIND #%d: type=%u value=%s\n", i + 1, param->ptype, val_str);
                pfree(val_str);
            }
            else
            {
                trace_write("  BIND #%d: type=%u value=NULL\n", i + 1, param->ptype);
            }
        }
    }

    trace_write("---------------------------------------------------------------------\n");
}

/*
 * ExecutorRun hook
 */
static void
pg_trace_ExecutorRun_hook(QueryDesc *queryDesc, ScanDirection direction,
                          uint64 count, bool execute_once)
{
    TimestampTz exec_start;
    
    if (trace_enabled_this_session && trace_level >= TRACE_LEVEL_BASIC && current_query_context)
    {
        exec_start = GetCurrentTimestamp();
        current_query_context->exec_start_time = exec_start;
    }

    /* Call previous hook or standard function */
    if (prev_ExecutorRun)
        prev_ExecutorRun(queryDesc, direction, count, execute_once);
    else
        standard_ExecutorRun(queryDesc, direction, count, execute_once);

    if (trace_enabled_this_session && trace_level >= TRACE_LEVEL_BASIC && current_query_context)
    {
        TimestampTz exec_end = GetCurrentTimestamp();
        long secs;
        int microsecs;

        TimestampDifference(exec_start, exec_end, &secs, &microsecs);
        
        trace_write("FETCH: rows=%lld time=%ld.%06d sec\n",
                    (long long) queryDesc->estate->es_processed,
                    secs, microsecs);
    }
}

/*
 * ExecutorFinish hook
 */
static void
pg_trace_ExecutorFinish_hook(QueryDesc *queryDesc)
{
    /* Call previous hook or standard function */
    if (prev_ExecutorFinish)
        prev_ExecutorFinish(queryDesc);
    else
        standard_ExecutorFinish(queryDesc);
}

/*
 * ExecutorEnd hook
 */
static void
pg_trace_ExecutorEnd_hook(QueryDesc *queryDesc)
{
    if (trace_enabled_this_session && trace_level >= TRACE_LEVEL_BASIC && current_query_context)
    {
        trace_write_query_end(queryDesc, current_query_context);

        /* Write detailed plan statistics if requested */
        if (trace_level >= TRACE_LEVEL_PLAN && queryDesc->planstate)
        {
            trace_write("\n");
            trace_write("EXECUTION PLAN WITH STATISTICS:\n");
            trace_write("=================================================================\n");
            recurse_plan_tree(queryDesc->planstate, 0, current_query_context);
        }

        /* Cleanup */
        if (current_query_context->wait_events)
            list_free_deep(current_query_context->wait_events);
        pfree(current_query_context);
        current_query_context = NULL;
    }

    /* Call previous hook or standard function */
    if (prev_ExecutorEnd)
        prev_ExecutorEnd(queryDesc);
    else
        standard_ExecutorEnd(queryDesc);
}

/*
 * Write query end trace
 */
static void
trace_write_query_end(QueryDesc *queryDesc, QueryTraceContext *ctx)
{
    TimestampTz end_time = GetCurrentTimestamp();
    long secs;
    int microsecs;
    BufferUsage buffer_end = pgBufferUsage;
    WalUsage wal_end = pgWalUsage;

    TimestampDifference(ctx->start_time, end_time, &secs, &microsecs);

    trace_write("---------------------------------------------------------------------\n");
    trace_write("END EXEC #%lld\n", (long long) trace_event_sequence);
    trace_write("ROWS: %lld\n", (long long) queryDesc->estate->es_processed);
    trace_write("ELAPSED TIME: %ld.%06d seconds\n", secs, microsecs);

    /* Write buffer statistics */
    trace_write_buffer_stats(&ctx->buffer_usage_start, &buffer_end, "TOTAL QUERY");

    /* Write WAL statistics */
    if (wal_end.wal_records > ctx->wal_usage_start.wal_records)
    {
        trace_write("WAL STATS:\n");
        trace_write("  records: %ld\n", 
                    wal_end.wal_records - ctx->wal_usage_start.wal_records);
        trace_write("  fpi: %ld\n",
                    wal_end.wal_fpi - ctx->wal_usage_start.wal_fpi);
        trace_write("  bytes: %lld\n",
                    (long long) (wal_end.wal_bytes - ctx->wal_usage_start.wal_bytes));
    }

    trace_write("=====================================================================\n");
    trace_write("\n");
}

/*
 * Recursively write plan tree with statistics
 */
static void
recurse_plan_tree(PlanState *planstate, int level, QueryTraceContext *ctx)
{
    int i;
    char indent[256];

    if (planstate == NULL)
        return;

    /* Create indentation */
    for (i = 0; i < level && i < 127; i++)
        indent[i * 2] = indent[i * 2 + 1] = ' ';
    indent[level * 2] = '\0';

    trace_write_plan_node(planstate, level, ctx);

    /* Recurse to children */
    if (planstate->lefttree)
        recurse_plan_tree(planstate->lefttree, level + 1, ctx);
    
    if (planstate->righttree)
        recurse_plan_tree(planstate->righttree, level + 1, ctx);

    /* Handle special node types with multiple children */
    if (IsA(planstate, AppendState))
    {
        AppendState *appendstate = (AppendState *) planstate;
        for (i = 0; i < appendstate->as_nplans; i++)
            recurse_plan_tree(appendstate->appendplans[i], level + 1, ctx);
    }
    else if (IsA(planstate, MergeAppendState))
    {
        MergeAppendState *mergeappendstate = (MergeAppendState *) planstate;
        for (i = 0; i < mergeappendstate->ms_nplans; i++)
            recurse_plan_tree(mergeappendstate->mergeplans[i], level + 1, ctx);
    }
    else if (IsA(planstate, BitmapAndState))
    {
        BitmapAndState *bitmapandstate = (BitmapAndState *) planstate;
        for (i = 0; i < bitmapandstate->nplans; i++)
            recurse_plan_tree(bitmapandstate->bitmapplans[i], level + 1, ctx);
    }
    else if (IsA(planstate, BitmapOrState))
    {
        BitmapOrState *bitmaporstate = (BitmapOrState *) planstate;
        for (i = 0; i < bitmaporstate->nplans; i++)
            recurse_plan_tree(bitmaporstate->bitmapplans[i], level + 1, ctx);
    }
    else if (IsA(planstate, SubqueryScanState))
    {
        SubqueryScanState *subquerystate = (SubqueryScanState *) planstate;
        recurse_plan_tree(subquerystate->subplan, level + 1, ctx);
    }
}

/*
 * Write plan node statistics
 */
static void
trace_write_plan_node(PlanState *planstate, int level, QueryTraceContext *ctx)
{
    Instrumentation *instr = planstate->instrument;
    Plan *plan = planstate->plan;
    char indent[256];
    int i;

    /* Create indentation */
    for (i = 0; i < level && i < 127; i++)
        indent[i * 2] = indent[i * 2 + 1] = ' ';
    indent[level * 2] = '\0';

    trace_write("%s%s", indent, nodeTag(planstate));

    if (plan->plan_node_id > 0)
        trace_write(" (Node %d)", plan->plan_node_id);

    trace_write("\n");

    if (instr && instr->nloops > 0)
    {
        trace_write("%s  Rows: actual=%.0f planned=%.0f\n",
                    indent, instr->ntuples, plan->plan_rows);
        trace_write("%s  Loops: %.0f\n", indent, instr->nloops);
        trace_write("%s  Time: startup=%.3f total=%.3f (ms)\n",
                    indent, instr->startup * 1000.0, instr->total * 1000.0);

        if (instr->need_bufusage)
        {
            trace_write("%s  Buffers: shared hit=%ld read=%ld dirtied=%ld written=%ld\n",
                        indent,
                        instr->bufusage.shared_blks_hit,
                        instr->bufusage.shared_blks_read,
                        instr->bufusage.shared_blks_dirtied,
                        instr->bufusage.shared_blks_written);
            
            if (instr->bufusage.local_blks_hit > 0 || instr->bufusage.local_blks_read > 0)
            {
                trace_write("%s           local hit=%ld read=%ld dirtied=%ld written=%ld\n",
                            indent,
                            instr->bufusage.local_blks_hit,
                            instr->bufusage.local_blks_read,
                            instr->bufusage.local_blks_dirtied,
                            instr->bufusage.local_blks_written);
            }
            
            if (instr->bufusage.temp_blks_read > 0 || instr->bufusage.temp_blks_written > 0)
            {
                trace_write("%s           temp read=%ld written=%ld\n",
                            indent,
                            instr->bufusage.temp_blks_read,
                            instr->bufusage.temp_blks_written);
            }
        }

        if (instr->need_walusage && instr->walusage.wal_records > 0)
        {
            trace_write("%s  WAL: records=%ld fpi=%ld bytes=%lld\n",
                        indent,
                        instr->walusage.wal_records,
                        instr->walusage.wal_fpi,
                        (long long) instr->walusage.wal_bytes);
        }
    }
}

/*
 * SQL function: pg_trace_session_trace_enable()
 * Enable tracing for current session
 */
Datum
pg_trace_session_trace_enable(PG_FUNCTION_ARGS)
{
    if (!trace_enabled_this_session)
    {
        trace_enabled_this_session = true;
        open_trace_file();
        
        ereport(NOTICE,
                (errmsg("Session trace enabled"),
                 errdetail("Trace file: %s", trace_filename)));
    }

    PG_RETURN_BOOL(true);
}

/*
 * SQL function: pg_trace_session_trace_disable()
 * Disable tracing for current session
 */
Datum
pg_trace_session_trace_disable(PG_FUNCTION_ARGS)
{
    if (trace_enabled_this_session)
    {
        close_trace_file();
        trace_enabled_this_session = false;
        
        ereport(NOTICE,
                (errmsg("Session trace disabled")));
    }

    PG_RETURN_BOOL(true);
}

/*
 * SQL function: pg_trace_set_level(level integer)
 * Set trace level for current session
 */
Datum
pg_trace_set_level(PG_FUNCTION_ARGS)
{
    int new_level = PG_GETARG_INT32(0);

    if (new_level < 0 || new_level > 16)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("trace level must be between 0 and 16")));

    trace_level = new_level;

    if (trace_enabled_this_session && trace_file_handle)
    {
        trace_write("\n*** Trace level changed to %d at %s\n\n",
                    new_level, timestamptz_to_str(GetCurrentTimestamp()));
    }

    PG_RETURN_INT32(trace_level);
}

/*
 * SQL function: pg_trace_get_tracefile()
 * Get current trace filename
 */
Datum
pg_trace_get_tracefile(PG_FUNCTION_ARGS)
{
    if (trace_enabled_this_session && trace_filename[0] != '\0')
        PG_RETURN_TEXT_P(cstring_to_text(trace_filename));
    else
        PG_RETURN_NULL();
}

