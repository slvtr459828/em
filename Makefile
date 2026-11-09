ARCH := $(shell uname -m)
CC   ?= gcc
WARN := -Wall -Wextra -Werror
TESTS := tests/t1_pingpong tests/t2_churn tests/t3_many tests/t4_guard tests/t5_fp tests/t6_deadlock \
  tests/t7_echo tests/t8_deadline tests/t9_cancel_race tests/t10_failfast tests/t11_nested \
  tests/t12_close_recv tests/t13_sleep tests/t14_blocked tests/t15_deadlock_unwind \
  tests/t16_reuseport_echo tests/t17_identity tests/t18_failfast_cross tests/t19_cancel_all \
  tests/t20_sigterm tests/t21_udp tests/t22_map_cap tests/t23_cancel_exit_race \
  tests/t24_fairness tests/t25_udp_waiters tests/t26_close_wakes tests/t27_backend_env tests/t28_timer_heap tests/t29_fd_reuse tests/t30_outside tests/t31_short_read \
  tests/t32_offload_nostall tests/t33_offload_thread tests/t34_offload_nocancel tests/t35_offload_cap \
  tests/t36_resolve tests/t37_resolve_abandon tests/t38_file_read tests/t39_yield_poll

# em_tls (libem_tls.a, tests t40..t46) needs OpenSSL. Without it libem and its tests still build;
# `make test` says the TLS tests were skipped. Simulate that: make PKG_CONFIG=false
PKG_CONFIG ?= pkg-config
ifeq ($(shell $(PKG_CONFIG) --exists openssl && echo yes),yes)
TLS_TESTS := tests/t40_tls_echo tests/t41_tls_duplex tests/t42_tls_verify tests/t43_tls_truncate \
  tests/t44_tls_cancel tests/t45_tls_workers tests/t46_tls_flushwait
TLS_CF    := $(shell $(PKG_CONFIG) --cflags openssl)
TLS_LIBS  := $(shell $(PKG_CONFIG) --libs openssl)
TESTS     += $(TLS_TESTS)
endif

ifeq ($(ARCH),x86_64)
ASM := src/ctx_x86_64.S
else
ASM := src/ctx_aarch64.S
endif

B_REL  := build/$(ARCH)/rel
B_ASAN := build/$(ARCH)/asan
CF_REL  := -std=gnu11 -O2 -g -D_FORTIFY_SOURCE=3 $(WARN) -Iinclude
CF_ASAN := -std=gnu11 -O1 -g $(WARN) -Iinclude -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all test test-asan test-rust bench-em bench-em-tls clean
all: $(B_REL)/libem.a $(if $(TLS_TESTS),$(B_REL)/libem_tls.a)

# $(1)=build dir $(2)=cflags
define rules
$(1)/%.o: src/%.c src/internal.h include/em.h
	@mkdir -p $(1)
	$(CC) $(2) -c $$< -o $$@
$(1)/ctx.o: $(ASM)
	@mkdir -p $(1)
	$(CC) $(2) -c $$< -o $$@
$(1)/libem.a: $(1)/fiber.o $(1)/scope.o $(1)/io.o $(1)/io_uring.o $(1)/io_epoll.o $(1)/pool.o $(1)/ctx.o
	rm -f $$@ && ar rcs $$@ $$^
$(1)/%: tests/%.c $(1)/libem.a src/internal.h
	$(CC) $(2) -Isrc $$< $(1)/libem.a -luring -lm -lpthread -o $$@
ifdef TLS_TESTS
$(1)/tls.o: src/tls.c include/em_tls.h include/em.h
	@mkdir -p $(1)
	$(CC) $(2) $(TLS_CF) -c $$< -o $$@
$(1)/libem_tls.a: $(1)/tls.o
	rm -f $$@ && ar rcs $$@ $$^
$(addprefix $(1)/,$(notdir $(TLS_TESTS))): $(1)/%: tests/%.c tests/tls_util.h $(1)/libem_tls.a $(1)/libem.a
	$(CC) $(2) $(TLS_CF) -Isrc $$< $(1)/libem_tls.a $(1)/libem.a $(TLS_LIBS) -luring -lm -lpthread -o $$@
endif
endef
$(eval $(call rules,$(B_REL),$(CF_REL)))
$(eval $(call rules,$(B_ASAN),$(CF_ASAN)))

# every test under each backend in BACKENDS (t14 and t27 also set EM_BACKEND themselves).
# Docker CI (io_uring blocked): make test BACKENDS=auto
BACKENDS ?= uring epoll
run_tests = set -e; for b in $(BACKENDS); do echo "=== EM_BACKEND=$$b"; \
  for t in $(notdir $(TESTS)); do echo "== $$t"; EM_BACKEND=$$b $(1)/$$t; done; done

no_tls_msg = $(if $(TLS_TESTS),,@echo "SKIPPED: TLS tests t40..t46 and libem_tls (openssl not found by $(PKG_CONFIG))")

test: $(addprefix $(B_REL)/,$(notdir $(TESTS)))
	$(no_tls_msg)
	@$(call run_tests,$(B_REL))
	$(no_tls_msg)

test-asan: $(addprefix $(B_ASAN)/,$(notdir $(TESTS)))
	$(no_tls_msg)
	@$(call run_tests,$(B_ASAN))
	$(no_tls_msg)

# Rust crate: one em_run per test, so tests run one at a time. The panic test runs once more
# with RUST_BACKTRACE=1 (the unwinder walks up a fiber stack to em_tramp).
test-rust:
	@set -e; cd rust/em; for b in $(BACKENDS); do echo "=== EM_BACKEND=$$b"; \
	  EM_BACKEND=$$b cargo test --locked -- --test-threads=1; \
	  EM_BACKEND=$$b RUST_BACKTRACE=1 cargo test --locked --test panic -- --test-threads=1; done

bench-em: $(B_REL)/em_echo
$(B_REL)/em_echo: bench/em_echo.c $(B_REL)/libem.a
	$(CC) $(CF_REL) $< $(B_REL)/libem.a -luring -lm -lpthread -o $@

ifdef TLS_TESTS
bench-em-tls: $(B_REL)/em_tls_echo
$(B_REL)/em_tls_echo: bench/em_tls_echo.c tests/tls_util.h $(B_REL)/libem_tls.a $(B_REL)/libem.a
	$(CC) $(CF_REL) $(TLS_CF) -Isrc $< $(B_REL)/libem_tls.a $(B_REL)/libem.a $(TLS_LIBS) -luring -lm -lpthread -o $@
endif

clean:
	rm -rf build
