# membench build.
#
# NUMA pinning (-X <node>, e.g. slow-tier runs) needs libnuma, and it is core to
# this suite, so it is ON BY DEFAULT. Opt out only on a box without libnuma-dev:
#
#   make           # default: NUMA enabled (needs libnuma-dev / numactl)
#   make NUMA=0    # portable: drop libnuma, -X <node> becomes a hard error

CC      ?= gcc
CFLAGS  ?= -O3 -march=native -W -Wall -Wextra -Wuninitialized -Wstrict-aliasing
LDFLAGS ?=
LDLIBS  := -lpthread

NUMA    ?= 1
ifneq ($(NUMA),0)
CFLAGS  += -DMEMBENCH_NUMA
LDLIBS  += -lnuma
endif

OBJS := main.o workloads.o
BIN  := membench

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c membench.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(BIN)

.PHONY: clean
