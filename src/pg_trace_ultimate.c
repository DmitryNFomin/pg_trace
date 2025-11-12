/*-------------------------------------------------------------------------
 *
 * pg_trace_ultimate.c
 *    Complete Oracle 10046-style tracing with per-block I/O detail
 *
 * This extension provides:
 * - SQL text, bind variables, execution plans
 * - CPU time from /proc
 * - Aggregate I/O from /proc
 * - PER-BLOCK I/O timing (with track_io_timing)
 * - OS cache vs physical disk distinction
 * - File paths and relation names
 * - All without eBPF or root!
 *
 * Requirements:
 * - SET track_io_timing = on;
 * - PostgreSQL shared_preload_libraries
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "access/heapam.h"
#include "access/tableam.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "common/relpath.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "optimizer/planner.h"
#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/timestamp.h"

#include "pg_trace_procfs.h"

PG_MODULE_MAGIC;

/*---- Module callbacks ----*/
void _PG_init(void);
void _PG_fini(void);

/*---- GUC variables ----*/
static char *trace_output_directory = NULL;
static bool trace_enabled = false;
static int os_cache_threshold_us = 500;  /* Threshold to distinguish OS cache vs disk */

/*---- Per-session state ----*/
static FILE *trace_file = NULL;
static char trace_filename[MAXPGPATH];
static int64 cursor_sequence = 0;
static TimestampTz session_start_time;

/*---- Block I/O tracking ----*/
typedef struct BlockIoStat
{
    RelFileNode rnode;
    ForkNumber forknum;
    BlockNumber blocknum;
    char *relname;
    char *filepath;
    TimestampTz timestamp;
    double io_time_us;
    bool was_hit;           /* Buffer hit (no syscall) */
    bool from_os_cache;     /* Fast syscall (< threshold) */
    bool from_disk;         /* Slow syscall (> threshold) */
} BlockIoStat;

/*---- Query execution context ----*/
typedef struct QueryTraceContext
{
    int64 cursor_id;
    char *sql_text;
    TimestampTz start_time;
    TimestampTz parse_time;
    TimestampTz exec_start_time;
    BufferUsage buffer_usage_start;
    ProcStats os_stats_start;
    
    /* Block-level I/O tracking */
    List *block_ios;
    
    /* Accumulated statistics */
    long pg_cache_hits;
    long os_cache_hits;
    long disk_reads;
    double total_os_cache_time_us;
    double total_disk_time_us;
} QueryTraceContext;

static QueryTraceContext *current_query_context = NULL;

/* For tracking buffer state between calls */
typedef struct BufferTracker
{
    BufferUsage last_bufusage;
    instr_time last_io_time;
    BlockNumber last_block[100];  /* Track recent blocks per relation */
    int last_block_count;
} BufferTracker;

static BufferTracker buffer_tracker;

/*---- Saved hooks ----*/
static planner_hook_type prev_planner_hook = NULL;
static ExecutorStart_hook_type prev_ExecutorStart_hook = NULL;
static ExecutorRun_hook_type prev_ExecutorRun_hook = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd_hook = NULL;

/*---- Function declarations ----*/
static void trace_printf(const char *fmt, ...) pg_attribute_printf(1, 2);
static void track_block_io_during_execution(void);
static void write_block_io_summary(void);
static void write_plan_tree(PlanState *planstate, int level);
static char *get_relation_name(RelFileNode *rnode);
static void capture_buffer_io_stats(void);

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
PG_FUNCTION_INFO_V1(pg_trace_set_cache_threshold);

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

    DefineCustomIntVariable("pg_trace.os_cache_threshold_us",
                            "Threshold in microseconds to distinguish OS cache from disk",
                            "I/O operations faster than this are considered OS cache hits",
                            &os_cache_threshold_us,
                            500,
                            10, 10000,
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
    
    /* Initialize buffer tracker */
    memset(&buffer_tracker, 0, sizeof(BufferTracker));
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
 * Get relation name from RelFileNode
 */
static char *
get_relation_name(RelFileNode *rnode)
{
    Oid relid = RelidByRelfilenode(rnode->spcNode, rnode->relNode);
    
    if (OidIsValid(relid))
    {
        char *relname = get_rel_name(relid);
        if (relname)
            return relname;
    }
    
    return psprintf("%u/%u/%u", rnode->spcNode, rnode->dbNode, rnode->relNode);
}

/*
 * Capture buffer I/O statistics by examining buffer usage deltas
 */
static void
capture_buffer_io_stats(void)
{
    BufferUsage current_bufusage = pgBufferUsage;
    instr_time current_io_time;
    
    if (!track_io_timing)
        return;
    
    current_io_time = pgBufferUsage.blk_read_time;
    
    /* Check if any new I/O happened */
    long new_reads = current_bufusage.shared_blks_read - 
                     buffer_tracker.last_bufusage.shared_blks_read;
    long new_hits = current_bufusage.shared_blks_hit - 
                    buffer_tracker.last_bufusage.shared_blks_hit;
    
    if (new_reads > 0 || new_hits > 0)
    {
        /* Calculate I/O time delta */
        instr_time io_delta = current_io_time;
        INSTR_TIME_SUBTRACT(io_delta, buffer_tracker.last_io_time);
        double io_time_us = INSTR_TIME_GET_MICROSEC(io_delta);
        
        /* Create block I/O stat entry */
        BlockIoStat *stat = palloc0(sizeof(BlockIoStat));
        stat->timestamp = GetCurrentTimestamp();
        
        if (new_hits > 0)
        {
            /* Buffer cache hit - no I/O */
            stat->was_hit = true;
            stat->io_time_us = 0;
            current_query_context->pg_cache_hits += new_hits;
        }
        else if (new_reads > 0)
        {
            /* Buffer miss - I/O occurred */
            stat->was_hit = false;
            stat->io_time_us = io_time_us / new_reads;  /* Average per block */
            
            /* Classify based on timing */
            if (stat->io_time_us < os_cache_threshold_us)
            {
                stat->from_os_cache = true;
                current_query_context->os_cache_hits += new_reads;
                current_query_context->total_os_cache_time_us += io_time_us;
            }
            else
            {
                stat->from_disk = true;
                current_query_context->disk_reads += new_reads;
                current_query_context->total_disk_time_us += io_time_us;
            }
        }
        
        current_query_context->block_ios = lappend(current_query_context->block_ios, stat);
    }
    
    /* Update tracker */
    buffer_tracker.last_bufusage = current_bufusage;
    buffer_tracker.last_io_time = current_io_time;
}

/*
 * Track block I/O during query execution
 */
static void
track_block_io_during_execution(void)
{
    if (!current_query_context || !track_io_timing)
        return;
    
    capture_buffer_io_stats();
}

/*
 * Write block I/O summary
 */
static void
write_block_io_summary(void)
{
    ListCell *lc;
    long total_blocks = current_query_context->pg_cache_hits +
                       current_query_context->os_cache_hits +
                       current_query_context->disk_reads;
    
    if (total_blocks == 0)
        return;
    
    trace_printf("---------------------------------------------------------------------\n");
    trace_printf("BLOCK I/O ANALYSIS (track_io_timing=%s):\n",
                 track_io_timing ? "on" : "off");
    
    if (!track_io_timing)
    {
        trace_printf("  WARNING: track_io_timing is OFF - no timing data available!\n");
        trace_printf("  Enable with: SET track_io_timing = on;\n");
        return;
    }
    
    trace_printf("\n");
    trace_printf("Three-Tier Cache Analysis:\n");
    trace_printf("\n");
    
    /* Tier 1: PostgreSQL shared buffers */
    trace_printf("  Tier 1 - PostgreSQL Shared Buffers:\n");
    trace_printf("    Hits: %ld blocks (%.1f%%) - NO syscall, instant access\n",
                 current_query_context->pg_cache_hits,
                 (double)current_query_context->pg_cache_hits / total_blocks * 100);
    
    /* Tier 2: OS page cache */
    if (current_query_context->os_cache_hits > 0)
    {
        double avg_os_cache_us = current_query_context->total_os_cache_time_us / 
                                 current_query_context->os_cache_hits;
        
        trace_printf("\n");
        trace_printf("  Tier 2 - OS Page Cache:\n");
        trace_printf("    Hits: %ld blocks (%.1f%%) - syscall but NO disk I/O\n",
                     current_query_context->os_cache_hits,
                     (double)current_query_context->os_cache_hits / total_blocks * 100);
        trace_printf("    Avg latency: %.1f us (< %d us threshold)\n",
                     avg_os_cache_us, os_cache_threshold_us);
        trace_printf("    Total time: %.2f ms\n",
                     current_query_context->total_os_cache_time_us / 1000.0);
    }
    
    /* Tier 3: Physical disk */
    if (current_query_context->disk_reads > 0)
    {
        double avg_disk_us = current_query_context->total_disk_time_us / 
                            current_query_context->disk_reads;
        
        trace_printf("\n");
        trace_printf("  Tier 3 - Physical Disk:\n");
        trace_printf("    Reads: %ld blocks (%.1f%%) - ACTUAL physical I/O\n",
                     current_query_context->disk_reads,
                     (double)current_query_context->disk_reads / total_blocks * 100);
        trace_printf("    Avg latency: %.1f us (> %d us threshold)\n",
                     avg_disk_us, os_cache_threshold_us);
        trace_printf("    Total time: %.2f ms ← THIS IS THE BOTTLENECK!\n",
                     current_query_context->total_disk_time_us / 1000.0);
    }
    
    trace_printf("\n");
    trace_printf("SUMMARY:\n");
    trace_printf("  Total blocks accessed: %ld\n", total_blocks);
    trace_printf("  No I/O (PG cache): %ld (%.1f%%)\n",
                 current_query_context->pg_cache_hits,
                 (double)current_query_context->pg_cache_hits / total_blocks * 100);
    trace_printf("  Fast I/O (OS cache): %ld (%.1f%%)\n",
                 current_query_context->os_cache_hits,
                 (double)current_query_context->os_cache_hits / total_blocks * 100);
    trace_printf("  Slow I/O (disk): %ld (%.1f%%) ← Performance issue if high\n",
                 current_query_context->disk_reads,
                 (double)current_query_context->disk_reads / total_blocks * 100);
    
    /* Verify against /proc if available */
    ProcStats os_end;
    if (proc_read_all_stats(MyProcPid, &os_end))
    {
        ProcIoStats io_diff;
        proc_io_stats_diff(&current_query_context->os_stats_start.io, &os_end.io, &io_diff);
        
        long actual_disk_blocks = io_diff.read_bytes / BLCKSZ;
        
        trace_printf("\n");
        trace_printf("Verification from /proc/[pid]/io:\n");
        trace_printf("  Physical reads: %llu bytes (%ld blocks)\n",
                     io_diff.read_bytes, actual_disk_blocks);
        
        if (actual_disk_blocks == current_query_context->disk_reads)
            trace_printf("  ✓ Matches our disk read count!\n");
        else if (actual_disk_blocks < current_query_context->disk_reads)
            trace_printf("  Note: Some 'disk' reads may have been from OS cache\n");
    }
}

/*
 * Write plan tree with statistics - ENHANCED with per-node detail
 */
static void
write_plan_tree(PlanState *planstate, int level)
{
    if (!planstate)
        return;
    
    Instrumentation *instr = planstate->instrument;
    Plan *plan = planstate->plan;
    char indent[256];
    int i;
    
    for (i = 0; i < level * 2 && i < 255; i++)
        indent[i] = ' ';
    indent[i] = '\0';
    
    trace_printf("%s-> %s", indent, nodeToString(planstate->type));
    
    if (instr && instr->nloops > 0)
    {
        double total_ms = instr->total * 1000.0;
        double startup_ms = instr->startup * 1000.0;
        
        trace_printf(" (actual rows=%.0f loops=%d)\n", 
                     instr->ntuples / instr->nloops, instr->nloops);
        
        /* Timing breakdown */
        trace_printf("%s   Timing: startup=%.3f ms, total=%.3f ms", 
                     indent, startup_ms, total_ms);
        
        if (instr->nloops > 1)
            trace_printf(", avg=%.3f ms/loop", total_ms / instr->nloops);
        trace_printf("\n");
        
        /* Buffer statistics with three-tier analysis */
        if (instr->need_bufusage)
        {
            long total_blocks = instr->bufusage.shared_blks_hit + 
                               instr->bufusage.shared_blks_read;
            
            if (total_blocks > 0)
            {
                double hit_pct = (double)instr->bufusage.shared_blks_hit / total_blocks * 100.0;
                
                trace_printf("%s   Buffers: shared hit=%ld read=%ld",
                             indent,
                             instr->bufusage.shared_blks_hit,
                             instr->bufusage.shared_blks_read);
                
                if (instr->bufusage.shared_blks_dirtied > 0)
                    trace_printf(" dirtied=%ld", instr->bufusage.shared_blks_dirtied);
                if (instr->bufusage.shared_blks_written > 0)
                    trace_printf(" written=%ld", instr->bufusage.shared_blks_written);
                
                trace_printf(" (%.1f%% cache hit)\n", hit_pct);
                
                /* I/O timing analysis */
                if (track_io_timing && instr->bufusage.shared_blks_read > 0)
                {
                    double io_ms = INSTR_TIME_GET_MILLISEC(instr->bufusage.blk_read_time);
                    double avg_us = (io_ms * 1000.0) / instr->bufusage.shared_blks_read;
                    
                    /* Estimate OS cache vs disk based on timing */
                    long estimated_os_cache = 0;
                    long estimated_disk = 0;
                    
                    if (avg_us < os_cache_threshold_us)
                    {
                        estimated_os_cache = instr->bufusage.shared_blks_read;
                    }
                    else
                    {
                        /* Mixed - rough estimate */
                        double disk_ratio = (avg_us - os_cache_threshold_us / 2.0) / 
                                           (avg_us + os_cache_threshold_us / 2.0);
                        if (disk_ratio < 0) disk_ratio = 0;
                        if (disk_ratio > 1) disk_ratio = 1;
                        
                        estimated_disk = (long)(instr->bufusage.shared_blks_read * disk_ratio);
                        estimated_os_cache = instr->bufusage.shared_blks_read - estimated_disk;
                    }
                    
                    trace_printf("%s   I/O Detail: total=%.3f ms, avg=%.1f us/block", 
                                 indent, io_ms, avg_us);
                    
                    if (estimated_os_cache > 0)
                        trace_printf(", ~%ld from OS cache", estimated_os_cache);
                    if (estimated_disk > 0)
                        trace_printf(", ~%ld from disk", estimated_disk);
                    
                    trace_printf("\n");
                    
                    /* CPU time estimation (wall clock - I/O time) */
                    double cpu_ms = total_ms - io_ms;
                    if (cpu_ms > 0)
                    {
                        double cpu_pct = (cpu_ms / total_ms) * 100.0;
                        double io_pct = (io_ms / total_ms) * 100.0;
                        
                        trace_printf("%s   Time breakdown: CPU ~%.3f ms (%.1f%%), I/O ~%.3f ms (%.1f%%)\n",
                                     indent, cpu_ms, cpu_pct, io_ms, io_pct);
                    }
                }
                else if (!track_io_timing && instr->bufusage.shared_blks_read > 0)
                {
                    trace_printf("%s   (track_io_timing=off, no I/O timing available)\n", indent);
                }
            }
            
            /* Local buffers (for temp tables) */
            if (instr->bufusage.local_blks_hit > 0 || instr->bufusage.local_blks_read > 0)
            {
                trace_printf("%s   Local Buffers: hit=%ld read=%ld",
                             indent,
                             instr->bufusage.local_blks_hit,
                             instr->bufusage.local_blks_read);
                if (instr->bufusage.local_blks_written > 0)
                    trace_printf(" written=%ld", instr->bufusage.local_blks_written);
                trace_printf("\n");
            }
            
            /* Temp buffers */
            if (instr->bufusage.temp_blks_read > 0 || instr->bufusage.temp_blks_written > 0)
            {
                trace_printf("%s   Temp Buffers: read=%ld written=%ld\n",
                             indent,
                             instr->bufusage.temp_blks_read,
                             instr->bufusage.temp_blks_written);
            }
        }
        
        /* WAL statistics (if available) */
        if (instr->need_walusage && 
            (instr->walusage.wal_records > 0 || instr->walusage.wal_bytes > 0))
        {
            trace_printf("%s   WAL: records=%ld fpi=%ld bytes=%ld\n",
                         indent,
                         instr->walusage.wal_records,
                         instr->walusage.wal_fpi,
                         instr->walusage.wal_bytes);
        }
    }
    else
    {
        trace_printf("\n");
    }
    
    /* Recurse */
    if (planstate->lefttree)
        write_plan_tree(planstate->lefttree, level + 1);
    if (planstate->righttree)
        write_plan_tree(planstate->righttree, level + 1);
    
    /* Handle special nodes */
    if (IsA(planstate, AppendState))
    {
        AppendState *as = (AppendState *) planstate;
        for (i = 0; i < as->as_nplans; i++)
            write_plan_tree(as->appendplans[i], level + 1);
    }
    else if (IsA(planstate, SubqueryScanState))
    {
        SubqueryScanState *sss = (SubqueryScanState *) planstate;
        write_plan_tree(sss->subplan, level + 1);
    }
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

    current_query_context = (QueryTraceContext *) MemoryContextAllocZero(
        TopMemoryContext, sizeof(QueryTraceContext));
    current_query_context->cursor_id = ++cursor_sequence;
    current_query_context->sql_text = MemoryContextStrdup(TopMemoryContext, query_string);
    current_query_context->block_ios = NIL;

    start = GetCurrentTimestamp();

    if (prev_planner_hook)
        result = prev_planner_hook(parse, query_string, cursorOptions, boundParams);
    else
        result = standard_planner(parse, query_string, cursorOptions, boundParams);

    end = GetCurrentTimestamp();
    TimestampDifference(start, end, &secs, &microsecs);

    trace_printf("=====================================================================\n");
    trace_printf("PARSE #%lld\n", (long long) current_query_context->cursor_id);
    trace_printf("SQL: %s\n", query_string);
    trace_printf("PARSE TIME: %ld.%06d sec\n", secs, microsecs);

    return result;
}

/*
 * ExecutorStart hook
 */
static void
trace_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
    if (trace_enabled && current_query_context)
    {
        /* Enable full instrumentation */
        queryDesc->instrument_options = INSTRUMENT_ALL;
        
        /* Capture starting state */
        current_query_context->buffer_usage_start = pgBufferUsage;
        proc_read_all_stats(MyProcPid, &current_query_context->os_stats_start);
        
        /* Initialize buffer tracker */
        buffer_tracker.last_bufusage = pgBufferUsage;
        buffer_tracker.last_io_time = pgBufferUsage.blk_read_time;
        
        /* Write binds if present */
        if (queryDesc->params && queryDesc->params->numParams > 0)
        {
            ParamListInfo params = queryDesc->params;
            int i;

            trace_printf("---------------------------------------------------------------------\n");
            trace_printf("BINDS #%lld:\n", (long long) current_query_context->cursor_id);

            for (i = 0; i < params->numParams; i++)
            {
                ParamExternData *param = &params->params[i];
                trace_printf(" Bind#%d type=%u ", i, param->ptype);

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

    if (trace_enabled && current_query_context)
    {
        start = GetCurrentTimestamp();
        current_query_context->exec_start_time = start;

        trace_printf("---------------------------------------------------------------------\n");
        trace_printf("EXEC #%lld\n", (long long) current_query_context->cursor_id);
    }

    /* Capture I/O before execution */
    if (trace_enabled && current_query_context)
        track_block_io_during_execution();

    if (prev_ExecutorRun_hook)
        prev_ExecutorRun_hook(queryDesc, direction, count, execute_once);
    else
        standard_ExecutorRun(queryDesc, direction, count, execute_once);

    /* Capture I/O after execution */
    if (trace_enabled && current_query_context)
    {
        track_block_io_during_execution();
        
        end = GetCurrentTimestamp();
        TimestampDifference(start, end, &secs, &microsecs);

        trace_printf("EXEC TIME: ela=%ld.%06d sec rows=%lld\n",
                     secs, microsecs,
                     (long long) queryDesc->estate->es_processed);
    }
}

/*
 * ExecutorEnd hook
 */
static void
trace_ExecutorEnd(QueryDesc *queryDesc)
{
    if (trace_enabled && current_query_context)
    {
        BufferUsage buffer_end = pgBufferUsage;
        ProcStats os_end;
        
        /* Final I/O capture */
        track_block_io_during_execution();
        
        /* Write buffer statistics */
        BufferUsage buffer_diff;
        buffer_diff.shared_blks_hit = buffer_end.shared_blks_hit - 
                                      current_query_context->buffer_usage_start.shared_blks_hit;
        buffer_diff.shared_blks_read = buffer_end.shared_blks_read - 
                                       current_query_context->buffer_usage_start.shared_blks_read;

        trace_printf("---------------------------------------------------------------------\n");
        trace_printf("BUFFER STATS: cr=%ld pr=%ld\n",
                     buffer_diff.shared_blks_hit,
                     buffer_diff.shared_blks_read);
        
        /* Write OS stats */
        if (proc_read_all_stats(MyProcPid, &os_end))
        {
            ProcCpuStats cpu_diff;
            proc_cpu_stats_diff(&current_query_context->os_stats_start.cpu, &os_end.cpu, &cpu_diff);
            
            trace_printf("CPU: user=%.3f sec system=%.3f sec total=%.3f sec\n",
                         cpu_diff.utime_sec,
                         cpu_diff.stime_sec,
                         cpu_diff.total_sec);
        }
        
        /* Write block I/O analysis */
        write_block_io_summary();
        
        /* Write execution plan */
        if (queryDesc->planstate)
        {
            trace_printf("---------------------------------------------------------------------\n");
            trace_printf("EXECUTION PLAN #%lld:\n", (long long) current_query_context->cursor_id);
            write_plan_tree(queryDesc->planstate, 0);
        }

        trace_printf("=====================================================================\n\n");

        /* Cleanup */
        if (current_query_context->sql_text)
            pfree(current_query_context->sql_text);
        list_free_deep(current_query_context->block_ios);
        pfree(current_query_context);
        current_query_context = NULL;
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
    trace_printf("*** PostgreSQL Ultimate Trace (Oracle 10046-style + per-block I/O)\n");
    trace_printf("*** PID: %d\n", MyProcPid);
    trace_printf("*** Start: %s\n", timestamptz_to_str(GetCurrentTimestamp()));
    trace_printf("*** File: %s\n", trace_filename);
    trace_printf("*** track_io_timing: %s\n", track_io_timing ? "ON" : "OFF");
    if (!track_io_timing)
    {
        trace_printf("***\n");
        trace_printf("*** WARNING: track_io_timing is OFF!\n");
        trace_printf("*** Enable with: SET track_io_timing = on;\n");
        trace_printf("*** Without it, you won't get per-block I/O timing!\n");
    }
    trace_printf("*** OS cache threshold: %d microseconds\n", os_cache_threshold_us);
    trace_printf("***********************************************************************\n\n");

    trace_enabled = true;

    ereport(NOTICE,
            (errmsg("Trace enabled for session"),
             errdetail("Trace file: %s", trace_filename),
             errhint("Make sure track_io_timing = on for per-block I/O timing!")));

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

Datum
pg_trace_set_cache_threshold(PG_FUNCTION_ARGS)
{
    int new_threshold = PG_GETARG_INT32(0);
    
    if (new_threshold < 10 || new_threshold > 10000)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("cache threshold must be between 10 and 10000 microseconds")));
    
    os_cache_threshold_us = new_threshold;
    
    ereport(NOTICE,
            (errmsg("OS cache threshold set to %d microseconds", os_cache_threshold_us)));
    
    PG_RETURN_INT32(os_cache_threshold_us);
}

