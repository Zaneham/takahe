# Takahe -- Open-Source Universal Synthesis


CC = gcc

WARN  = -Wall -Wextra -Werror -pedantic
WARN += -Wshadow -Wconversion -Wdouble-promotion -Wundef
WARN += -Wformat=2 -Wnull-dereference -Wswitch-enum -Wswitch-default
WARN += -Wstrict-prototypes -Wold-style-definition -Wmissing-prototypes
WARN += -Wredundant-decls -Wnested-externs -Wcast-align
WARN += -Wno-unused-parameter

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
       src/tk_jrn.c \
       src/lex/tk_lex.c \
       src/lex/vh_lex.c \
       src/lex/tk_dload.c \
       src/lex/tk_pp.c \
       src/parse/tk_parse.c \
       src/parse/vh_parse.c \
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
       src/xform/tk_bblst.c \
       src/xform/tk_pmatch.c \
       src/xform/tk_espresso.c \
       src/xform/tk_espro.c \
       src/xform/tk_tmr.c \
       src/tech/tk_cdef.c \
       src/tech/tk_lib.c \
       src/tech/tk_bind.c \
       src/tech/tk_pchip.c \
       src/tech/tk_sta.c \
       src/tech/tk_tdopt.c \
       src/map/tk_fpga.c \
       src/emit/tk_blif.c \
       src/emit/tk_yosys.c \
       src/emit/tk_vlog.c

OBJS = $(SRCS:.c=.o)
TARGET = takahe

ifeq ($(OS),Windows_NT)
TARGET := $(TARGET).exe
endif

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ -lm

%.o: %.c include/takahe.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Tests
TEST_SRCS = tests/tmain.c tests/tlex.c tests/tparse.c tests/telab.c \
            tests/trtl.c tests/topt.c tests/tmap.c tests/tvhdl.c
TEST_TARGET = trunner

ifeq ($(OS),Windows_NT)
TEST_TARGET := $(TEST_TARGET).exe
endif

test: $(TARGET) $(TEST_TARGET)
	./$(TEST_TARGET) --all

TEST_OBJS = $(filter-out src/main.o,$(OBJS))
$(TEST_TARGET): $(TEST_SRCS) $(TEST_OBJS)
	$(CC) $(TFLAGS) -o $@ $^ -lm

clean:
	rm -f $(OBJS) $(TARGET) $(TEST_TARGET)
	rm -f tests/*.o
	rm -f src/lex/*.o src/parse/*.o src/elab/*.o
	rm -f src/rtl/*.o src/opt/*.o
	rm -f src/xform/*.o src/tech/*.o src/emit/*.o
	rm -f src/*.o
