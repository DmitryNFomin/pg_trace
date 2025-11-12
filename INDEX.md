# pg_trace - File Index

## 📁 Project Structure

```
/Users/dmitryfomin/work/git/pg_trace/
│
├── 📚 Documentation
│   ├── README.md                  ← ⭐ Main project readme (START HERE!)
│   ├── QUICK_REFERENCE.md         ← One-page cheat sheet
│   ├── ULTIMATE_README.md         ← Complete guide to Ultimate version
│   ├── PER_NODE_STATS.md          ← Per-node statistics explained
│   ├── HOW_IT_WORKS.md            ← Technical deep dive
│   ├── FINAL_SUMMARY.md           ← All three versions compared
│   └── INDEX.md                   ← This file (file navigation)
│
├── 🔧 Source Code - Ultimate (RECOMMENDED)
│   ├── src/pg_trace_ultimate.c         ⭐ Main extension - per-block I/O
│   ├── src/pg_trace_procfs.c           ← /proc reader (CPU/I/O)
│   ├── src/pg_trace_procfs.h           ← /proc reader header
│   ├── sql/pg_trace_ultimate--1.0.sql  ← SQL interface
│   ├── pg_trace_ultimate.control       ← Extension metadata
│   └── Makefile.ultimate               ← Build file
│
├── 🔧 Source Code - Enhanced
│   ├── src/pg_trace_enhanced.c         ← Extension + /proc stats
│   ├── sql/pg_trace_enhanced--1.0.sql  ← SQL interface
│   ├── pg_trace_enhanced.control       ← Extension metadata
│   └── Makefile.enhanced               ← Build file
│
├── 🔧 Source Code - Basic MVP
│   ├── src/pg_trace_mvp.c              ← Minimal extension
│   ├── sql/pg_trace_mvp--1.0.sql       ← SQL interface
│   ├── pg_trace_mvp.control            ← Extension metadata
│   └── Makefile.mvp                    ← Build file
│
├── 🐍 eBPF Scripts (Optional, requires root)
│   ├── ebpf/pg_trace_waits.py          ← Wait event tracing
│   └── ebpf/pg_trace_orchestrate.py    ← Combines extension + eBPF
│
└── 🗑️ Generated at Runtime
    └── /tmp/pg_trace/                  ← Trace output files
        └── pg_trace_<pid>_<ts>.trc     ← Individual traces
```

---

## 📖 Documentation Guide

### Quick Start (5 minutes)
**Read:** `QUICK_REFERENCE.md`
- One-page summary
- 3-command installation
- 3-command usage
- Common troubleshooting

### Full Setup (15 minutes)
**Read:** `ULTIMATE_README.md`
- Complete installation guide
- Detailed configuration
- Use cases and examples
- Performance tuning

### Understanding the Solution (30 minutes)
**Read:** `HOW_IT_WORKS.md`
- Technical architecture
- Algorithm explanation
- Three-tier cache analysis
- Overhead breakdown

### Choosing a Version (10 minutes)
**Read:** `FINAL_SUMMARY.md`
- All three versions compared
- Feature matrix
- Performance comparison
- Which to use when

---

## 🔍 Finding Information

### "How do I install this?"
→ `QUICK_REFERENCE.md` (Installation section)
→ `ULTIMATE_README.md` (Installation section)

### "Which version should I use?"
→ `FINAL_SUMMARY.md` (Which Version Should You Use)
→ `QUICK_REFERENCE.md` (bottom of page)

### "How does it work internally?"
→ `HOW_IT_WORKS.md` (Architecture section)
→ Source code: `src/pg_trace_ultimate.c`

### "What's the overhead?"
→ `QUICK_REFERENCE.md` (Performance Guidelines)
→ `HOW_IT_WORKS.md` (Q3: What is the overhead?)
→ `ULTIMATE_README.md` (Performance Impact)

### "How to interpret results?"
→ `QUICK_REFERENCE.md` (Interpreting Results)
→ `ULTIMATE_README.md` (Use Cases)

### "What's different from Oracle?"
→ `QUICK_REFERENCE.md` (vs Oracle 10046)
→ `ULTIMATE_README.md` (Comparing with Oracle)

### "How do per-node statistics work?"
→ `PER_NODE_STATS.md` (Complete guide)

---

## 🎯 File Purpose Quick Lookup

| File Name | Purpose | Read If... |
|-----------|---------|-----------|
| `README.md` | Main project readme | You're new to the project |
| `QUICK_REFERENCE.md` | One-page cheat sheet | You want to get started NOW |
| `ULTIMATE_README.md` | Complete guide | You're using Ultimate version |
| `PER_NODE_STATS.md` | Per-node statistics | You want to understand per-node details |
| `HOW_IT_WORKS.md` | Technical deep dive | You want to understand internals |
| `FINAL_SUMMARY.md` | All versions compared | You're choosing which version |
| `INDEX.md` | File navigation | You're looking for a specific file |

---

## 🚀 Most Important Files (Start Here)

### To USE the tool:
1. `README.md` ⭐⭐⭐
2. `QUICK_REFERENCE.md` ⭐⭐
3. `Makefile.ultimate`
4. `src/pg_trace_ultimate.c` (source)

### To UNDERSTAND the tool:
1. `HOW_IT_WORKS.md` ⭐⭐⭐
2. `FINAL_SUMMARY.md`
3. Source code

### To CHOOSE a version:
1. `FINAL_SUMMARY.md` ⭐⭐⭐
2. `QUICK_REFERENCE.md`

---

## 🎯 Quick Action Matrix

| I Want To... | Use These Files | Read This Docs |
|--------------|----------------|----------------|
| **Use it now** | ultimate.c + Makefile.ultimate | QUICK_REFERENCE.md |
| **Understand internals** | ultimate.c source code | HOW_IT_WORKS.md |
| **Choose version** | All three .c files | FINAL_SUMMARY.md |
| **Troubleshoot** | ultimate.c | ULTIMATE_README.md (Troubleshooting) |
| **Optimize performance** | ultimate.c | QUICK_REFERENCE.md (Performance) |
| **Per-node stats** | ultimate.c | PER_NODE_STATS.md |
| **Add eBPF** | ebpf/*.py | See ebpf/ directory README |

---

## 📞 Help!

**I'm lost, where do I start?**
→ Read `README.md`, then `QUICK_REFERENCE.md`, then build `Makefile.ultimate`

**I want the best version**
→ Use `pg_trace_ultimate` (this is it!)

**I need minimal overhead**
→ Use `pg_trace_enhanced` (no per-block I/O)

**I just need SQL + plans**
→ Use `pg_trace_mvp` (simplest)

**I'm getting errors**
→ Check `ULTIMATE_README.md` Troubleshooting section

**What's the overhead again?**
→ 2-4% with `track_io_timing`, see `HOW_IT_WORKS.md`

---

**Bottom Line:** Start with `README.md`, then `QUICK_REFERENCE.md`, build `Makefile.ultimate`, create extension, start tracing! 🚀
