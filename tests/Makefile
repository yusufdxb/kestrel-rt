# Host tests for portable Kestrel components. ASan+UBSan for everything, TSan for the lock-free
# SPSC queue under real concurrency. `make -C tests` from the repo root.
CC ?= cc
ROOT := ..
CFLAGS := -std=c17 -O1 -g -Wall -Wextra -Werror -Wconversion -Wshadow -I$(ROOT) -DKESTREL_HOST_TEST
SAN := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
BUILD := build

PORTABLE := $(ROOT)/trace/frame.c $(ROOT)/trace/crc16.c $(ROOT)/safety/crashlog/crc32.c $(ROOT)/link/xlink.c
TESTS := test_spsc test_mailbox test_frame test_crc test_timebase test_khist test_xlink

all: run

$(BUILD)/%: %.c $(PORTABLE) test_util.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SAN) $< $(PORTABLE) -o $@ -lpthread

$(BUILD)/test_spsc_tsan: test_spsc_threads.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=thread $< -o $@ -lpthread

run: $(addprefix $(BUILD)/,$(TESTS)) $(BUILD)/test_spsc_tsan
	@set -e; for t in $(TESTS); do echo "== $$t"; ./$(BUILD)/$$t; done
	@echo "== test_spsc_tsan (ASLR disabled for this process: TSan vs high mmap entropy)"
	@setarch $$(uname -m) -R ./$(BUILD)/test_spsc_tsan
	@echo "ALL HOST TESTS PASSED"

clean:
	rm -rf $(BUILD)
