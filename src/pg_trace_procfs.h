/*-------------------------------------------------------------------------
 *
 * pg_trace_procfs.h
 *    /proc filesystem utilities for CPU and I/O statistics
 *
 * This provides a simple way to collect OS-level statistics without
 * requiring eBPF or root privileges.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TRACE_PROCFS_H
#define PG_TRACE_PROCFS_H

#include <unistd.h>
#include <sys/types.h>

/* CPU statistics from /proc/[pid]/stat */
typedef struct ProcCpuStats
{
    unsigned long utime;        /* User CPU time in clock ticks */
    unsigned long stime;        /* System CPU time in clock ticks */
    unsigned long cutime;       /* Children user time */
    unsigned long cstime;       /* Children system time */
    
    /* Calculated fields */
    double utime_sec;           /* User time in seconds */
    double stime_sec;           /* System time in seconds */
    double total_sec;           /* Total CPU time in seconds */
} ProcCpuStats;

/* I/O statistics from /proc/[pid]/io */
typedef struct ProcIoStats
{
    unsigned long long rchar;           /* Bytes read (all) */
    unsigned long long wchar;           /* Bytes written (all) */
    unsigned long long syscr;           /* Read syscalls */
    unsigned long long syscw;           /* Write syscalls */
    unsigned long long read_bytes;      /* Storage I/O read */
    unsigned long long write_bytes;     /* Storage I/O write */
    unsigned long long cancelled_write_bytes;
} ProcIoStats;

/* Memory statistics from /proc/[pid]/status */
typedef struct ProcMemStats
{
    unsigned long vm_peak_kb;   /* Peak virtual memory */
    unsigned long vm_size_kb;   /* Current virtual memory */
    unsigned long vm_rss_kb;    /* Resident set size */
} ProcMemStats;

/* Combined statistics snapshot */
typedef struct ProcStats
{
    ProcCpuStats cpu;
    ProcIoStats io;
    ProcMemStats mem;
    bool valid;
} ProcStats;

/* Function declarations */
extern bool proc_read_cpu_stats(pid_t pid, ProcCpuStats *stats);
extern bool proc_read_io_stats(pid_t pid, ProcIoStats *stats);
extern bool proc_read_mem_stats(pid_t pid, ProcMemStats *stats);
extern bool proc_read_all_stats(pid_t pid, ProcStats *stats);

extern void proc_cpu_stats_diff(const ProcCpuStats *start, 
                                 const ProcCpuStats *end,
                                 ProcCpuStats *diff);
extern void proc_io_stats_diff(const ProcIoStats *start,
                                const ProcIoStats *end,
                                ProcIoStats *diff);

/* Formatting helpers */
extern char *proc_format_cpu_stats(const ProcCpuStats *stats);
extern char *proc_format_io_stats(const ProcIoStats *stats);
extern char *proc_format_mem_stats(const ProcMemStats *stats);

#endif /* PG_TRACE_PROCFS_H */

