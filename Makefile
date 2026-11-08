ARCH := $(shell uname -m)
CC   ?= gcc
WARN := -Wall -Wextra -Werror
TESTS := tests/t1_pingpong tests/t2_churn tests/t3_many tests/t4_guard tests/t5_fp tests/t6_deadlock \
  tests/t7_echo tests/t8_deadline tests/t9_cancel_race tests/t10_failfast tests/t11_nested \
  tests/t12_close_recv tests/t13_sleep tests/t14_blocked tests/t15_deadlock_unwind \
  tests/t16_reuseport_echo tests/t17_identity tests/t18_failfast_cross tests/t19_cancel_all \
  tests/t20_sigterm tests/t21_udp tests/t22_map_cap tests/t23_cancel_exit_race \
  tests/t24_fairness tests/t25_udp_waiters tests/t26_close_wakes tests/t27_backend_env tests/t28_timer_heap tests/t29_fd_reuse

ifeq ($(ARCH),x86_64)
ASM := src/ctx_x86_64.S
else
ASM := src/ctx_aarch64.S
endif

B_REL  := build/$(ARCH)/rel
B_ASAN := build/$(ARCH)/asan
CF_REL  := -std=gnu11 -O2 -g -D_FORTIFY_SOURCE=3 $(WARN) -Iinclude
CF_ASAN := -std=gnu11 -O1 -g $(WARN) -Iinclude -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all test test-asan bench-em clean
all: $(B_REL)/libem.a

# $(1)=build dir $(2)=cflags
define rules
$(1)/%.o: src/%.c src/internal.h include/em.h
	@mkdir -p $(1)
	$(CC) $(2) -c $$< -o $$@
$(1)/ctx.o: $(ASM)
	@mkdir -p $(1)
	$(CC) $(2) -c $$< -o $$@
$(1)/libem.a: $(1)/fiber.o $(1)/scope.o $(1)/io.o $(1)/io_uring.o $(1)/io_epoll.o $(1)/ctx.o
	rm -f $$@ && ar rcs $$@ $$^
$(1)/%: tests/%.c $(1)/libem.a src/internal.h
	$(CC) $(2) -Isrc $$< $(1)/libem.a -luring -lm -lpthread -o $$@
endef
$(eval $(call rules,$(B_REL),$(CF_REL)))
$(eval $(call rules,$(B_ASAN),$(CF_ASAN)))

# every test under each backend in BACKENDS (t14 and t27 also set EM_BACKEND themselves).
# Docker CI (io_uring blocked): make test BACKENDS=auto
BACKENDS ?= uring epoll
run_tests = set -e; for b in $(BACKENDS); do echo "=== EM_BACKEND=$$b"; \
  for t in $(notdir $(TESTS)); do echo "== $$t"; EM_BACKEND=$$b $(1)/$$t; done; done

test: $(addprefix $(B_REL)/,$(notdir $(TESTS)))
	@$(call run_tests,$(B_REL))

test-asan: $(addprefix $(B_ASAN)/,$(notdir $(TESTS)))
	@$(call run_tests,$(B_ASAN))

bench-em: $(B_REL)/em_echo
$(B_REL)/em_echo: bench/em_echo.c $(B_REL)/libem.a
	$(CC) $(CF_REL) $< $(B_REL)/libem.a -luring -lm -lpthread -o $@

clean:
	rm -rf build
