# membench build.
#
# NUMA pinning (-X/-A/-B <node>) is core to this tiered-memory suite, so libnuma
# is required. Install libnuma-dev / numactl if linking -lnuma fails.

CC      ?= gcc
CFLAGS  ?= -O3 -march=native -W -Wall -Wextra -Wuninitialized -Wstrict-aliasing
LDFLAGS ?=
LDLIBS  := -lpthread -lnuma

OBJS := main.o workloads.o
BIN  := membench
# htmm_ctl starts/stops the MEMTIS sampler (ksamplingd) via the htmm syscalls;
# the managed-corun harness needs it since enabling the cgroup alone does not
# start sampling. Standalone, no libnuma/pthread.
HELPER := htmm_ctl

all: $(BIN) $(HELPER)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(HELPER): $(HELPER).c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

%.o: %.c membench.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(BIN) $(HELPER)

.PHONY: all clean
