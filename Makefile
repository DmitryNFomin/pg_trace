# pg_trace/Makefile

MODULE_big = pg_trace
OBJS = src/pg_trace.o

EXTENSION = pg_trace
DATA = sql/pg_trace--1.0.sql

# Header files
HEADERS = include/pg_trace.h

# PostgreSQL build system integration
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# Additional compiler flags for warnings and optimization
PG_CPPFLAGS += -I$(srcdir)/include

# Installation
installdirs: installdirs-lib
	$(MKDIR_P) '$(DESTDIR)$(datadir)/extension'
	$(MKDIR_P) '$(DESTDIR)$(includedir_server)/extension/pg_trace'

install: install-lib install-data
	$(INSTALL_DATA) include/pg_trace.h '$(DESTDIR)$(includedir_server)/extension/pg_trace/'

.PHONY: installdirs install

