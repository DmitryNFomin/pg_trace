/*-------------------------------------------------------------------------
 *
 * pg_trace_mvp.c
 *    MVP: Oracle 10046-style tracing with eBPF integration
 *
 * This is a minimal viable product that demonstrates:
 * - SQL text capture
 * - Bind variable logging
 * - Execution plan with statistics
 * - Integration point for eBPF wait event tracing
 *
 * Usage:
 *   SELECT pg_trace_start_trace();  -- Enable for your session
 *   -- Run your queries
 *   SELECT pg_trace_stop_trace();   -- Disable and show trace file
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "access/xact.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "optimizer/planner.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

PG_MODULE_MAGIC;

/* GUC variables */
static char *trace_output_directory = NULL;
static bool trace_enabled = false;

/* Per-session state */
static FILE *trace_file = NULL;
static char trace_filename[MAXPGPATH];
static int64 cursor_sequence = 0;
static TimestampTz session_start_time;

/* Current query context */
typedef struct CurrentQuery {
    int64 cursor_id;
    char *sql_text;
    TimestampTz parse_start;
    TimestampTz parse_end;
    TimestampTz exec_start;
    BufferUsage buffer_start;
} CurrentQuery;

static CurrentQuery *current_query = NULL;

/* Shared memory for eBPF coordination */
typedef struct PgTraceSharedState {
    LWLock *lock;
    
    /* Map PID to cursor_id for eBPF */
    struct {
        int pid;
        int64 cursor_id;
        bool active;
    } pid_map[100];  /* Simple fixed array for MVP */
    
    int next_slot;
} PgTraceSharedState;

static PgTraceSharedState *shared_state = NULL;

/* Saved hooks */
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static planner_hook_type prev_planner_hook = NULL;
static ExecutorStart_hook_type prev_ExecutorStart_hook = NULL;
static ExecutorRun_hook_type prev_ExecutorRun_hook = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd_hook = NULL;

/* Function declarations */
void _PG_init(void);
void _PG_fini(void);

static Size pg_trace_shmem_size(void);
static void pg_trace_shmem_startup(void);
static void trace_printf(const char *fmt, ...) pg_attribute_printf(1, 2);
static void write_plan_node(PlanState *planstate, int level);
static void write_plan_tree(PlanState *planstate, int level);
static void register_cursor_for_ebpf(int64 cursor_id);
static void unregister_cursor_for_ebpf(void);

/* Hook implementations */
static PlannedStmt *trace_planner(Query *parse, const char *query_string,
                                  int cursorOptions, ParamListInfo boundParams);
static void trace_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void trace_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction,
                              uint64 count, bool execute_once);
static void trace_ExecutorEnd(QueryDesc *queryDesc);

/* SQL functions */
PG_FUNCTION_INFO_V1(pg_trace_start_trace);
PG_FUNCTION_INFO_V1(pg_trace_stop_trace);
PG_FUNCTION_INFO_V1(pg_trace_get_tracefile);

/*
 * Module initialization
 */
void
_PG_init(void)
{
    if (!process_shared_preload_libraries_in_progress)
        return;

    /* GUC variables */
    DefineCustomStringVariable("pg_trace.output_directory",
                               "Directory for trace files",
                               NULL,
                               &trace_output_directory,
                               "/tmp/pg_trace",
                               PGC_SUSET,
                               0,
                               NULL, NULL, NULL);

    /* Shared memory */
    RequestAddinShmemSpace(pg_trace_shmem_size());
    RequestNamedLWLockTranche("pg_trace_mvp", 1);

    /* Install hooks */
    prev_shmem_startup_hook = shmem_startup_hook;
    shmem_startup_hook = pg_trace_shmem_startup;

    prev_planner_hook = planner_hook;
    planner_hook = trace_planner;

    prev_ExecutorStart_hook = ExecutorStart_hook;
    ExecutorStart_hook = trace_ExecutorStart;

    prev_ExecutorRun_hook = ExecutorRun_hook;
    ExecutorRun_hook = trace_ExecutorRun;

    prev_ExecutorEnd_hook = ExecutorEnd_hook;
    ExecutorEnd_hook = trace_ExecutorEnd;

    session_start_time = GetCurrentTimestamp();
}

void
_PG_fini(void)
{
    shmem_startup_hook = prev_shmem_startup_hook;
    planner_hook = prev_planner_hook;
    ExecutorStart_hook = prev_ExecutorStart_hook;
    ExecutorRun_hook = prev_ExecutorRun_hook;
    ExecutorEnd_hook = prev_ExecutorEnd_hook;
}

/*
 * Shared memory setup
 */
static Size
pg_trace_shmem_size(void)
{
    return MAXALIGN(sizeof(PgTraceSharedState));
}

static void
pg_trace_shmem_startup(void)
{
    bool found;

    if (prev_shmem_startup_hook)
        prev_shmem_startup_hook();

    LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

    shared_state = ShmemInitStruct("pg_trace_mvp",
                                   pg_trace_shmem_size(),
                                   &found);

    if (!found)
    {
        shared_state->lock = &(GetNamedLWLockTranche("pg_trace_mvp")->lock);
        shared_state->next_slot = 0;
        memset(shared_state->pid_map, 0, sizeof(shared_state->pid_map));
    }

    LWLockRelease(AddinShmemInitLock);
}

/*
 * Printf to trace file
 */
static void
trace_printf(const char *fmt, ...)
{
    va_list args;
    char buffer[8192];

    if (!trace_file)
        return;

    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    fputs(buffer, trace_file);
    fflush(trace_file);  /* Real-time output */
}

/*
 * Register current cursor with eBPF (via shared memory)
 */
static void
register_cursor_for_ebpf(int64 cursor_id)
{
    int i;

    if (!shared_state)
        return;

    LWLockAcquire(shared_state->lock, LW_EXCLUSIVE);

    /* Find a slot */
    for (i = 0; i < 100; i++)
    {
        if (!shared_state->pid_map[i].active)
        {
            shared_state->pid_map[i].pid = MyProcPid;
            shared_state->pid_map[i].cursor_id = cursor_id;
            shared_state->pid_map[i].active = true;
            break;
        }
    }

    LWLockRelease(shared_state->lock);
}

static void
unregister_cursor_for_ebpf(void)
{
    int i;

    if (!shared_state)
        return;

    LWLockAcquire(shared_state->lock, LW_EXCLUSIVE);

    for (i = 0; i < 100; i++)
    {
        if (shared_state->pid_map[i].active &&
            shared_state->pid_map[i].pid == MyProcPid)
        {
            shared_state->pid_map[i].active = false;
            break;
        }
    }

    LWLockRelease(shared_state->lock);
}

/*
 * Planner hook - capture parse/plan phase
 */
static PlannedStmt *
trace_planner(Query *parse, const char *query_string,
              int cursorOptions, ParamListInfo boundParams)
{
    PlannedStmt *result;
    TimestampTz start, end;

    if (!trace_enabled || !query_string)
    {
        if (prev_planner_hook)
            return prev_planner_hook(parse, query_string, cursorOptions, boundParams);
        else
            return standard_planner(parse, query_string, cursorOptions, boundParams);
    }

    /* Allocate query context */
    current_query = (CurrentQuery *) MemoryContextAllocZero(TopMemoryContext,
                                                             sizeof(CurrentQuery));
    current_query->cursor_id = ++cursor_sequence;
    current_query->sql_text = MemoryContextStrdup(TopMemoryContext, query_string);

    start = GetCurrentTimestamp();

    if (prev_planner_hook)
        result = prev_planner_hook(parse, query_string, cursorOptions, boundParams);
    else
        result = standard_planner(parse, query_string, cursorOptions, boundParams);

    end = GetCurrentTimestamp();

    current_query->parse_start = start;
    current_query->parse_end = end;

    /* Write PARSE section */
    trace_printf("=====================================================================\n");
    trace_printf("PARSE #%lld\n", (long long) current_query->cursor_id);
    trace_printf("SQL: %s\n", query_string);
    trace_printf("PARSE TIME: %s to %s\n",
                 timestamptz_to_str(start),
                 timestamptz_to_str(end));

    return result;
}

/*
 * ExecutorStart hook
 */
static void
trace_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
    if (trace_enabled && current_query)
    {
        /* Enable instrumentation */
        eflags |= EXEC_FLAG_EXPLAIN_ONLY;  /* Wrong flag, let me fix */
        queryDesc->instrument_options = INSTRUMENT_ALL;

        /* Write BINDS section if we have parameters */
        if (queryDesc->params && queryDesc->params->numParams > 0)
        {
            ParamListInfo params = queryDesc->params;
            int i;

            trace_printf("---------------------------------------------------------------------\n");
            trace_printf("BINDS #%lld:\n", (long long) current_query->cursor_id);

            for (i = 0; i < params->numParams; i++)
            {
                ParamExternData *param = &params->params[i];

                trace_printf(" Bind#%d\n", i);
                trace_printf("  oacdty=%u ", param->ptype);

                if (!param->isnull)
                {
                    Oid typoutput;
                    bool typIsVarlena;
                    char *val_str;

                    getTypeOutputInfo(param->ptype, &typoutput, &typIsVarlena);
                    val_str = OidOutputFunctionCall(typoutput, param->value);

                    trace_printf("value=\"%s\"\n", val_str);
                    pfree(val_str);
                }
                else
                {
                    trace_printf("value=NULL\n");
                }
            }
        }

        /* Capture starting buffer usage */
        current_query->buffer_start = pgBufferUsage;

        /* Register with eBPF */
        register_cursor_for_ebpf(current_query->cursor_id);
    }

    /* Call original */
    if (prev_ExecutorStart_hook)
        prev_ExecutorStart_hook(queryDesc, eflags);
    else
        standard_ExecutorStart(queryDesc, eflags);
}

/*
 * ExecutorRun hook
 */
static void
trace_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction,
                  uint64 count, bool execute_once)
{
    TimestampTz start, end;
    long secs;
    int microsecs;

    if (trace_enabled && current_query)
    {
        start = GetCurrentTimestamp();
        current_query->exec_start = start;

        trace_printf("---------------------------------------------------------------------\n");
        trace_printf("EXEC #%lld at %s\n",
                     (long long) current_query->cursor_id,
                     timestamptz_to_str(start));
    }

    /* Call original */
    if (prev_ExecutorRun_hook)
        prev_ExecutorRun_hook(queryDesc, direction, count, execute_once);
    else
        standard_ExecutorRun(queryDesc, direction, count, execute_once);

    if (trace_enabled && current_query)
    {
        end = GetCurrentTimestamp();
        TimestampDifference(start, end, &secs, &microsecs);

        trace_printf("EXEC TIME: ela=%ld.%06d sec rows=%lld\n",
                     secs, microsecs,
                     (long long) queryDesc->estate->es_processed);
    }
}

/*
 * Write a single plan node with statistics
 */
static void
write_plan_node(PlanState *planstate, int level)
{
    Plan *plan = planstate->plan;
    Instrumentation *instr = planstate->instrument;
    int i;
    char indent[256];

    /* Create indentation */
    for (i = 0; i < level * 2 && i < 255; i++)
        indent[i] = ' ';
    indent[i] = '\0';

    /* Node type */
    trace_printf("%s-> %s", indent, nodeToString(planstate->type));

    if (plan->plan_node_id > 0)
        trace_printf(" [Node %d]", plan->plan_node_id);

    trace_printf("\n");

    /* Statistics if available */
    if (instr && instr->nloops > 0)
    {
        trace_printf("%s   Rows: planned=%.0f actual=%.0f loops=%.0f\n",
                     indent, plan->plan_rows, instr->ntuples, instr->nloops);
        trace_printf("%s   Time: startup=%.3f total=%.3f ms\n",
                     indent,
                     instr->startup * 1000.0,
                     instr->total * 1000.0);

        if (instr->need_bufusage)
        {
            trace_printf("%s   Buffers: shared hit=%ld read=%ld dirtied=%ld written=%ld\n",
                         indent,
                         instr->bufusage.shared_blks_hit,
                         instr->bufusage.shared_blks_read,
                         instr->bufusage.shared_blks_dirtied,
                         instr->bufusage.shared_blks_written);

            if (instr->bufusage.temp_blks_read > 0)
            {
                trace_printf("%s   Temp: read=%ld written=%ld\n",
                             indent,
                             instr->bufusage.temp_blks_read,
                             instr->bufusage.temp_blks_written);
            }
        }
    }
}

/*
 * Recursively write plan tree
 */
static void
write_plan_tree(PlanState *planstate, int level)
{
    if (!planstate)
        return;

    write_plan_node(planstate, level);

    /* Recurse to children */
    if (planstate->lefttree)
        write_plan_tree(planstate->lefttree, level + 1);

    if (planstate->righttree)
        write_plan_tree(planstate->righttree, level + 1);

    /* Handle special node types */
    if (IsA(planstate, AppendState))
    {
        AppendState *as = (AppendState *) planstate;
        int i;
        for (i = 0; i < as->as_nplans; i++)
            write_plan_tree(as->appendplans[i], level + 1);
    }
    else if (IsA(planstate, MergeAppendState))
    {
        MergeAppendState *ms = (MergeAppendState *) planstate;
        int i;
        for (i = 0; i < ms->ms_nplans; i++)
            write_plan_tree(ms->mergeplans[i], level + 1);
    }
    else if (IsA(planstate, SubqueryScanState))
    {
        SubqueryScanState *sss = (SubqueryScanState *) planstate;
        write_plan_tree(sss->subplan, level + 1);
    }
}

/*
 * ExecutorEnd hook
 */
static void
trace_ExecutorEnd(QueryDesc *queryDesc)
{
    if (trace_enabled && current_query)
    {
        BufferUsage buffer_end = pgBufferUsage;
        BufferUsage buffer_diff;

        /* Calculate buffer differences */
        buffer_diff.shared_blks_hit = buffer_end.shared_blks_hit - 
                                      current_query->buffer_start.shared_blks_hit;
        buffer_diff.shared_blks_read = buffer_end.shared_blks_read - 
                                       current_query->buffer_start.shared_blks_read;
        buffer_diff.shared_blks_dirtied = buffer_end.shared_blks_dirtied - 
                                          current_query->buffer_start.shared_blks_dirtied;
        buffer_diff.shared_blks_written = buffer_end.shared_blks_written - 
                                          current_query->buffer_start.shared_blks_written;

        trace_printf("---------------------------------------------------------------------\n");
        trace_printf("STATS #%lld:\n", (long long) current_query->cursor_id);
        trace_printf("  BUFFER STATS: cr=%ld pr=%ld pw=%ld dirtied=%ld\n",
                     buffer_diff.shared_blks_hit,
                     buffer_diff.shared_blks_read,
                     buffer_diff.shared_blks_written,
                     buffer_diff.shared_blks_dirtied);

        /* Write execution plan with statistics */
        if (queryDesc->planstate)
        {
            trace_printf("---------------------------------------------------------------------\n");
            trace_printf("EXECUTION PLAN #%lld:\n", (long long) current_query->cursor_id);
            write_plan_tree(queryDesc->planstate, 0);
        }

        trace_printf("=====================================================================\n\n");

        /* Cleanup */
        unregister_cursor_for_ebpf();

        if (current_query->sql_text)
            pfree(current_query->sql_text);
        pfree(current_query);
        current_query = NULL;
    }

    /* Call original */
    if (prev_ExecutorEnd_hook)
        prev_ExecutorEnd_hook(queryDesc);
    else
        standard_ExecutorEnd(queryDesc);
}

/*
 * SQL function: pg_trace_start_trace()
 */
Datum
pg_trace_start_trace(PG_FUNCTION_ARGS)
{
    struct stat st;

    if (trace_enabled)
    {
        ereport(NOTICE, (errmsg("Trace already enabled")));
        PG_RETURN_TEXT_P(cstring_to_text(trace_filename));
    }

    /* Create output directory if needed */
    if (stat(trace_output_directory, &st) != 0)
        mkdir(trace_output_directory, 0755);

    /* Create trace file */
    snprintf(trace_filename, MAXPGPATH, "%s/pg_trace_%d_%ld.trc",
             trace_output_directory, MyProcPid, (long) time(NULL));

    trace_file = fopen(trace_filename, "w");
    if (!trace_file)
        ereport(ERROR,
                (errcode_for_file_access(),
                 errmsg("could not open trace file \"%s\"", trace_filename)));

    /* Write header */
    trace_printf("***********************************************************************\n");
    trace_printf("*** PostgreSQL Session Trace (MVP)\n");
    trace_printf("*** PID: %d\n", MyProcPid);
    trace_printf("*** Start: %s\n", timestamptz_to_str(GetCurrentTimestamp()));
    trace_printf("*** File: %s\n", trace_filename);
    trace_printf("***\n");
    trace_printf("*** Note: Wait events require eBPF tracer to be running\n");
    trace_printf("***       Run: sudo python3 pg_trace_ebpf.py -p %d\n", MyProcPid);
    trace_printf("***********************************************************************\n\n");

    trace_enabled = true;

    ereport(NOTICE,
            (errmsg("Trace enabled for session"),
             errdetail("Trace file: %s", trace_filename),
             errhint("Run eBPF tracer: sudo python3 pg_trace_ebpf.py -p %d", MyProcPid)));

    PG_RETURN_TEXT_P(cstring_to_text(trace_filename));
}

/*
 * SQL function: pg_trace_stop_trace()
 */
Datum
pg_trace_stop_trace(PG_FUNCTION_ARGS)
{
    if (!trace_enabled)
    {
        ereport(NOTICE, (errmsg("Trace not enabled")));
        PG_RETURN_NULL();
    }

    trace_printf("\n*** Trace ended at %s\n", timestamptz_to_str(GetCurrentTimestamp()));
    trace_printf("*** Total queries traced: %lld\n", (long long) cursor_sequence);

    fclose(trace_file);
    trace_file = NULL;
    trace_enabled = false;

    ereport(NOTICE,
            (errmsg("Trace disabled"),
             errdetail("Trace file: %s", trace_filename)));

    PG_RETURN_TEXT_P(cstring_to_text(trace_filename));
}

/*
 * SQL function: pg_trace_get_tracefile()
 */
Datum
pg_trace_get_tracefile(PG_FUNCTION_ARGS)
{
    if (!trace_enabled || trace_filename[0] == '\0')
        PG_RETURN_NULL();

    PG_RETURN_TEXT_P(cstring_to_text(trace_filename));
}


