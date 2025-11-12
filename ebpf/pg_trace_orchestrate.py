#!/usr/bin/env python3
"""
pg_trace_orchestrate.py - Orchestrator for pg_trace MVP

This script coordinates the extension output with eBPF wait event tracing
to produce a unified Oracle 10046-style trace file.

Usage:
    sudo python3 pg_trace_orchestrate.py -p <postgres_pid> -f <trace_file>
"""

import argparse
import subprocess
import sys
import os
import time
import signal
import threading
from pathlib import Path

def tail_trace_file(trace_file, output_file):
    """Tail the trace file from extension"""
    try:
        with open(trace_file, 'r') as tf:
            # Seek to end initially
            tf.seek(0, 2)
            
            while True:
                line = tf.readline()
                if line:
                    output_file.write(line)
                    output_file.flush()
                else:
                    time.sleep(0.1)
    except Exception as e:
        print(f"Error tailing trace file: {e}", file=sys.stderr)

def merge_traces(extension_file, wait_file, output_file):
    """Merge extension trace with wait events"""
    print(f"Merging traces into: {output_file}")
    
    with open(output_file, 'w') as out:
        # Write extension trace
        if os.path.exists(extension_file):
            with open(extension_file, 'r') as ef:
                out.write(ef.read())
        
        # Append wait events
        if os.path.exists(wait_file):
            out.write("\n" + "="*70 + "\n")
            out.write("WAIT EVENTS (from eBPF)\n")
            out.write("="*70 + "\n\n")
            with open(wait_file, 'r') as wf:
                out.write(wf.read())
    
    print(f"Trace complete: {output_file}")

def main():
    parser = argparse.ArgumentParser(description='Orchestrate pg_trace with eBPF')
    parser.add_argument('-p', '--pid', type=int, required=True,
                       help='PostgreSQL backend PID')
    parser.add_argument('-f', '--trace-file', type=str, required=True,
                       help='Trace file from pg_trace extension')
    parser.add_argument('-o', '--output', type=str,
                       help='Final output file (default: <trace_file>.complete)')
    parser.add_argument('--wait-only', action='store_true',
                       help='Only run wait event tracer')
    args = parser.parse_args()

    if not os.path.exists(args.trace_file):
        print(f"Error: Trace file not found: {args.trace_file}", file=sys.stderr)
        print("Have you called pg_trace_start_trace() in PostgreSQL?", file=sys.stderr)
        sys.exit(1)

    output_file = args.output or f"{args.trace_file}.complete"
    wait_file = f"/tmp/pg_trace_waits_{args.pid}.log"

    print("="*70)
    print("PostgreSQL Oracle 10046-Style Trace (MVP)")
    print("="*70)
    print(f"PID: {args.pid}")
    print(f"Extension trace: {args.trace_file}")
    print(f"Wait events: {wait_file}")
    print(f"Combined output: {output_file}")
    print("="*70)
    print()

    # Start eBPF wait tracer
    ebpf_script = Path(__file__).parent / 'pg_trace_waits.py'
    if not ebpf_script.exists():
        print(f"Error: eBPF script not found: {ebpf_script}", file=sys.stderr)
        sys.exit(1)

    print(f"Starting eBPF wait event tracer...")
    wait_process = subprocess.Popen(
        ['python3', str(ebpf_script), '-p', str(args.pid), '-o', wait_file],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE
    )

    # Give eBPF time to attach
    time.sleep(2)

    if wait_process.poll() is not None:
        _, stderr = wait_process.communicate()
        print(f"Error starting eBPF tracer:", file=sys.stderr)
        print(stderr.decode(), file=sys.stderr)
        sys.exit(1)

    print("eBPF tracer running")
    print()
    print("Monitoring trace file...")
    print("Press Ctrl-C when done")
    print()

    # Monitor trace file and show progress
    try:
        last_size = 0
        while True:
            if os.path.exists(args.trace_file):
                size = os.path.getsize(args.trace_file)
                if size != last_size:
                    print(f"\rTrace file size: {size} bytes", end='', flush=True)
                    last_size = size
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n\nStopping trace...")
        wait_process.send_signal(signal.SIGINT)
        wait_process.wait()

    # Merge traces
    print("\nMerging trace files...")
    merge_traces(args.trace_file, wait_file, output_file)
    
    # Cleanup
    if os.path.exists(wait_file):
        os.remove(wait_file)

if __name__ == '__main__':
    if os.geteuid() != 0:
        print("Error: This script must be run as root (for eBPF)", file=sys.stderr)
        sys.exit(1)
    main()


