#!/usr/bin/env python3
"""
pg_trace_waits.py - eBPF-based wait event tracer for PostgreSQL

This script uses eBPF to trace PostgreSQL wait events with precise timing.
It integrates with the pg_trace extension to provide Oracle 10046-style
wait event logging.

Requirements:
    - Linux kernel 4.9+
    - BCC (BPF Compiler Collection)
    - Root privileges or CAP_BPF capability

Usage:
    sudo python3 pg_trace_waits.py -p <postgres_pid>
"""

from bcc import BPF
import argparse
import sys
import ctypes
import time
from datetime import datetime

# eBPF program
bpf_program = """
#include <uapi/linux/ptrace.h>

// Structure to pass wait event data to user space
struct wait_event_t {
    u64 timestamp_ns;
    u32 pid;
    u32 tid;
    u64 cursor_id;
    u32 wait_event_info;
    u64 duration_ns;
    char wait_name[64];
};

// Hash map to track wait start times
BPF_HASH(wait_starts, u32, u64);

// Hash map to lookup cursor_id from PID (populated by extension)
BPF_HASH(pid_to_cursor, u32, u64);

// Perf output for sending events to user space
BPF_PERF_OUTPUT(wait_events);

// Trace pgstat_report_wait_start
int trace_wait_start(struct pt_regs *ctx, u32 wait_event_info) {
    u64 pid_tid = bpf_get_current_pid_tgid();
    u32 tid = (u32)pid_tid;
    u64 ts = bpf_ktime_get_ns();
    
    // Store start time
    wait_starts.update(&tid, &ts);
    
    return 0;
}

// Trace pgstat_report_wait_end
int trace_wait_end(struct pt_regs *ctx) {
    u64 pid_tid = bpf_get_current_pid_tgid();
    u32 pid = pid_tid >> 32;
    u32 tid = (u32)pid_tid;
    
    // Look up start time
    u64 *start_ts = wait_starts.lookup(&tid);
    if (!start_ts)
        return 0;
    
    u64 end_ts = bpf_ktime_get_ns();
    u64 duration_ns = end_ts - *start_ts;
    
    // Only report waits > 1 microsecond (reduce noise)
    if (duration_ns < 1000)
        goto cleanup;
    
    // Create event
    struct wait_event_t event = {};
    event.timestamp_ns = end_ts;
    event.pid = pid;
    event.tid = tid;
    event.duration_ns = duration_ns;
    
    // Look up cursor_id (if extension has registered one)
    u64 *cursor_id = pid_to_cursor.lookup(&pid);
    if (cursor_id)
        event.cursor_id = *cursor_id;
    
    // Submit to user space
    wait_events.perf_submit(ctx, &event, sizeof(event));
    
cleanup:
    wait_starts.delete(&tid);
    return 0;
}

// Trace specific I/O operations for detailed stats
int trace_buffer_read_start(struct pt_regs *ctx) {
    u64 pid_tid = bpf_get_current_pid_tgid();
    u32 tid = (u32)pid_tid;
    u64 ts = bpf_ktime_get_ns();
    
    wait_starts.update(&tid, &ts);
    return 0;
}

int trace_buffer_read_done(struct pt_regs *ctx) {
    return trace_wait_end(ctx);
}
"""

# Wait event names (subset from PostgreSQL)
WAIT_EVENT_NAMES = {
    0x01000001: 'LockManager',
    0x02000001: 'BufferPin',
    0x03000001: 'ActivityMain',
    0x03000002: 'ActivityAutovacuum',
    0x04000001: 'ClientRead',
    0x04000002: 'ClientWrite',
    0x05000001: 'DataFileRead',
    0x05000002: 'DataFileWrite',
    0x05000003: 'DataFileExtend',
    0x05000004: 'DataFileFlush',
    0x05000005: 'DataFileSync',
    0x05000006: 'WALWrite',
    0x05000007: 'WALSync',
    0x06000001: 'MessageQueueSend',
    0x06000002: 'MessageQueueReceive',
}

def get_wait_event_name(wait_event_info):
    """Get human-readable wait event name"""
    return WAIT_EVENT_NAMES.get(wait_event_info, f'Unknown:0x{wait_event_info:08x}')

def print_wait_event(cpu, data, size):
    """Callback for each wait event"""
    event = ctypes.cast(data, ctypes.POINTER(WaitEvent)).contents
    
    timestamp = datetime.now().strftime('%Y-%m-%d %H:%M:%S.%f')[:-3]
    duration_us = event.duration_ns / 1000
    wait_name = get_wait_event_name(event.wait_event_info)
    
    # Format similar to Oracle 10046
    if event.cursor_id > 0:
        print(f"WAIT #{event.cursor_id}: nam='{wait_name}' ela={duration_us:.0f} us tim={timestamp}")
    else:
        print(f"WAIT [PID {event.pid}]: nam='{wait_name}' ela={duration_us:.0f} us tim={timestamp}")
    
    sys.stdout.flush()

# Define wait event structure for Python
class WaitEvent(ctypes.Structure):
    _fields_ = [
        ("timestamp_ns", ctypes.c_uint64),
        ("pid", ctypes.c_uint32),
        ("tid", ctypes.c_uint32),
        ("cursor_id", ctypes.c_uint64),
        ("wait_event_info", ctypes.c_uint32),
        ("duration_ns", ctypes.c_uint64),
        ("wait_name", ctypes.c_char * 64),
    ]

def find_postgres_binary():
    """Find PostgreSQL binary path"""
    import subprocess
    try:
        result = subprocess.run(['which', 'postgres'], 
                              capture_output=True, text=True, check=True)
        return result.stdout.strip()
    except:
        # Try common locations
        for path in ['/usr/pgsql-14/bin/postgres', 
                    '/usr/lib/postgresql/14/bin/postgres',
                    '/usr/local/pgsql/bin/postgres']:
            import os
            if os.path.exists(path):
                return path
    return None

def main():
    parser = argparse.ArgumentParser(description='Trace PostgreSQL wait events with eBPF')
    parser.add_argument('-p', '--pid', type=int, required=True,
                       help='PostgreSQL backend process ID to trace')
    parser.add_argument('-v', '--verbose', action='store_true',
                       help='Verbose output')
    parser.add_argument('-o', '--output', type=str,
                       help='Output file (default: stdout)')
    args = parser.parse_args()

    # Find PostgreSQL binary
    postgres_bin = find_postgres_binary()
    if not postgres_bin:
        print("Error: Could not find postgres binary", file=sys.stderr)
        print("Please specify pg_config or postgres path", file=sys.stderr)
        sys.exit(1)
    
    if args.verbose:
        print(f"Using PostgreSQL binary: {postgres_bin}")
        print(f"Tracing PID: {args.pid}")

    # Redirect output if requested
    if args.output:
        sys.stdout = open(args.output, 'w', buffering=1)

    # Load eBPF program
    try:
        b = BPF(text=bpf_program)
    except Exception as e:
        print(f"Error loading eBPF program: {e}", file=sys.stderr)
        print("Make sure you have BCC installed and running as root", file=sys.stderr)
        sys.exit(1)

    # Attach to PostgreSQL functions
    try:
        b.attach_uprobe(name=postgres_bin,
                       sym="pgstat_report_wait_start",
                       fn_name="trace_wait_start",
                       pid=args.pid)
        b.attach_uprobe(name=postgres_bin,
                       sym="pgstat_report_wait_end",
                       fn_name="trace_wait_end",
                       pid=args.pid)
        
        if args.verbose:
            print("Attached to pgstat_report_wait_start/end")
    except Exception as e:
        print(f"Error attaching to PostgreSQL: {e}", file=sys.stderr)
        print("Make sure:", file=sys.stderr)
        print("  1. PID is correct and process is running", file=sys.stderr)
        print("  2. Binary has symbols (not stripped)", file=sys.stderr)
        print("  3. You have root privileges", file=sys.stderr)
        sys.exit(1)

    print(f"=== Tracing PostgreSQL wait events for PID {args.pid} ===")
    print("Press Ctrl-C to stop")
    print()

    # Open perf buffer
    b["wait_events"].open_perf_buffer(print_wait_event)

    # Poll for events
    try:
        while True:
            b.perf_buffer_poll()
    except KeyboardInterrupt:
        print("\n=== Trace stopped ===")

if __name__ == '__main__':
    main()


