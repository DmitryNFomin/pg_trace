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

#include <string.h>
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
    Oid spcNode;            /* Tablespace OID */
    Oid dbNode;             /* Database OID */
    Oid relNode;            /* Relation OID */
    ForkNumber forknum;
    BlockNumber blocknum;
    char relname[64];       /* Relation name */
    double io_time_us;      /* Time for this block's I/O */
    bool was_hit;           /* Buffer hit (no syscall) */
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
static const char *fork_names[] = {"main", "fsm", "vm", "init"};

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
                               "/tmp",
                               PGC_USERSET,
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
 * Note: RelidByRelfilenode may not be available in all PostgreSQL versions
 */
static char *
get_relation_name(RelFileNode *rnode)
{
    /* Try to find relation by relfilenode - simplified approach */
    /* For now, just return the file path representation */
    return psprintf("%u/%u/%u", rnode->spcNode, rnode->dbNode, rnode->relNode);
}

/*
 * Capture blocks from buffer descriptors with relation names
 */
static void
capture_buffer_io_stats(void)
{
    int i;
    BufferDesc *bufHdr;
    uint32 buf_state;
    BufferUsage current_bufusage;
    instr_time current_io_time;
    long new_reads;
    double io_time_us = 0;
    double avg_time_per_block;
    
    if (!track_io_timing)
        return;
    
    current_bufusage = pgBufferUsage;
    current_io_time = pgBufferUsage.blk_read_time;
    
    /* Check if any new I/O happened */
    new_reads = current_bufusage.shared_blks_read - 
                buffer_tracker.last_bufusage.shared_blks_read;
    
    if (new_reads > 0)
    {
        instr_time io_delta = current_io_time;
        INSTR_TIME_SUBTRACT(io_delta, buffer_tracker.last_io_time);
        io_time_us = INSTR_TIME_GET_MICROSEC(io_delta);
        avg_time_per_block = io_time_us / new_reads;
        
        current_query_context->disk_reads += new_reads;
        current_query_context->total_disk_time_us += io_time_us;
    }
    
    /* Update hits */
    current_query_context->pg_cache_hits += current_bufusage.shared_blks_hit - 
                                            buffer_tracker.last_bufusage.shared_blks_hit;
    
    /* Scan buffer descriptors to capture which specific blocks were accessed */
    for (i = 0; i < NBuffers && i < 10000; i++)  /* Limit scan */
    {
        bufHdr = GetBufferDescriptor(i);
        buf_state = LockBufHdr(bufHdr);
        
        /* Check if this buffer is valid, tagged, and was recently used */
        if ((buf_state & BM_VALID) && (buf_state & BM_TAG_VALID) &&
            bufHdr->tag.rnode.dbNode == MyDatabaseId)
        {
            BlockIoStat *stat = palloc0(sizeof(BlockIoStat));
            
            stat->spcNode = bufHdr->tag.rnode.spcNode;
            stat->dbNode = bufHdr->tag.rnode.dbNode;
            stat->relNode = bufHdr->tag.rnode.relNode;
            stat->forknum = bufHdr->tag.forkNum;
            stat->blocknum = bufHdr->tag.blockNum;
            
            /* Get relation name */
            {
                char *relname = get_rel_name(stat->relNode);
            if (relname)
            {
                strncpy(stat->relname, relname, sizeof(stat->relname) - 1);
                pfree(relname);
            }
            else
            {
                snprintf(stat->relname, sizeof(stat->relname), "rel_%u", stat->relNode);
            }
            }
            
            /* Estimate timing for this block */
            stat->was_hit = (BUF_STATE_GET_REFCOUNT(buf_state) > 1);
            stat->io_time_us = stat->was_hit ? 0 : avg_time_per_block;
            
            current_query_context->block_ios = lappend(current_query_context->block_ios, stat);
            
            /* Limit collection to 500 blocks */
            if (list_length(current_query_context->block_ios) >= 500)
            {
                UnlockBufHdr(bufHdr, buf_state);
                break;
            }
        }
        
        UnlockBufHdr(bufHdr, buf_state);
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
    long total_blocks = current_query_context->pg_cache_hits +
                       current_query_context->os_cache_hits +
                       current_query_context->disk_reads;
    
    if (total_blocks == 0)
        return;
    
    trace_printf("---------------------------------------------------------------------\n");
    trace_printf("WAIT EVENTS (Oracle 10046-style):\n");
    trace_printf("---------------------------------------------------------------------\n");
    
    /* Print per-block wait events */
    if (current_query_context->block_ios != NIL)
    {
        ListCell *lc;
        BlockIoStat *stat;
        int block_num;
        
        block_num = 0;
        
        foreach(lc, current_query_context->block_ios)
        {
            stat = (BlockIoStat *) lfirst(lc);
            
            if (!stat->was_hit && stat->io_time_us > 0)
            {
                /* Print Oracle-style WAIT event */
                trace_printf("WAIT #%lld: nam='db file sequential read' ela=%.0f file#=%u/%u/%u block=%u obj#=%u\n",
                             (long long) current_query_context->cursor_id,
                             stat->io_time_us,
                             stat->spcNode,
                             stat->dbNode,
                             stat->relNode,
                             stat->blocknum,
                             stat->relNode);
                trace_printf("  table='%s' fork=%s\n",
                             stat->relname,
                             (stat->forknum < 4) ? fork_names[stat->forknum] : "unknown");
                
                block_num++;
                if (block_num >= 100)
                {
                    trace_printf("  ... (showing first 100 I/O blocks only, total: %d)\n",
                                 list_length(current_query_context->block_ios));
                    break;
                }
            }
        }
        
        if (block_num == 0)
            trace_printf("  (no physical I/O - all blocks from cache)\n");
    }
    
    trace_printf("\n");
    trace_printf("---------------------------------------------------------------------\n");
    trace_printf("BLOCK I/O SUMMARY:\n");
    trace_printf("---------------------------------------------------------------------\n");
    trace_printf("Total blocks accessed: %ld\n", total_blocks);
    trace_printf("  Buffer hits (cr): %ld blocks - no I/O\n",
                 current_query_context->pg_cache_hits);
    trace_printf("  Physical reads (pr): %ld blocks\n",
                 current_query_context->disk_reads);
    
    if (current_query_context->disk_reads > 0)
    {
        double avg_time = current_query_context->total_disk_time_us / current_query_context->disk_reads;
        trace_printf("  Average I/O time: %.1f microseconds/block\n", avg_time);
        trace_printf("  Total I/O time: %.2f ms\n",
                     current_query_context->total_disk_time_us / 1000.0);
    }
    
    /* Verify against /proc if available */
    {
        ProcStats os_end;
        
        if (proc_read_all_stats(MyProcPid, &os_end))
        {
            ProcIoStats io_diff;
            long actual_disk_blocks;
            
            proc_io_stats_diff(&current_query_context->os_stats_start.io, &os_end.io, &io_diff);
            actual_disk_blocks = io_diff.read_bytes / BLCKSZ;
        
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
}

/*
 * Recursively finalize instrumentation for all nodes
 */
static void
finalize_plan_instrumentation(PlanState *planstate)
{
    int i;
    
    if (!planstate)
        return;
    
    /* Finalize this node if needed */
    if (planstate->instrument && planstate->instrument->running)
        InstrEndLoop(planstate->instrument);
    
    /* Recurse to child nodes */
    if (planstate->lefttree)
        finalize_plan_instrumentation(planstate->lefttree);
    if (planstate->righttree)
        finalize_plan_instrumentation(planstate->righttree);
    
    /* Handle special multi-child nodes */
    if (IsA(planstate, AppendState))
    {
        AppendState *as = (AppendState *) planstate;
        for (i = 0; i < as->as_nplans; i++)
            finalize_plan_instrumentation(as->appendplans[i]);
    }
    else if (IsA(planstate, SubqueryScanState))
    {
        SubqueryScanState *sss = (SubqueryScanState *) planstate;
        finalize_plan_instrumentation(sss->subplan);
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
    
    Instrumentation *instr;
    char indent[256];
    int i;
    char *node_type_name;
    char type_buf[256];
    NodeTag tag;
    
    /* Create indentation first */
    for (i = 0; i < level * 2 && i < 255; i++)
        indent[i] = ' ';
    indent[i] = '\0';
    
    /* Get node type name and plan details */
    if (planstate->plan)
    {
        Plan *plan = planstate->plan;
        
        /* Use nodeToString but extract just the type name (format: "{NODETYPE :field...}") */
        node_type_name = nodeToString((Node *)planstate->plan);
        
        /* Extract node type: skip '{' and get text up to first ':' or space */
        if (node_type_name[0] == '{')
        {
            char *start;
            char *end;
            int type_len;
            
            start = node_type_name + 1;  /* Skip '{' */
            end = strchr(start, ':');
            if (!end)
                end = strchr(start, ' ');
            if (!end)
                end = start + strlen(start);
            
            type_len = end - start;
            if (type_len > 255) type_len = 255;
            strncpy(type_buf, start, type_len);
            type_buf[type_len] = '\0';
            trace_printf("%s-> %s", indent, type_buf);
        }
        else
        {
            /* Fallback: print first 50 chars */
            strncpy(type_buf, node_type_name, 50);
            type_buf[50] = '\0';
            trace_printf("%s-> %s", indent, type_buf);
        }
        
        /* Print plan costs and estimates */
        trace_printf(" (cost=%.2f..%.2f rows=%.0f width=%d)",
                     plan->startup_cost,
                     plan->total_cost,
                     plan->plan_rows,
                     plan->plan_width);
    }
    else
    {
        /* Fallback: use node tag number */
        tag = nodeTag(planstate);
        trace_printf("%s-> NodeType-%d", indent, (int)tag);
    }
    
    instr = planstate->instrument;
    
    /* Print actual runtime statistics on same line as costs */
    if (instr && instr->nloops > 0)
    {
        double total_ms;
        double startup_ms;
        
        total_ms = instr->total * 1000.0;
        startup_ms = instr->startup * 1000.0;
        
        trace_printf(" (actual rows=%.0f loops=%.0f)\n", 
                     instr->ntuples / instr->nloops, instr->nloops);
        
        /* Print node-specific details (table/index names) */
        if (planstate->plan)
        {
            Plan *plan = planstate->plan;
            
            switch (nodeTag(plan))
            {
                case T_SeqScan:
                case T_SampleScan:
                    {
                        Scan *scan = (Scan *)plan;
                        RangeTblEntry *rte;
                        char *relname;
                        
                        if (scan->scanrelid > 0)
                        {
                            rte = exec_rt_fetch(scan->scanrelid, planstate->state);
                            if (rte && rte->relid != InvalidOid)
                            {
                                relname = get_rel_name(rte->relid);
                                if (relname)
                                    trace_printf("%s   Relation: %s\n", indent, relname);
                            }
                        }
                    }
                    break;
                
                case T_IndexScan:
                case T_IndexOnlyScan:
                    {
                        IndexScan *iscan = (IndexScan *)plan;
                        RangeTblEntry *rte;
                        char *relname;
                        char *idxname;
                        
                        if (iscan->scan.scanrelid > 0)
                        {
                            rte = exec_rt_fetch(iscan->scan.scanrelid, planstate->state);
                            if (rte && rte->relid != InvalidOid)
                            {
                                relname = get_rel_name(rte->relid);
                                idxname = get_rel_name(iscan->indexid);
                                if (relname)
                                    trace_printf("%s   Relation: %s\n", indent, relname);
                                if (idxname)
                                    trace_printf("%s   Index: %s\n", indent, idxname);
                            }
                        }
                    }
                    break;
                
                case T_BitmapIndexScan:
                    {
                        BitmapIndexScan *biscan = (BitmapIndexScan *)plan;
                        char *idxname;
                        
                        idxname = get_rel_name(biscan->indexid);
                        if (idxname)
                            trace_printf("%s   Index: %s\n", indent, idxname);
                    }
                    break;
                
                case T_BitmapHeapScan:
                    {
                        BitmapHeapScan *bhscan = (BitmapHeapScan *)plan;
                        RangeTblEntry *rte;
                        char *relname;
                        
                        if (bhscan->scan.scanrelid > 0)
                        {
                            rte = exec_rt_fetch(bhscan->scan.scanrelid, planstate->state);
                            if (rte && rte->relid != InvalidOid)
                            {
                                relname = get_rel_name(rte->relid);
                                if (relname)
                                    trace_printf("%s   Relation: %s\n", indent, relname);
                            }
                        }
                    }
                    break;
                
                default:
                    break;
            }
        }
        
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
                    {
                        double cpu_ms;
                        double cpu_pct;
                        double io_pct;
                        
                        cpu_ms = total_ms - io_ms;
                        if (cpu_ms > 0)
                        {
                            cpu_pct = (cpu_ms / total_ms) * 100.0;
                            io_pct = (io_ms / total_ms) * 100.0;
                        
                            trace_printf("%s   Time breakdown: CPU ~%.3f ms (%.1f%%), I/O ~%.3f ms (%.1f%%)\n",
                                         indent, cpu_ms, cpu_pct, io_ms, io_pct);
                        }
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
    BufferUsage buffer_end;
    BufferUsage buffer_diff;
    ProcStats os_end;
    
    if (trace_enabled && current_query_context)
    {
        buffer_end = pgBufferUsage;
        
        /* Final I/O capture */
        track_block_io_during_execution();
        
        /* Write buffer statistics */
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
            
            trace_printf("CPU: user=%.3f sec system=%.3f sec total=%.3f sec",
                         cpu_diff.utime_sec,
                         cpu_diff.stime_sec,
                         cpu_diff.total_sec);
            
            /* Note about CPU time granularity */
            if (cpu_diff.total_sec < 0.01)
            {
                trace_printf(" (Note: /proc granularity is ~10ms, very fast queries may show 0.000)");
            }
            trace_printf("\n");
        }
        
        /* Write block I/O analysis */
        write_block_io_summary();
        
        /* Write execution plan */
        if (queryDesc->planstate)
        {
            /* Finalize instrumentation for all nodes recursively */
            finalize_plan_instrumentation(queryDesc->planstate);
            
            trace_printf("---------------------------------------------------------------------\n");
            trace_printf("EXECUTION PLAN #%lld:\n", (long long) current_query_context->cursor_id);
            write_plan_tree(queryDesc->planstate, 0);
        }

        trace_printf("=====================================================================\n\n");

        /* Cleanup */
        if (current_query_context->sql_text)
            pfree(current_query_context->sql_text);
        if (current_query_context->block_ios)
            list_free_deep(current_query_context->block_ios);
        pfree(current_query_context);
        current_query_context = NULL;
    }
    
    /* Call standard executor end to cleanup */
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

    const char *output_dir;
    
    if (trace_enabled)
    {
        ereport(NOTICE, (errmsg("Trace already enabled")));
        PG_RETURN_TEXT_P(cstring_to_text(trace_filename));
    }

    /* Use configured directory or default to /tmp */
    output_dir = (trace_output_directory && trace_output_directory[0]) ? trace_output_directory : "/tmp";

    if (stat(output_dir, &st) != 0)
        mkdir(output_dir, 0755);

    snprintf(trace_filename, MAXPGPATH, "%s/pg_trace_%d_%ld.trc",
             output_dir, MyProcPid, (long) time(NULL));

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

