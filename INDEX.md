# pg_trace - Complete File Index

## 📁 Project Structure

```
/Users/dmitryfomin/work/git/pg_trace/
│
├── 📚 Documentation (START HERE!)
│   ├── START_HERE.md              ← ⭐ BEGIN HERE!
│   ├── QUICK_REFERENCE.md          ← One-page cheat sheet
│   ├── ULTIMATE_README.md          ← Complete guide to Ultimate version
│   ├── PER_NODE_STATS.md          ← ✨ NEW! Per-node statistics explained
│   ├── FINAL_SUMMARY.md            ← All three versions compared
│   ├── HOW_IT_WORKS.md            ← Technical deep dive
│   ├── APPROACHES_COMPARISON.md    ← All architectural approaches
│   ├── PROCFS_APPROACH.md         ← /proc filesystem details
│   ├── SMGR_APPROACH.md           ← Storage manager discussion
│   ├── QUICKSTART.md              ← Original MVP quickstart
│   ├── README_MVP.md              ← MVP documentation
│   ├── MVP_SUMMARY.md             ← MVP technical summary
│   ├── FUTURE_ENHANCEMENTS.md     ← Enhancement ideas
│   └── INDEX.md                   ← This file
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
├── 🔧 Source Code - Original (Deprecated)
│   ├── src/pg_trace.c                  ← Original implementation
│   ├── include/pg_trace.h              ← Header file
│   ├── sql/pg_trace--1.0.sql           ← SQL interface
│   ├── pg_trace.control                ← Extension metadata
│   └── Makefile                        ← Build file
│
├── 🐍 eBPF Scripts (Optional, requires root)
│   ├── ebpf/pg_trace_waits.py          ← Wait event tracing
│   └── ebpf/pg_trace_orchestrate.py    ← Combines extension + eBPF
│
├── 📂 Directories
│   ├── include/                        ← Header files
│   ├── src/                            ← Source files
│   ├── sql/                            ← SQL scripts
│   ├── test/                           ← Test files (empty)
│   └── ebpf/                           ← eBPF scripts
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

### Background Research (45 minutes)
**Read in order:**
1. `APPROACHES_COMPARISON.md` - All architectural options
2. `SMGR_APPROACH.md` - Why we can't hook storage manager
3. `PROCFS_APPROACH.md` - How /proc gives us OS stats

---

## 🔧 Source Code Overview

### File Sizes (approximate)
```
pg_trace_ultimate.c      ~1000 lines  ⭐ Most complete
pg_trace_enhanced.c       ~800 lines     Good balance
pg_trace_mvp.c           ~500 lines     Simplest
pg_trace_procfs.c        ~200 lines     /proc reader (shared)
```

### Dependencies Between Files

```
pg_trace_ultimate.c
    ├─→ pg_trace_procfs.c       (CPU/I/O from /proc)
    └─→ PostgreSQL headers      (executor, instrumentation, etc.)

pg_trace_enhanced.c
    ├─→ pg_trace_procfs.c       (CPU/I/O from /proc)
    └─→ PostgreSQL headers

pg_trace_mvp.c
    └─→ PostgreSQL headers      (no /proc dependency)

pg_trace_procfs.c
    └─→ Linux /proc filesystem  (standalone, reusable)
```

---

## 🎯 Which Files Do I Need?

### For Most Users (Recommended):
```bash
✅ src/pg_trace_ultimate.c
✅ src/pg_trace_procfs.c
✅ src/pg_trace_procfs.h
✅ sql/pg_trace_ultimate--1.0.sql
✅ pg_trace_ultimate.control
✅ Makefile.ultimate
📖 QUICK_REFERENCE.md
📖 ULTIMATE_README.md
```

**Build:**
```bash
make -f Makefile.ultimate
```

### For Minimal Setup:
```bash
✅ src/pg_trace_mvp.c
✅ sql/pg_trace_mvp--1.0.sql
✅ pg_trace_mvp.control
✅ Makefile.mvp
📖 README_MVP.md
```

**Build:**
```bash
make -f Makefile.mvp
```

### For eBPF Integration (Advanced):
```bash
✅ All Ultimate files (above)
✅ ebpf/pg_trace_waits.py
✅ ebpf/pg_trace_orchestrate.py
📖 README_MVP.md (has eBPF section)
```

**Requires:** `bcc` tools, root access

---

## 📊 Feature Matrix by File

| Feature | ultimate.c | enhanced.c | mvp.c | Original |
|---------|-----------|-----------|-------|----------|
| SQL text | ✅ | ✅ | ✅ | ✅ |
| Bind variables | ✅ | ✅ | ✅ | ✅ |
| Execution plan | ✅ | ✅ | ✅ | ✅ |
| Buffer stats | ✅ | ✅ | ✅ | ✅ |
| CPU time | ✅ | ✅ | ❌ | ❌ |
| I/O bytes | ✅ | ✅ | ❌ | ❌ |
| **Per-block I/O** | ✅ | ❌ | ❌ | ❌ |
| **OS cache detection** | ✅ | ❌ | ❌ | ❌ |
| File paths | ✅ | ❌ | ❌ | ❌ |
| Lines of code | ~1000 | ~800 | ~500 | ~800 |
| Overhead | 2-4% | ~2% | ~2% | ~2% |
| **Recommended** | ⭐ **YES** | For old HW | For minimal | Deprecated |

---

## 🏗️ Build Commands

### Ultimate (Recommended):
```bash
make -f Makefile.ultimate           # Build
make -f Makefile.ultimate install   # Install
make -f Makefile.ultimate test      # Test
make -f Makefile.ultimate help      # Help
```

### Enhanced:
```bash
make -f Makefile.enhanced
make -f Makefile.enhanced install
```

### Basic MVP:
```bash
make -f Makefile.mvp
make -f Makefile.mvp install
```

### Original (Don't use):
```bash
make                    # Deprecated
```

---

## 📖 Documentation Reading Order

### Path 1: "I want to use this NOW!" (10 minutes)
1. `QUICK_REFERENCE.md` - Setup and usage
2. Build and test
3. Done!

### Path 2: "I want to understand it first" (45 minutes)
1. `FINAL_SUMMARY.md` - What we built and why
2. `ULTIMATE_README.md` - How to use Ultimate version
3. `QUICK_REFERENCE.md` - Quick reference
4. Build and test

### Path 3: "I want to master this" (2 hours)
1. `FINAL_SUMMARY.md` - Overview
2. `HOW_IT_WORKS.md` - Technical deep dive
3. `APPROACHES_COMPARISON.md` - All options explored
4. `ULTIMATE_README.md` - Practical usage
5. Read source code: `src/pg_trace_ultimate.c`
6. Build, test, experiment

### Path 4: "I'm researching alternatives" (1 hour)
1. `APPROACHES_COMPARISON.md` - All architectural options
2. `SMGR_APPROACH.md` - Storage manager discussion
3. `PROCFS_APPROACH.md` - /proc approach
4. `HOW_IT_WORKS.md` - Our final solution
5. `FINAL_SUMMARY.md` - Comparison

---

## 🎯 File Purpose Quick Lookup

| File Name | Purpose | Read If... |
|-----------|---------|-----------|
| `QUICK_REFERENCE.md` | One-page cheat sheet | You want to get started NOW |
| `ULTIMATE_README.md` | Complete guide | You're using Ultimate version |
| `FINAL_SUMMARY.md` | All versions compared | You're choosing which version |
| `HOW_IT_WORKS.md` | Technical deep dive | You want to understand internals |
| `APPROACHES_COMPARISON.md` | Architecture discussion | You're researching approaches |
| `PROCFS_APPROACH.md` | /proc implementation | You want OS stats details |
| `SMGR_APPROACH.md` | Storage manager | You wonder about smgr hooks |
| `README_MVP.md` | MVP documentation | You're using MVP version |
| `QUICKSTART.md` | MVP quick start | You're using MVP version |
| `MVP_SUMMARY.md` | MVP technical details | You're developing MVP |

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

### "Why not use eBPF?"
→ `APPROACHES_COMPARISON.md` (eBPF Approach)
→ `FINAL_SUMMARY.md` (Why This Approach)

### "Why can't I hook smgr?"
→ `SMGR_APPROACH.md` (entire file)

### "How to get CPU stats?"
→ `PROCFS_APPROACH.md` (CPU Statistics)
→ Source: `src/pg_trace_procfs.c`

### "What's different from Oracle?"
→ `QUICK_REFERENCE.md` (vs Oracle 10046)
→ `ULTIMATE_README.md` (Comparing with Oracle)

---

## 🗂️ File Categories

### 📚 User Documentation (Read These)
- `QUICK_REFERENCE.md` ⭐
- `ULTIMATE_README.md` ⭐
- `FINAL_SUMMARY.md`
- `README_MVP.md`
- `QUICKSTART.md`

### 🔬 Technical Documentation (For Developers)
- `HOW_IT_WORKS.md` ⭐
- `APPROACHES_COMPARISON.md`
- `PROCFS_APPROACH.md`
- `SMGR_APPROACH.md`
- `MVP_SUMMARY.md`

### 💻 Source Code (Implementation)
- `src/pg_trace_ultimate.c` ⭐
- `src/pg_trace_enhanced.c`
- `src/pg_trace_mvp.c`
- `src/pg_trace_procfs.c`

### 🔧 Build Configuration
- `Makefile.ultimate` ⭐
- `Makefile.enhanced`
- `Makefile.mvp`
- `*.control` files

### 📜 SQL Interface
- `sql/pg_trace_ultimate--1.0.sql` ⭐
- `sql/pg_trace_enhanced--1.0.sql`
- `sql/pg_trace_mvp--1.0.sql`

### 🐍 Optional Tools
- `ebpf/pg_trace_waits.py`
- `ebpf/pg_trace_orchestrate.py`

---

## 🚀 Most Important Files (Start Here)

### To USE the tool:
1. `QUICK_REFERENCE.md` ⭐⭐⭐
2. `Makefile.ultimate`
3. `src/pg_trace_ultimate.c` (source)

### To UNDERSTAND the tool:
1. `HOW_IT_WORKS.md` ⭐⭐⭐
2. `FINAL_SUMMARY.md`
3. Source code

### To CHOOSE a version:
1. `FINAL_SUMMARY.md` ⭐⭐⭐
2. `QUICK_REFERENCE.md`

---

## 📊 LOC (Lines of Code) Summary

```
Documentation:       ~3,500 lines
Source Code:         ~2,500 lines
SQL Scripts:           ~100 lines
Build Files:           ~150 lines
eBPF (optional):       ~400 lines
─────────────────────────────────
Total:               ~6,650 lines
```

**Ratio:** Documentation:Code = 1.4:1 (very well documented!)

---

## 🎯 Quick Action Matrix

| I Want To... | Use These Files | Read This Docs |
|--------------|----------------|----------------|
| **Use it now** | ultimate.c + Makefile.ultimate | QUICK_REFERENCE.md |
| **Understand internals** | ultimate.c source code | HOW_IT_WORKS.md |
| **Choose version** | All three .c files | FINAL_SUMMARY.md |
| **Troubleshoot** | ultimate.c | ULTIMATE_README.md (Troubleshooting) |
| **Optimize performance** | ultimate.c | QUICK_REFERENCE.md (Performance) |
| **Add eBPF** | ebpf/*.py | README_MVP.md (eBPF section) |
| **Develop extension** | ultimate.c + procfs.c | HOW_IT_WORKS.md + source |
| **Compare approaches** | N/A | APPROACHES_COMPARISON.md |

---

## 📞 Help!

**I'm lost, where do I start?**
→ Read `QUICK_REFERENCE.md`, then build `Makefile.ultimate`

**I want the best version**
→ Use `pg_trace_ultimate` (this is it!)

**I need minimal overhead**
→ Use `pg_trace_enhanced` (no per-block I/O)

**I just need SQL + plans**
→ Use `pg_trace_mvp` (simplest)

**I want true wait events**
→ Use `pg_trace_ultimate` + eBPF scripts (requires root)

**I'm getting errors**
→ Check `ULTIMATE_README.md` Troubleshooting section

**What's the overhead again?**
→ 2-4% with `track_io_timing`, see `HOW_IT_WORKS.md`

---

## ✅ Completeness Checklist

✅ Three implementations (MVP, Enhanced, Ultimate)  
✅ Complete documentation (6+ guides)  
✅ Build system (Makefiles)  
✅ SQL interfaces  
✅ Extension metadata (.control files)  
✅ Optional eBPF scripts  
✅ Quick reference  
✅ Technical deep dive  
✅ Troubleshooting guides  
✅ Performance analysis  
✅ Comparison matrices  
✅ This index!  

**Everything you need is here!** 🎉

---

**Bottom Line:** Start with `QUICK_REFERENCE.md`, build `Makefile.ultimate`, create extension, start tracing! 🚀

