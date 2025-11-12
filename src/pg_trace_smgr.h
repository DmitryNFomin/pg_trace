/*-------------------------------------------------------------------------
 *
 * pg_trace_smgr.h
 *    Storage Manager wrapper for detailed I/O tracing
 *
 * This provides Oracle 10046-style I/O tracing by wrapping the storage
 * manager layer. We intercept all I/O operations (read, write, extend)
 * and log them with file#, block#, and timing.
 *
 * This approach is better than eBPF because:
 * - No root required
 * - Access to PostgreSQL context (relation OID, fork type)
 * - Precise block-level detail
 * - Can correlate with table/index names
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TRACE_SMGR_H
#define PG_TRACE_SMGR_H

#include "postgres.h"
#include "storage/smgr.h"
#include "utils/timestamp.h"

/* I/O operation types */
typedef enum IoOpType
{
    IO_OP_READ,
    IO_OP_WRITE,
    IO_OP_EXTEND,
    IO_OP_PREFETCH,
    IO_OP_WRITEBACK,
    IO_OP_SYNC
} IoOpType;

/* I/O trace event */
typedef struct IoTraceEvent
{
    TimestampTz timestamp;
    int64 cursor_id;            /* Associated query cursor */
    RelFileNode rnode;          /* Relation file node */
    ForkNumber forknum;         /* Fork type (main, fsm, vm) */
    BlockNumber blocknum;       /* Block number */
    IoOpType op_type;           /* Read, write, extend, etc. */
    int64 duration_us;          /* Duration in microseconds */
    int nblocks;                /* Number of blocks (for extend) */
    bool hit;                   /* Buffer hit (for reads) */
} IoTraceEvent;

/* Initialize smgr tracing */
extern void pg_trace_smgr_init(void);

/* Enable/disable tracing */
extern void pg_trace_smgr_enable(int64 cursor_id);
extern void pg_trace_smgr_disable(void);

/* Write I/O event to trace */
extern void pg_trace_write_io_event(IoTraceEvent *event);

/* Get relation name from RelFileNode */
extern char *pg_trace_get_relname(RelFileNode *rnode, ForkNumber forknum);

#endif /* PG_TRACE_SMGR_H */

