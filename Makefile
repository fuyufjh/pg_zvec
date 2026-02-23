##############################################################################
# pg_zvec Makefile  (Phase 2 – FDW architecture)
#
# Uses PGXS for the overall build.  The C++ bridge (zvec_bridge.cc) is
# compiled with an explicit rule using $(CXX); -lstdc++ is added to
# SHLIB_LINK so the shared library links correctly.
#
# Variables you can override on the command line:
#   PG_CONFIG  – path to pg_config          (default: first on $PATH)
#   USE_ZVEC   – set to 1 to compile with the real zvec library
#   ZVEC_BUILD – path to the zvec cmake build directory
##############################################################################

MODULE_big = pg_zvec

# ---- Object files -----------------------------------------------------------
C_SRCS = \
	src/pg_zvec_fdw.c \
	src/pg_zvec_shmem.c \
	src/pg_zvec_worker.c

CXX_OBJ = src/zvec_bridge/zvec_bridge.o

OBJS = $(C_SRCS:.c=.o) $(CXX_OBJ)

# ---- Extension data ---------------------------------------------------------
EXTENSION = pg_zvec
DATA      = sql/pg_zvec--2.0.sql

# ---- Preprocessor flags for C files -----------------------------------------
PG_CPPFLAGS = -I$(CURDIR)/src

# ---- Linker flags -----------------------------------------------------------
SHLIB_LINK += -lstdc++

ifdef USE_ZVEC
ZVEC_BUILD ?= $(CURDIR)/zvec/build
SHLIB_LINK += \
	$(ZVEC_BUILD)/lib/libzvec.a \
	$(ZVEC_BUILD)/external/usr/local/lib/librocksdb.a \
	$(ZVEC_BUILD)/external/usr/local/lib/libarrow.a \
	$(ZVEC_BUILD)/external/usr/local/lib/libprotobuf.a \
	$(ZVEC_BUILD)/external/usr/local/lib/libantlr4-runtime.a \
	$(ZVEC_BUILD)/external/usr/local/lib/libroaring.a \
	$(ZVEC_BUILD)/external/usr/local/lib/liblz4.a \
	-lgflags -lglog -lpthread -lsnappy -lbz2 -lz -ldl
endif

# ---- Regression tests -------------------------------------------------------
# Run with:
#   PGHOST=/tmp/pg_zvec_socket PGPORT=5499 make installcheck
REGRESS      = 01_extension 02_server 03_table_options 04_ddl 05_scan
REGRESS_OPTS = \
	--inputdir=test \
	--outputdir=test \
	--dbname=pg_zvec_regress

# ---- PGXS -------------------------------------------------------------------
PG_CONFIG ?= pg_config
PGXS      := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# ---- C++ compilation --------------------------------------------------------
PG_INCLUDEDIR_SERVER := $(shell $(PG_CONFIG) --includedir-server)
PG_INCLUDEDIR        := $(shell $(PG_CONFIG) --includedir)

ZVEC_CXXFLAGS = \
	-std=c++17 \
	-fPIC \
	-O2 \
	-Wall \
	-I$(CURDIR)/src \
	-I$(PG_INCLUDEDIR_SERVER) \
	-I$(PG_INCLUDEDIR)

ifdef USE_ZVEC
ZVEC_CXXFLAGS += \
	-DUSE_ZVEC \
	-I$(CURDIR)/zvec/src/include \
	-I$(ZVEC_BUILD)/external/usr/local/include
endif

$(CXX_OBJ): src/zvec_bridge/zvec_bridge.cc src/zvec_bridge/zvec_bridge.h
	$(CXX) $(ZVEC_CXXFLAGS) -c $< -o $@

# JIT bitcode for C++ bridge
CLANGXX ?= clang++-14

src/zvec_bridge/zvec_bridge.bc: src/zvec_bridge/zvec_bridge.cc src/zvec_bridge/zvec_bridge.h
	$(CLANGXX) -std=c++17 -fPIC -O2 \
	    -I$(CURDIR)/src \
	    -I$(PG_INCLUDEDIR_SERVER) \
	    -I$(PG_INCLUDEDIR) \
	    -flto=thin -emit-llvm \
	    -c $< -o $@
