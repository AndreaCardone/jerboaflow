CC      ?= gcc
# Hardening: stack protector, FORTIFY, PIE, full RELRO, stack-clash protection,
# extra warnings that matter for embedded/safety code (-Wshadow catches
# shadowed locals; -Wformat-security catches printf(user_str); -Wnull-dereference
# catches obvious NULL deref; -Wdouble-promotion matters on FPU-less MCUs).
CFLAGS  ?= -Wall -Wextra -Werror -O2 -g -std=c11 \
           -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fstack-clash-protection \
           -fPIE \
           -Wshadow -Wformat-security -Wnull-dereference -Wdouble-promotion
LDFLAGS ?= -pthread -pie -Wl,-z,now -Wl,-z,relro

NODE_SRC = $(filter-out nodes/lua.c nodes/mqtt.c nodes/gpio.c,$(wildcard nodes/*.c))

# Feature toggles collect their -D/-I/-l into FEAT_C/FEAT_L so the sanitizer
# rules below can pick them up without re-deriving the WITH_* state.
FEAT_CFLAGS  :=
FEAT_LDFLAGS :=

ifeq ($(WITH_LUA),1)
    LUA_CFLAGS   := $(shell pkg-config --cflags lua5.4)
    LUA_LDFLAGS  := $(shell pkg-config --libs   lua5.4)
    FEAT_CFLAGS  += -DWITH_LUA $(LUA_CFLAGS)
    FEAT_LDFLAGS += $(LUA_LDFLAGS)
    NODE_SRC     += nodes/lua.c
endif

ifeq ($(WITH_MQTT),1)
    MQTT_CFLAGS  := $(shell pkg-config --cflags libmosquitto)
    MQTT_LDFLAGS := $(shell pkg-config --libs   libmosquitto)
    FEAT_CFLAGS  += -DWITH_MQTT $(MQTT_CFLAGS)
    FEAT_LDFLAGS += $(MQTT_LDFLAGS)
    NODE_SRC     += nodes/mqtt.c
endif

ifeq ($(WITH_GPIO),1)
    GPIO_CFLAGS  := $(shell pkg-config --cflags libgpiod 2>/dev/null)
    GPIO_LDFLAGS := $(shell pkg-config --libs   libgpiod 2>/dev/null)
    ifeq ($(strip $(GPIO_LDFLAGS)),)
        GPIO_LDFLAGS := -lgpiod
    endif
    FEAT_CFLAGS  += -DWITH_GPIO $(GPIO_CFLAGS)
    FEAT_LDFLAGS += $(GPIO_LDFLAGS)
    NODE_SRC     += nodes/gpio.c
endif

CFLAGS  += $(FEAT_CFLAGS)
LDFLAGS += $(FEAT_LDFLAGS)

RUNTIME  = jerboa.c metrics.c http_io.c $(NODE_SRC)
SRC      = main.c $(RUNTIME)
HDR      = jerboa.h nodes/nodes.h http_io.h

TESTS = test_packet test_pqueue test_flow test_nodes
TEST_BINS = $(addprefix test/,$(TESTS))

all: jerboa

jerboa: $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LDFLAGS)

# Test binaries link against the runtime (jerboa.c + nodes/*.c), not main.c.
test/%: test/%.c $(RUNTIME) $(HDR) test/test.h
	$(CC) $(CFLAGS) $< $(RUNTIME) -o $@ $(LDFLAGS)

test: $(TEST_BINS)
	@set -e; rc=0; \
	for t in $(TEST_BINS); do \
	    printf "\n== %s ==\n" $$t; \
	    ./$$t || rc=1; \
	done; \
	exit $$rc

# Valgrind the whole test suite — must report zero errors and zero leaks.
test-valgrind: $(TEST_BINS)
	@set -e; rc=0; \
	for t in $(TEST_BINS); do \
	    printf "\n== valgrind %s ==\n" $$t; \
	    valgrind --quiet --leak-check=full --show-leak-kinds=all \
	             --error-exitcode=1 ./$$t || rc=1; \
	done; \
	exit $$rc

# ThreadSanitizer build (no -Werror with TSan, no FORTIFY_SOURCE, no -O2)
jerboa_tsan: $(SRC) $(HDR)
	$(CC) -Wall -Wextra -std=c11 -g -fsanitize=thread $(FEAT_CFLAGS) \
	      $(SRC) -o $@ -pthread $(FEAT_LDFLAGS)
# Run TSan (setarch -R works around the Ubuntu 24.04 ASLR/TSan vma issue)
test-tsan: jerboa_tsan
	setarch $$(uname -m) -R ./jerboa_tsan flow.conf 2

# AddressSanitizer + UBSan build of the test suite.
test/%.asan: test/%.c $(RUNTIME) $(HDR) test/test.h
	$(CC) -Wall -Wextra -std=c11 -g -O1 -fno-omit-frame-pointer \
	      -fsanitize=address,undefined $(FEAT_CFLAGS) \
	      $< $(RUNTIME) -o $@ -pthread $(FEAT_LDFLAGS)
test-asan: $(addsuffix .asan,$(TEST_BINS))
	@set -e; rc=0; \
	for t in $(addsuffix .asan,$(TEST_BINS)); do \
	    printf "\n== asan %s ==\n" $$t; \
	    ./$$t || rc=1; \
	done; \
	exit $$rc

clean:
	rm -f jerboa jerboa_tsan $(TEST_BINS) $(addsuffix .asan,$(TEST_BINS))

.PHONY: all clean test test-valgrind test-tsan test-asan
