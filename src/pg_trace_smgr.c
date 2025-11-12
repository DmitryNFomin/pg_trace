/*-------------------------------------------------------------------------
 *
 * pg_trace_smgr.c
 *    Storage Manager wrapper for I/O tracing
 *
 * This wraps the default 'md' (magnetic disk) storage manager to intercept
 * and trace all I/O operations with detailed timing and context.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xlog.h"
#include "catalog/catalog.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/md.h"
#include "storage/smgr.h"
#include "utils/hsearch.h"
#include "utils/rel.h"

#include "pg_trace_smgr.h"

/* Original md storage manager functions */
static const f_smgr *original_md_smgr = NULL;

/* Tracing state */
static bool smgr_tracing_enabled = false;
static int64 current_cursor_id = 0;
static FILE *io_trace_file = NULL;

/* Forward declarations */
static void trace_smgr_init(void);
static void trace_smgr_open(SMgrRelation reln);
static void trace_smgr_close(SMgrRelation reln, ForkNumber forknum);
static void trace_smgr_create(SMgrRelation reln, ForkNumber forknum, bool isRedo);
static bool trace_smgr_exists(SMgrRelation reln, ForkNumber forknum);
static void trace_smgr_unlink(RelFileNodeBackend rnode, ForkNumber forknum, bool isRedo);
static void trace_smgr_extend(SMgrRelation reln, ForkNumber forknum,
                              BlockNumber blocknum, char *buffer, bool skipFsync);
static void trace_smgr_prefetch(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum);
static void trace_smgr_read(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
                            char *buffer);
static void trace_smgr_write(SMgrRelation reln, ForkNumber forknum,
                             BlockNumber blocknum, char *buffer, bool skipFsync);
static void trace_smgr_writeback(SMgrRelation reln, ForkNumber forknum,
                                 BlockNumber blocknum, BlockNumber nblocks);
static BlockNumber trace_smgr_nblocks(SMgrRelation reln, ForkNumber forknum);
static void trace_smgr_truncate(SMgrRelation reln, ForkNumber forknum, BlockNumber nblocks);
static void trace_smgr_immedsync(SMgrRelation reln, ForkNumber forknum);

/* Our custom storage manager */
static const f_smgr trace_smgr = {
    .smgr_init = trace_smgr_init,
    .smgr_shutdown = NULL,
    .smgr_open = trace_smgr_open,
    .smgr_close = trace_smgr_close,
    .smgr_create = trace_smgr_create,
    .smgr_exists = trace_smgr_exists,
    .smgr_unlink = trace_smgr_unlink,
    .smgr_extend = trace_smgr_extend,
    .smgr_prefetch = trace_smgr_prefetch,
    .smgr_read = trace_smgr_read,
    .smgr_write = trace_smgr_write,
    .smgr_writeback = trace_smgr_writeback,
    .smgr_nblocks = trace_smgr_nblocks,
    .smgr_truncate = trace_smgr_truncate,
    .smgr_immedsync = trace_smgr_immedsync,
};

/*
 * Initialize smgr tracing
 */
void
pg_trace_smgr_init(void)
{
    /* Save pointer to original md storage manager */
    original_md_smgr = &mdsmgr;
    
    /* Note: Actual registration would require PostgreSQL core support
     * or we need to use a different approach - see below */
}

/*
 * Enable tracing for current cursor
 */
void
pg_trace_smgr_enable(int64 cursor_id)
{
    smgr_tracing_enabled = true;
    current_cursor_id = cursor_id;
}

/*
 * Disable tracing
 */
void
pg_trace_smgr_disable(void)
{
    smgr_tracing_enabled = false;
    current_cursor_id = 0;
}

/*
 * Write I/O event to trace file
 */
void
pg_trace_write_io_event(IoTraceEvent *event)
{
    char *relname;
    const char *op_name;
    const char *fork_name;
    
    if (!io_trace_file || !event)
        return;
    
    /* Get operation name */
    switch (event->op_type)
    {
        case IO_OP_READ:
            op_name = "read";
            break;
        case IO_OP_WRITE:
            op_name = "write";
            break;
        case IO_OP_EXTEND:
            op_name = "extend";
            break;
        case IO_OP_PREFETCH:
            op_name = "prefetch";
            break;
        case IO_OP_WRITEBACK:
            op_name = "writeback";
            break;
        case IO_OP_SYNC:
            op_name = "sync";
            break;
        default:
            op_name = "unknown";
    }
    
    /* Get fork name */
    switch (event->forknum)
    {
        case MAIN_FORKNUM:
            fork_name = "main";
            break;
        case FSM_FORKNUM:
            fork_name = "fsm";
            break;
        case VISIBILITYMAP_FORKNUM:
            fork_name = "vm";
            break;
        case INIT_FORKNUM:
            fork_name = "init";
            break;
        default:
            fork_name = "unknown";
    }
    
    /* Get relation name */
    relname = pg_trace_get_relname(&event->rnode, event->forknum);
    
    /* Write in Oracle 10046 style */
    fprintf(io_trace_file,
            "WAIT #%lld: nam='db file %s' ela=%lld us "
            "file#=%u/%u/%u block#=%u blocks=%d obj#=%u fork=%s rel=%s\n",
            (long long) event->cursor_id,
            op_name,
            (long long) event->duration_us,
            event->rnode.spcNode,
            event->rnode.dbNode,
            event->rnode.relNode,
            event->blocknum,
            event->nblocks,
            event->rnode.relNode,
            fork_name,
            relname ? relname : "unknown");
    
    fflush(io_trace_file);
    
    if (relname)
        pfree(relname);
}

/*
 * Get relation name from RelFileNode
 */
char *
pg_trace_get_relname(RelFileNode *rnode, ForkNumber forknum)
{
    Oid relid = RelidByRelfilenode(rnode->spcNode, rnode->relNode);
    
    if (OidIsValid(relid))
    {
        char *relname = get_rel_name(relid);
        if (relname)
            return pstrdup(relname);
    }
    
    /* If we can't find the name, return the relfilenode */
    return psprintf("%u/%u/%u",
                    rnode->spcNode,
                    rnode->dbNode,
                    rnode->relNode);
}

/*
 * Wrapper functions - these intercept and trace, then call original
 */

static void
trace_smgr_init(void)
{
    if (original_md_smgr && original_md_smgr->smgr_init)
        original_md_smgr->smgr_init();
}

static void
trace_smgr_open(SMgrRelation reln)
{
    if (original_md_smgr)
        original_md_smgr->smgr_open(reln);
}

static void
trace_smgr_close(SMgrRelation reln, ForkNumber forknum)
{
    if (original_md_smgr)
        original_md_smgr->smgr_close(reln, forknum);
}

static void
trace_smgr_create(SMgrRelation reln, ForkNumber forknum, bool isRedo)
{
    if (original_md_smgr)
        original_md_smgr->smgr_create(reln, forknum, isRedo);
}

static bool
trace_smgr_exists(SMgrRelation reln, ForkNumber forknum)
{
    if (original_md_smgr)
        return original_md_smgr->smgr_exists(reln, forknum);
    return false;
}

static void
trace_smgr_unlink(RelFileNodeBackend rnode, ForkNumber forknum, bool isRedo)
{
    if (original_md_smgr)
        original_md_smgr->smgr_unlink(rnode, forknum, isRedo);
}

static void
trace_smgr_extend(SMgrRelation reln, ForkNumber forknum,
                  BlockNumber blocknum, char *buffer, bool skipFsync)
{
    instr_time start, end;
    IoTraceEvent event;
    
    if (smgr_tracing_enabled)
    {
        INSTR_TIME_SET_CURRENT(start);
    }
    
    /* Call original */
    if (original_md_smgr)
        original_md_smgr->smgr_extend(reln, forknum, blocknum, buffer, skipFsync);
    
    if (smgr_tracing_enabled)
    {
        INSTR_TIME_SET_CURRENT(end);
        INSTR_TIME_SUBTRACT(end, start);
        
        /* Log the extend operation */
        memset(&event, 0, sizeof(IoTraceEvent));
        event.timestamp = GetCurrentTimestamp();
        event.cursor_id = current_cursor_id;
        event.rnode = reln->smgr_rnode.node;
        event.forknum = forknum;
        event.blocknum = blocknum;
        event.op_type = IO_OP_EXTEND;
        event.duration_us = (int64) (INSTR_TIME_GET_MICROSEC(end));
        event.nblocks = 1;
        
        pg_trace_write_io_event(&event);
    }
}

static void
trace_smgr_prefetch(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum)
{
    instr_time start, end;
    IoTraceEvent event;
    
    if (smgr_tracing_enabled)
    {
        INSTR_TIME_SET_CURRENT(start);
    }
    
    /* Call original */
    if (original_md_smgr && original_md_smgr->smgr_prefetch)
        original_md_smgr->smgr_prefetch(reln, forknum, blocknum);
    
    if (smgr_tracing_enabled)
    {
        INSTR_TIME_SET_CURRENT(end);
        INSTR_TIME_SUBTRACT(end, start);
        
        memset(&event, 0, sizeof(IoTraceEvent));
        event.timestamp = GetCurrentTimestamp();
        event.cursor_id = current_cursor_id;
        event.rnode = reln->smgr_rnode.node;
        event.forknum = forknum;
        event.blocknum = blocknum;
        event.op_type = IO_OP_PREFETCH;
        event.duration_us = (int64) (INSTR_TIME_GET_MICROSEC(end));
        event.nblocks = 1;
        
        pg_trace_write_io_event(&event);
    }
}

static void
trace_smgr_read(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
                char *buffer)
{
    instr_time start, end;
    IoTraceEvent event;
    
    if (smgr_tracing_enabled)
    {
        INSTR_TIME_SET_CURRENT(start);
    }
    
    /* Call original */
    if (original_md_smgr)
        original_md_smgr->smgr_read(reln, forknum, blocknum, buffer);
    
    if (smgr_tracing_enabled)
    {
        INSTR_TIME_SET_CURRENT(end);
        INSTR_TIME_SUBTRACT(end, start);
        
        memset(&event, 0, sizeof(IoTraceEvent));
        event.timestamp = GetCurrentTimestamp();
        event.cursor_id = current_cursor_id;
        event.rnode = reln->smgr_rnode.node;
        event.forknum = forknum;
        event.blocknum = blocknum;
        event.op_type = IO_OP_READ;
        event.duration_us = (int64) (INSTR_TIME_GET_MICROSEC(end));
        event.nblocks = 1;
        
        pg_trace_write_io_event(&event);
    }
}

static void
trace_smgr_write(SMgrRelation reln, ForkNumber forknum,
                 BlockNumber blocknum, char *buffer, bool skipFsync)
{
    instr_time start, end;
    IoTraceEvent event;
    
    if (smgr_tracing_enabled)
    {
        INSTR_TIME_SET_CURRENT(start);
    }
    
    /* Call original */
    if (original_md_smgr)
        original_md_smgr->smgr_write(reln, forknum, blocknum, buffer, skipFsync);
    
    if (smgr_tracing_enabled)
    {
        INSTR_TIME_SET_CURRENT(end);
        INSTR_TIME_SUBTRACT(end, start);
        
        memset(&event, 0, sizeof(IoTraceEvent));
        event.timestamp = GetCurrentTimestamp();
        event.cursor_id = current_cursor_id;
        event.rnode = reln->smgr_rnode.node;
        event.forknum = forknum;
        event.blocknum = blocknum;
        event.op_type = IO_OP_WRITE;
        event.duration_us = (int64) (INSTR_TIME_GET_MICROSEC(end));
        event.nblocks = 1;
        
        pg_trace_write_io_event(&event);
    }
}

static void
trace_smgr_writeback(SMgrRelation reln, ForkNumber forknum,
                     BlockNumber blocknum, BlockNumber nblocks)
{
    instr_time start, end;
    IoTraceEvent event;
    
    if (smgr_tracing_enabled)
    {
        INSTR_TIME_SET_CURRENT(start);
    }
    
    /* Call original */
    if (original_md_smgr && original_md_smgr->smgr_writeback)
        original_md_smgr->smgr_writeback(reln, forknum, blocknum, nblocks);
    
    if (smgr_tracing_enabled)
    {
        INSTR_TIME_SET_CURRENT(end);
        INSTR_TIME_SUBTRACT(end, start);
        
        memset(&event, 0, sizeof(IoTraceEvent));
        event.timestamp = GetCurrentTimestamp();
        event.cursor_id = current_cursor_id;
        event.rnode = reln->smgr_rnode.node;
        event.forknum = forknum;
        event.blocknum = blocknum;
        event.op_type = IO_OP_WRITEBACK;
        event.duration_us = (int64) (INSTR_TIME_GET_MICROSEC(end));
        event.nblocks = nblocks;
        
        pg_trace_write_io_event(&event);
    }
}

static BlockNumber
trace_smgr_nblocks(SMgrRelation reln, ForkNumber forknum)
{
    if (original_md_smgr)
        return original_md_smgr->smgr_nblocks(reln, forknum);
    return 0;
}

static void
trace_smgr_truncate(SMgrRelation reln, ForkNumber forknum, BlockNumber nblocks)
{
    if (original_md_smgr)
        original_md_smgr->smgr_truncate(reln, forknum, nblocks);
}

static void
trace_smgr_immedsync(SMgrRelation reln, ForkNumber forknum)
{
    instr_time start, end;
    IoTraceEvent event;
    
    if (smgr_tracing_enabled)
    {
        INSTR_TIME_SET_CURRENT(start);
    }
    
    /* Call original */
    if (original_md_smgr)
        original_md_smgr->smgr_immedsync(reln, forknum);
    
    if (smgr_tracing_enabled)
    {
        INSTR_TIME_SET_CURRENT(end);
        INSTR_TIME_SUBTRACT(end, start);
        
        memset(&event, 0, sizeof(IoTraceEvent));
        event.timestamp = GetCurrentTimestamp();
        event.cursor_id = current_cursor_id;
        event.rnode = reln->smgr_rnode.node;
        event.forknum = forknum;
        event.blocknum = 0;
        event.op_type = IO_OP_SYNC;
        event.duration_us = (int64) (INSTR_TIME_GET_MICROSEC(end));
        event.nblocks = 0;
        
        pg_trace_write_io_event(&event);
    }
}

/*
 * Set trace file handle (called from main extension)
 */
void
pg_trace_smgr_set_tracefile(FILE *file)
{
    io_trace_file = file;
}

