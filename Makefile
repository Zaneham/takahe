# Takahe -- Open-Source Universal Synthesis


CC = gcc

WARN  = -Wall -Wextra -Werror -pedantic
WARN += -Wshadow -Wconversion -Wdouble-promotion -Wundef
WARN += -Wformat=2 -Wnull-dereference -Wswitch-enum -Wswitch-default
WARN += -Wstrict-prototypes -Wold-style-definition -Wmissing-prototypes
WARN += -Wredundant-decls -Wnested-externs -Wcast-align
WARN += -Wno-unused-parameter
WARN += -Wno-unused-but-set-variable

INCS    = -Iinclude -Isrc \
          -Isrc/lex -Isrc/parse -Isrc/elab \
          -Isrc/rtl -Isrc/opt \
          -Isrc/xform -Isrc/tech -Isrc/map -Isrc/emit
CFLAGS  = $(WARN) -std=c99 -O2 $(INCS)
TFLAGS  = $(WARN) -std=c99 -O0 -g $(INCS)

ifdef DEBUG
CFLAGS = $(WARN) -std=c99 -O0 -g -DDEBUG $(INCS)
TFLAGS = $(WARN) -std=c99 -O0 -g -DDEBUG $(INCS)
endif

# Source files -- one line per stage
SRCS = src/main.c \
       src/tk_abend.c \
       src/tk_data.c \
       src/tk_jrn.c \
       src/lex/tk_lex.c \
       src/lex/vh_lex.c \
       src/lex/tk_dload.c \
       src/lex/tk_pp.c \
       src/lex/ab_lex.c \
       src/parse/tk_parse.c \
       src/parse/vh_parse.c \
       src/parse/ab_parse.c \
       src/elab/tk_ceval.c \
       src/elab/tk_elab.c \
       src/elab/tk_width.c \
       src/elab/tk_gexp.c \
       src/elab/tk_flat.c \
       src/rtl/tk_rtl.c \
       src/rtl/tk_lower.c \
       src/opt/tk_cprop.c \
       src/opt/tk_dce.c \
       src/opt/tk_opt.c \
       src/opt/tk_equiv.c \
       src/opt/tk_cnf.c \
       src/opt/tk_sat.c \
       src/opt/tk_fi.c \
       src/xform/tk_bblst.c \
       src/xform/tk_pmatch.c \
       src/xform/tk_espresso.c \
       src/xform/tk_espro.c \
       src/xform/tk_tmr.c \
       src/xform/tk_seqr.c \
       src/xform/tk_sim.c \
       src/tech/tk_cdef.c \
       src/tech/tk_lib.c \
       src/tech/tk_bind.c \
       src/tech/tk_lfn.c \
       src/tech/tk_pchip.c \
       src/tech/tk_sta.c \
       src/tech/tk_tdopt.c \
       src/map/tk_fpga.c \
       src/map/tk_mmap.c \
       src/emit/tk_blif.c \
       src/emit/tk_yosys.c \
       src/emit/tk_vlog.c \
       src/emit/tk_hash.c

OBJS = $(SRCS:.c=.o)
TARGET = takahe

ifeq ($(OS),Windows_NT)
TARGET := $(TARGET).exe
endif

.PHONY: all clean test install uninstall

all: $(TARGET)

# LDFLAGS is honoured so a release build can ask for -static. Without it the
# binary wants libgcc and libssp at run time, which nobody downloading an
# archive has.
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ -lm

%.o: %.c include/takahe.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Tests
TEST_SRCS = tests/tmain.c tests/tlex.c tests/tparse.c tests/telab.c \
            tests/trtl.c tests/topt.c tests/tmap.c tests/tvhdl.c \
            tests/tabel.c tests/thash.c tests/tspans.c tests/tmmap.c \
            tests/tlfn.c tests/tnlst.c tests/tsat.c tests/tfi.c
TEST_TARGET = trunner

ifeq ($(OS),Windows_NT)
TEST_TARGET := $(TEST_TARGET).exe
endif

test: $(TARGET) $(TEST_TARGET)
	./$(TEST_TARGET) --all

TEST_OBJS = $(filter-out src/main.o,$(OBJS))
$(TEST_TARGET): $(TEST_SRCS) $(TEST_OBJS)
	$(CC) $(TFLAGS) -o $@ $^ -lm

# ---- Install ----
# The binary plus the definition files it cannot start without, and the
# message catalogues. The PDK libraries under lib/ are 78 MB of third-party
# Liberty data and --lib takes an explicit path, so they stay out of this.
#
# Version comes out of the header. awk keyed on the macro name rather than a
# sed capture group, because make 3.81 (which is what macOS ships) treats a #
# inside $(shell) as the start of a comment and swallows the rest of the call.
PREFIX  ?= /usr/local
BINDIR   = $(DESTDIR)$(PREFIX)/bin
SHAREDIR = $(DESTDIR)$(PREFIX)/share/takahe
CMAKEDIR = $(DESTDIR)$(PREFIX)/lib/cmake/Takahe

VER_MAJOR := $(shell awk '$$2 == "TK_VER_MAJOR" {print $$3}' include/takahe.h)
VER_MINOR := $(shell awk '$$2 == "TK_VER_MINOR" {print $$3}' include/takahe.h)
VER_PATCH := $(shell awk '$$2 == "TK_VER_PATCH" {print $$3}' include/takahe.h)
VERSION   := $(VER_MAJOR).$(VER_MINOR).$(VER_PATCH)

install: $(TARGET)
	install -d $(BINDIR) $(SHAREDIR)/defs $(SHAREDIR)/lang $(CMAKEDIR)
	install -m 755 $(TARGET) $(BINDIR)/$(TARGET)
	install -m 644 defs/*.def $(SHAREDIR)/defs/
	install -m 644 lang/en.txt lang/mi.txt $(SHAREDIR)/lang/
	sed -e 's/@TAKAHE_VERSION@/$(VERSION)/g' \
	    -e 's/@TAKAHE_VERSION_MAJOR@/$(VER_MAJOR)/g' \
	    -e 's/@TAKAHE_VERSION_MINOR@/$(VER_MINOR)/g' \
	    cmake/TakaheConfig.cmake.in > $(CMAKEDIR)/TakaheConfig.cmake
	sed -e 's/@TAKAHE_VERSION@/$(VERSION)/g' \
	    -e 's/@TAKAHE_VERSION_MAJOR@/$(VER_MAJOR)/g' \
	    -e 's/@TAKAHE_VERSION_MINOR@/$(VER_MINOR)/g' \
	    cmake/TakaheConfigVersion.cmake.in > $(CMAKEDIR)/TakaheConfigVersion.cmake

uninstall:
	rm -f $(BINDIR)/$(TARGET)
	rm -rf $(SHAREDIR) $(CMAKEDIR)

clean:
	rm -f $(OBJS) $(TARGET) $(TEST_TARGET)
	rm -f tests/*.o
	rm -f src/lex/*.o src/parse/*.o src/elab/*.o
	rm -f src/rtl/*.o src/opt/*.o
	rm -f src/xform/*.o src/tech/*.o src/emit/*.o
	rm -f src/*.o
