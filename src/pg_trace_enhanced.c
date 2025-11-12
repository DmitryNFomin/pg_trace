/*-------------------------------------------------------------------------
 *
 * pg_trace_enhanced.c
 *    Enhanced MVP with /proc filesystem CPU and I/O statistics
 *
 * This version adds OS-level statistics without requiring eBPF:
 * - CPU time (user, system) from /proc/[pid]/stat
 * - I/O statistics from /proc/[pid]/io
 * - Memory usage from /proc/[pid]/status
 *
 * Combined with the extension's PostgreSQL-level stats, this provides
 * a complete Oracle 10046-style trace.
 *
 * Usage is the same as pg_trace_mvp, but with richer output.
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

#include "pg_trace_procfs.h"

PG_MODULE_MAGIC;

/* GUC variables */
static char *trace_output_directory = NULL;
static bool trace_enabled = false;
static bool collect_os_stats = true;

/* Per-session state */
static FILE *trace_file = NULL;
static char trace_filename[MAXPGPATH];
static int64 cursor_sequence = 0;
static TimestampTz session_start_time;

/* Current query context with OS stats */
typedef struct CurrentQuery {
    int64 cursor_id;
    char *sql_text;
    TimestampTz parse_start;
    TimestampTz parse_end;
    TimestampTz exec_start;
    BufferUsage buffer_start;
    
    /* OS-level statistics */
    ProcStats os_stats_start;
    ProcStats os_stats_end;
} CurrentQuery;

static CurrentQuery *current_query = NULL;

/* Saved hooks */
static planner_hook_type prev_planner_hook = NULL;
static ExecutorStart_hook_type prev_ExecutorStart_hook = NULL;
static ExecutorRun_hook_type prev_ExecutorRun_hook = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd_hook = NULL;

/* Function declarations */
void _PG_init(void);
void _PG_fini(void);

static void trace_printf(const char *fmt, ...) pg_attribute_printf(1, 2);
static void write_plan_tree(PlanState *planstate, int level);
static void write_plan_node(PlanState *planstate, int level);
static void write_os_stats(const char *label, ProcStats *start, ProcStats *end);

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

    DefineCustomStringVariable("pg_trace.output_directory",
                               "Directory for trace files",
                               NULL,
                               &trace_output_directory,
                               "/tmp/pg_trace",
                               PGC_SUSET,
                               0,
                               NULL, NULL, NULL);

    DefineCustomBoolVariable("pg_trace.collect_os_stats",
                             "Collect OS-level CPU and I/O statistics",
                             NULL,
                             &collect_os_stats,
                             true,
                             PGC_USERSET,
                             0,
                             NULL, NULL, NULL);

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
    planner_hook = prev_planner_hook;
    ExecutorStart_hook = prev_ExecutorStart_hook;
    ExecutorRun_hook = prev_ExecutorRun_hook;
    ExecutorEnd_hook = prev_ExecutorEnd_hook;
    
    if (trace_file)
        fclose(trace_file);
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
    fflush(trace_file);
}

/*
 * Write OS-level statistics comparison
 */
static void
write_os_stats(const char *label, ProcStats *start, ProcStats *end)
{
    ProcCpuStats cpu_diff;
    ProcIoStats io_diff;
    
    if (!start || !end || !start->valid || !end->valid)
        return;
    
    trace_printf("OS STATS: %s\n", label);
    
    /* CPU statistics */
    proc_cpu_stats_diff(&start->cpu, &end->cpu, &cpu_diff);
    trace_printf("  CPU: user=%.3f sec system=%.3f sec total=%.3f sec\n",
                 cpu_diff.utime_sec,
                 cpu_diff.stime_sec,
                 cpu_diff.total_sec);
    
    /* I/O statistics */
    proc_io_stats_diff(&start->io, &end->io, &io_diff);
    
    if (io_diff.read_bytes > 0 || io_diff.write_bytes > 0)
    {
        trace_printf("  STORAGE I/O: read=%llu bytes (%llu syscalls) write=%llu bytes (%llu syscalls)\n",
                     io_diff.read_bytes, io_diff.syscr,
                     io_diff.write_bytes, io_diff.syscw);
    }
    
    if (io_diff.rchar > 0 || io_diff.wchar > 0)
    {
        trace_printf("  TOTAL I/O: read=%llu bytes write=%llu bytes\n",
                     io_diff.rchar, io_diff.wchar);
    }
    
    /* Memory */
    trace_printf("  MEMORY: rss=%lu KB peak=%lu KB\n",
                 end->mem.vm_rss_kb,
                 end->mem.vm_peak_kb);
}

/*
 * Planner hook
 */
static PlannedStmt *
trace_planner(Query *parse, const char *query_string,
              int cursorOptions, ParamListInfo boundParams)
{
    PlannedStmt *result;
    TimestampTz start, end;
    long secs;
    int microsecs;

    if (!trace_enabled || !query_string)
    {
        if (prev_planner_hook)
            return prev_planner_hook(parse, query_string, cursorOptions, boundParams);
        else
            return standard_planner(parse, query_string, cursorOptions, boundParams);
    }

    current_query = (CurrentQuery *) MemoryContextAllocZero(TopMemoryContext,
                                                             sizeof(CurrentQuery));
    current_query->cursor_id = ++cursor_sequence;
    current_query->sql_text = MemoryContextStrdup(TopMemoryContext, query_string);

    start = GetCurrentTimestamp();
    
    /* Capture OS stats before planning */
    if (collect_os_stats)
        proc_read_all_stats(MyProcPid, &current_query->os_stats_start);

    if (prev_planner_hook)
        result = prev_planner_hook(parse, query_string, cursorOptions, boundParams);
    else
        result = standard_planner(parse, query_string, cursorOptions, boundParams);

    end = GetCurrentTimestamp();
    TimestampDifference(start, end, &secs, &microsecs);

    current_query->parse_start = start;
    current_query->parse_end = end;

    /* Write PARSE section */
    trace_printf("=====================================================================\n");
    trace_printf("PARSE #%lld\n", (long long) current_query->cursor_id);
    trace_printf("SQL: %s\n", query_string);
    trace_printf("PARSE: c=%ld,e=%ld.%06d\n",
                 (secs * 1000000 + microsecs),  /* CPU time (approximation) */
                 secs, microsecs);              /* Elapsed time */

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
        queryDesc->instrument_options = INSTRUMENT_ALL;

        /* Write BINDS section */
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

        current_query->buffer_start = pgBufferUsage;
    }

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
    ProcStats os_start, os_end;

    if (trace_enabled && current_query)
    {
        start = GetCurrentTimestamp();
        current_query->exec_start = start;
        
        /* Capture OS stats before execution */
        if (collect_os_stats)
            proc_read_all_stats(MyProcPid, &os_start);

        trace_printf("---------------------------------------------------------------------\n");
        trace_printf("EXEC #%lld\n", (long long) current_query->cursor_id);
    }

    if (prev_ExecutorRun_hook)
        prev_ExecutorRun_hook(queryDesc, direction, count, execute_once);
    else
        standard_ExecutorRun(queryDesc, direction, count, execute_once);

    if (trace_enabled && current_query)
    {
        end = GetCurrentTimestamp();
        TimestampDifference(start, end, &secs, &microsecs);
        
        /* Capture OS stats after execution */
        if (collect_os_stats)
        {
            proc_read_all_stats(MyProcPid, &os_end);
            current_query->os_stats_end = os_end;
        }

        trace_printf("EXEC: c=%ld,e=%ld.%06d,r=%lld\n",
                     (secs * 1000000 + microsecs),  /* CPU (approx) */
                     secs, microsecs,               /* Elapsed */
                     (long long) queryDesc->estate->es_processed);  /* Rows */
        
        /* Write OS statistics */
        if (collect_os_stats && os_start.valid && os_end.valid)
        {
            ProcCpuStats cpu_diff;
            ProcIoStats io_diff;
            
            proc_cpu_stats_diff(&os_start.cpu, &os_end.cpu, &cpu_diff);
            proc_io_stats_diff(&os_start.io, &os_end.io, &io_diff);
            
            trace_printf("  OS CPU: user=%.3fs sys=%.3fs total=%.3fs\n",
                         cpu_diff.utime_sec, cpu_diff.stime_sec, cpu_diff.total_sec);
            
            if (io_diff.read_bytes > 0 || io_diff.write_bytes > 0)
            {
                trace_printf("  OS I/O: read=%llu bytes write=%llu bytes\n",
                             io_diff.read_bytes, io_diff.write_bytes);
            }
        }
    }
}

/*
 * Write plan node
 */
static void
write_plan_node(PlanState *planstate, int level)
{
    Plan *plan = planstate->plan;
    Instrumentation *instr = planstate->instrument;
    int i;
    char indent[256];

    for (i = 0; i < level * 2 && i < 255; i++)
        indent[i] = ' ';
    indent[i] = '\0';

    trace_printf("%s-> %s", indent, nodeToString(planstate->type));

    if (plan->plan_node_id > 0)
        trace_printf(" [Node %d]", plan->plan_node_id);

    trace_printf("\n");

    if (instr && instr->nloops > 0)
    {
        trace_printf("%s   c=%.0f,e=%.3f,r=%.0f\n",
                     indent,
                     (instr->total * 1000000.0),  /* CPU microseconds */
                     instr->total * 1000.0,        /* Elapsed milliseconds */
                     instr->ntuples);              /* Rows */

        if (instr->need_bufusage)
        {
            trace_printf("%s   cr=%ld pr=%ld pw=%ld dirtied=%ld\n",
                         indent,
                         instr->bufusage.shared_blks_hit,
                         instr->bufusage.shared_blks_read,
                         instr->bufusage.shared_blks_written,
                         instr->bufusage.shared_blks_dirtied);
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

    if (planstate->lefttree)
        write_plan_tree(planstate->lefttree, level + 1);

    if (planstate->righttree)
        write_plan_tree(planstate->righttree, level + 1);

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

        buffer_diff.shared_blks_hit = buffer_end.shared_blks_hit - 
                                      current_query->buffer_start.shared_blks_hit;
        buffer_diff.shared_blks_read = buffer_end.shared_blks_read - 
                                       current_query->buffer_start.shared_blks_read;
        buffer_diff.shared_blks_dirtied = buffer_end.shared_blks_dirtied - 
                                          current_query->buffer_start.shared_blks_dirtied;
        buffer_diff.shared_blks_written = buffer_end.shared_blks_written - 
                                          current_query->buffer_start.shared_blks_written;

        trace_printf("---------------------------------------------------------------------\n");
        trace_printf("STAT #%lld: cr=%ld pr=%ld pw=%ld dirtied=%ld\n",
                     (long long) current_query->cursor_id,
                     buffer_diff.shared_blks_hit,
                     buffer_diff.shared_blks_read,
                     buffer_diff.shared_blks_written,
                     buffer_diff.shared_blks_dirtied);

        /* Write execution plan */
        if (queryDesc->planstate)
        {
            trace_printf("---------------------------------------------------------------------\n");
            trace_printf("PLAN #%lld:\n", (long long) current_query->cursor_id);
            write_plan_tree(queryDesc->planstate, 0);
        }

        trace_printf("=====================================================================\n\n");

        if (current_query->sql_text)
            pfree(current_query->sql_text);
        pfree(current_query);
        current_query = NULL;
    }

    if (prev_ExecutorEnd_hook)
        prev_ExecutorEnd_hook(queryDesc);
    else
        standard_ExecutorEnd(queryDesc);
}

/*
 * SQL functions
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

    if (stat(trace_output_directory, &st) != 0)
        mkdir(trace_output_directory, 0755);

    snprintf(trace_filename, MAXPGPATH, "%s/pg_trace_%d_%ld.trc",
             trace_output_directory, MyProcPid, (long) time(NULL));

    trace_file = fopen(trace_filename, "w");
    if (!trace_file)
        ereport(ERROR,
                (errcode_for_file_access(),
                 errmsg("could not open trace file \"%s\"", trace_filename)));

    trace_printf("***********************************************************************\n");
    trace_printf("*** PostgreSQL Session Trace (with OS stats)\n");
    trace_printf("*** PID: %d\n", MyProcPid);
    trace_printf("*** Start: %s\n", timestamptz_to_str(GetCurrentTimestamp()));
    trace_printf("*** File: %s\n", trace_filename);
    trace_printf("*** OS Stats: %s\n", collect_os_stats ? "enabled" : "disabled");
    trace_printf("***********************************************************************\n\n");

    trace_enabled = true;

    ereport(NOTICE,
            (errmsg("Trace enabled for session"),
             errdetail("Trace file: %s", trace_filename)));

    PG_RETURN_TEXT_P(cstring_to_text(trace_filename));
}

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

Datum
pg_trace_get_tracefile(PG_FUNCTION_ARGS)
{
    if (!trace_enabled || trace_filename[0] == '\0')
        PG_RETURN_NULL();

    PG_RETURN_TEXT_P(cstring_to_text(trace_filename));
}

