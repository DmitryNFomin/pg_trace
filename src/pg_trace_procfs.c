/*-------------------------------------------------------------------------
 *
 * pg_trace_procfs.c
 *    Implementation of /proc filesystem statistics collection
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "pg_trace_procfs.h"

/* System clock ticks per second */
static long clock_ticks_per_sec = 0;

/*
 * Initialize clock ticks (call once)
 */
static inline void
init_clock_ticks(void)
{
    if (clock_ticks_per_sec == 0)
        clock_ticks_per_sec = sysconf(_SC_CLK_TCK);
}

/*
 * Convert clock ticks to seconds
 */
static inline double
ticks_to_seconds(unsigned long ticks)
{
    init_clock_ticks();
    return (double) ticks / (double) clock_ticks_per_sec;
}

/*
 * Read CPU statistics from /proc/[pid]/stat
 *
 * Format: pid (comm) state ppid pgrp session tty_nr tpgid flags minflt cminflt
 *         majflt cmajflt utime stime cutime cstime ...
 */
bool
proc_read_cpu_stats(pid_t pid, ProcCpuStats *stats)
{
    char path[256];
    FILE *fp;
    char line[2048];
    int n;
    
    if (!stats)
        return false;
    
    memset(stats, 0, sizeof(ProcCpuStats));
    
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    fp = fopen(path, "r");
    
    if (!fp)
        return false;
    
    if (!fgets(line, sizeof(line), fp))
    {
        fclose(fp);
        return false;
    }
    
    fclose(fp);
    
    /* Parse the stat line - fields are space separated */
    /* We need fields 14-17 (utime, stime, cutime, cstime) */
    /* Skip first field (PID) and second field (comm which may contain spaces) */
    
    char *p = strchr(line, ')');  /* Find end of comm field */
    if (!p)
        return false;
    
    p += 2;  /* Skip ") " */
    
    /* Now parse remaining fields */
    unsigned long fields[50];
    n = sscanf(p, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u "
                  "%lu %lu %lu %lu",  /* fields 14-17 */
                  &fields[0], &fields[1], &fields[2], &fields[3]);
    
    if (n < 4)
        return false;
    
    stats->utime = fields[0];
    stats->stime = fields[1];
    stats->cutime = fields[2];
    stats->cstime = fields[3];
    
    /* Calculate derived fields */
    stats->utime_sec = ticks_to_seconds(stats->utime);
    stats->stime_sec = ticks_to_seconds(stats->stime);
    stats->total_sec = stats->utime_sec + stats->stime_sec;
    
    return true;
}

/*
 * Read I/O statistics from /proc/[pid]/io
 */
bool
proc_read_io_stats(pid_t pid, ProcIoStats *stats)
{
    char path[256];
    FILE *fp;
    char line[256];
    
    if (!stats)
        return false;
    
    memset(stats, 0, sizeof(ProcIoStats));
    
    snprintf(path, sizeof(path), "/proc/%d/io", pid);
    fp = fopen(path, "r");
    
    if (!fp)
        return false;  /* May not have permission */
    
    /* Parse each line */
    while (fgets(line, sizeof(line), fp))
    {
        if (sscanf(line, "rchar: %llu", &stats->rchar) == 1)
            continue;
        if (sscanf(line, "wchar: %llu", &stats->wchar) == 1)
            continue;
        if (sscanf(line, "syscr: %llu", &stats->syscr) == 1)
            continue;
        if (sscanf(line, "syscw: %llu", &stats->syscw) == 1)
            continue;
        if (sscanf(line, "read_bytes: %llu", &stats->read_bytes) == 1)
            continue;
        if (sscanf(line, "write_bytes: %llu", &stats->write_bytes) == 1)
            continue;
        if (sscanf(line, "cancelled_write_bytes: %llu", 
                   &stats->cancelled_write_bytes) == 1)
            continue;
    }
    
    fclose(fp);
    return true;
}

/*
 * Read memory statistics from /proc/[pid]/status
 */
bool
proc_read_mem_stats(pid_t pid, ProcMemStats *stats)
{
    char path[256];
    FILE *fp;
    char line[256];
    
    if (!stats)
        return false;
    
    memset(stats, 0, sizeof(ProcMemStats));
    
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    fp = fopen(path, "r");
    
    if (!fp)
        return false;
    
    /* Parse each line */
    while (fgets(line, sizeof(line), fp))
    {
        if (sscanf(line, "VmPeak: %lu kB", &stats->vm_peak_kb) == 1)
            continue;
        if (sscanf(line, "VmSize: %lu kB", &stats->vm_size_kb) == 1)
            continue;
        if (sscanf(line, "VmRSS: %lu kB", &stats->vm_rss_kb) == 1)
            continue;
    }
    
    fclose(fp);
    return true;
}

/*
 * Read all statistics at once
 */
bool
proc_read_all_stats(pid_t pid, ProcStats *stats)
{
    bool ok = true;
    
    if (!stats)
        return false;
    
    memset(stats, 0, sizeof(ProcStats));
    
    ok &= proc_read_cpu_stats(pid, &stats->cpu);
    ok &= proc_read_io_stats(pid, &stats->io);
    ok &= proc_read_mem_stats(pid, &stats->mem);
    
    stats->valid = ok;
    return ok;
}

/*
 * Calculate difference between two CPU stat snapshots
 */
void
proc_cpu_stats_diff(const ProcCpuStats *start, 
                    const ProcCpuStats *end,
                    ProcCpuStats *diff)
{
    if (!start || !end || !diff)
        return;
    
    diff->utime = end->utime - start->utime;
    diff->stime = end->stime - start->stime;
    diff->cutime = end->cutime - start->cutime;
    diff->cstime = end->cstime - start->cstime;
    
    diff->utime_sec = ticks_to_seconds(diff->utime);
    diff->stime_sec = ticks_to_seconds(diff->stime);
    diff->total_sec = diff->utime_sec + diff->stime_sec;
}

/*
 * Calculate difference between two I/O stat snapshots
 */
void
proc_io_stats_diff(const ProcIoStats *start,
                   const ProcIoStats *end,
                   ProcIoStats *diff)
{
    if (!start || !end || !diff)
        return;
    
    diff->rchar = end->rchar - start->rchar;
    diff->wchar = end->wchar - start->wchar;
    diff->syscr = end->syscr - start->syscr;
    diff->syscw = end->syscw - start->syscw;
    diff->read_bytes = end->read_bytes - start->read_bytes;
    diff->write_bytes = end->write_bytes - start->write_bytes;
    diff->cancelled_write_bytes = end->cancelled_write_bytes - 
                                  start->cancelled_write_bytes;
}

/*
 * Format CPU statistics as string (Oracle 10046 style)
 */
char *
proc_format_cpu_stats(const ProcCpuStats *stats)
{
    StringInfoData buf;
    
    if (!stats)
        return pstrdup("");
    
    initStringInfo(&buf);
    
    appendStringInfo(&buf, "c=%.0f", stats->total_sec * 1000000.0);  /* microseconds */
    appendStringInfo(&buf, " (user=%.3f sys=%.3f)", 
                     stats->utime_sec, stats->stime_sec);
    
    return buf.data;
}

/*
 * Format I/O statistics as string
 */
char *
proc_format_io_stats(const ProcIoStats *stats)
{
    StringInfoData buf;
    
    if (!stats)
        return pstrdup("");
    
    initStringInfo(&buf);
    
    appendStringInfo(&buf, "io_read=%llu io_write=%llu",
                     stats->read_bytes, stats->write_bytes);
    appendStringInfo(&buf, " syscalls_r=%llu syscalls_w=%llu",
                     stats->syscr, stats->syscw);
    
    return buf.data;
}

/*
 * Format memory statistics as string
 */
char *
proc_format_mem_stats(const ProcMemStats *stats)
{
    StringInfoData buf;
    
    if (!stats)
        return pstrdup("");
    
    initStringInfo(&buf);
    
    appendStringInfo(&buf, "mem_rss=%lu KB mem_peak=%lu KB",
                     stats->vm_rss_kb, stats->vm_peak_kb);
    
    return buf.data;
}

