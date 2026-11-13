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
  tests/t36_resolve tests/t37_resolve_abandon tests/t38_file_read tests/t39_yield_poll tests/t57_deadline_timer
# em_http (libem_http.a, tests t50..): no OpenSSL needed in M1.
HTTP_TESTS := tests/t50_http_keepalive tests/t51_http_pipeline tests/t52_http_reject \
  tests/t53_http_workers tests/t54_http_body tests/t55_http_expect tests/t56_http_timeout
TESTS += $(HTTP_TESTS)

# em_tls (libem_tls.a, tests t40..t46) needs OpenSSL. Without it libem and its tests still build;
# `make test` says the TLS tests were skipped. Simulate that: make PKG_CONFIG=false
# TLS_PREFIX=dir builds against the library installed in dir (include/, lib/ or lib64/) instead of
# pkg-config's: OpenSSL self-built, or AWS-LC (see include/em_tls.h). Objects go to a build dir named
# after the prefix, so they never mix with the default build or with another prefix.
PKG_CONFIG ?= pkg-config
ifneq ($(TLS_PREFIX),)
TLS_FOUND := yes
TLS_LIBDIR := $(firstword $(wildcard $(TLS_PREFIX)/lib64/libssl.* $(TLS_PREFIX)/lib/libssl.*))
TLS_LIBDIR := $(patsubst %/,%,$(dir $(TLS_LIBDIR)))
ifeq ($(TLS_LIBDIR),)
$(error TLS_PREFIX=$(TLS_PREFIX): no lib64/libssl.* or lib/libssl.*)
endif
TLS_CF    := -I$(TLS_PREFIX)/include
TLS_LIBS  := -L$(TLS_LIBDIR) -Wl,-rpath,$(TLS_LIBDIR) -lssl -lcrypto
# basename for people, cksum of the full path so two prefixes named alike cannot share a dir
TLS_TAG   := -$(notdir $(patsubst %/,%,$(TLS_PREFIX)))-$(word 1,$(shell echo '$(TLS_PREFIX)' | cksum))
else
TLS_FOUND := $(shell $(PKG_CONFIG) --exists openssl && echo yes)
ifeq ($(TLS_FOUND),yes)
TLS_CF    := $(shell $(PKG_CONFIG) --cflags openssl)
TLS_LIBS  := $(shell $(PKG_CONFIG) --libs openssl)
endif
endif
ifeq ($(TLS_FOUND),yes)
TLS_TESTS := tests/t40_tls_echo tests/t41_tls_duplex tests/t42_tls_verify tests/t43_tls_truncate \
  tests/t44_tls_cancel tests/t45_tls_workers tests/t46_tls_recv_noblock
TESTS     += $(TLS_TESTS)
endif

# picohttpparser picks SSE4.2 at compile time. Default is portable; HTTP_SSE42=1 adds -msse4.2 to
# http.c and picohttpparser.c on x86_64 (ignored elsewhere) and builds into its own dir.
HTTP_CF := -Ithird_party/picohttpparser
ifeq ($(HTTP_SSE42)$(ARCH),1x86_64)
HTTP_CF  += -msse4.2
HTTP_TAG := -sse42
endif

ifeq ($(ARCH),x86_64)
ASM := src/ctx_x86_64.S
else
ASM := src/ctx_aarch64.S
endif

B_REL  := build/$(ARCH)/rel$(TLS_TAG)$(HTTP_TAG)
B_ASAN := build/$(ARCH)/asan$(TLS_TAG)$(HTTP_TAG)
CF_REL  := -std=gnu11 -O2 -g -D_FORTIFY_SOURCE=3 $(WARN) -Iinclude
CF_ASAN := -std=gnu11 -O1 -g $(WARN) -Iinclude -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all test test-asan test-rust bench-em bench-em-tls bench-em-http clean
all: $(B_REL)/libem.a $(B_REL)/libem_http.a $(if $(TLS_TESTS),$(B_REL)/libem_tls.a)

# $(1)=build dir $(2)=cflags
define rules
$(1)/%.o: src/%.c src/internal.h include/em.h
	@mkdir -p $(1)
	$(CC) $(2) -c $$< -o $$@
$(1)/ctx.o: $(ASM)
	@mkdir -p $(1)
	$(CC) $(2) -c $$< -o $$@
$(1)/libem.a: $(1)/fiber.o $(1)/scope.o $(1)/io.o $(1)/io_uring.o $(1)/io_epoll.o $(1)/timer.o $(1)/pool.o $(1)/ctx.o
	rm -f $$@ && ar rcs $$@ $$^
$(1)/%: tests/%.c $(1)/libem.a src/internal.h
	$(CC) $(2) -Isrc $$< $(1)/libem.a -luring -lm -lpthread -o $$@
$(1)/picohttpparser.o: third_party/picohttpparser/picohttpparser.c third_party/picohttpparser/picohttpparser.h
	@mkdir -p $(1)
	$(CC) $(2) $(HTTP_CF) -c $$< -o $$@
$(1)/http.o: src/http.c include/em_http.h include/em.h third_party/picohttpparser/picohttpparser.h
	@mkdir -p $(1)
	$(CC) $(2) $(HTTP_CF) -c $$< -o $$@
$(1)/libem_http.a: $(1)/http.o $(1)/picohttpparser.o
	rm -f $$@ && ar rcs $$@ $$^
$(addprefix $(1)/,$(notdir $(HTTP_TESTS))): $(1)/%: tests/%.c tests/http_util.h include/em_http.h $(1)/libem_http.a $(1)/libem.a
	$(CC) $(2) $$< $(1)/libem_http.a $(1)/libem.a -luring -lm -lpthread -o $$@
ifdef TLS_TESTS
$(1)/tls.o: src/tls.c src/internal.h include/em_tls.h include/em.h
	@mkdir -p $(1)
	$(CC) $(2) $(TLS_CF) -Isrc -c $$< -o $$@
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

bench-em-http: $(B_REL)/em_http
$(B_REL)/em_http: bench/em_http.c $(B_REL)/libem_http.a $(B_REL)/libem.a
	$(CC) $(CF_REL) $< $(B_REL)/libem_http.a $(B_REL)/libem.a -luring -lm -lpthread -o $@

ifdef TLS_TESTS
bench-em-tls: $(B_REL)/em_tls_echo
$(B_REL)/em_tls_echo: bench/em_tls_echo.c tests/tls_util.h $(B_REL)/libem_tls.a $(B_REL)/libem.a
	$(CC) $(CF_REL) $(TLS_CF) -Isrc $< $(B_REL)/libem_tls.a $(B_REL)/libem.a $(TLS_LIBS) -luring -lm -lpthread -o $@
endif

# libFuzzer + ASan + UBSan on em_http_serve (tests/fuzz/http_fuzz.c), needs clang. Everything is
# compiled in one go into build/$(ARCH)/fuzz/ (own dir: sanitizer objects never mix with the
# others). Runs FUZZ_SECONDS on the seed corpus; new inputs go to a work dir, not into the repo.
# A crash or sanitizer report makes libFuzzer exit nonzero, so the target fails.
FUZZ_SECONDS ?= 60
B_FUZZ := build/$(ARCH)/fuzz
FUZZ_SRC := src/fiber.c src/scope.c src/io.c src/io_uring.c src/io_epoll.c src/timer.c src/pool.c $(ASM) \
  src/http.c third_party/picohttpparser/picohttpparser.c
.PHONY: fuzz-http
$(B_FUZZ)/http_fuzz: tests/fuzz/http_fuzz.c $(FUZZ_SRC) $(wildcard src/*.h) include/em.h include/em_http.h
	@mkdir -p $(B_FUZZ)
	clang -std=gnu11 -O1 -g $(WARN) -Iinclude $(HTTP_CF) -fno-omit-frame-pointer \
	  -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
	  tests/fuzz/http_fuzz.c $(FUZZ_SRC) -luring -lm -lpthread -o $@
fuzz-http: $(B_FUZZ)/http_fuzz
	@rm -rf $(B_FUZZ)/http_work && mkdir -p $(B_FUZZ)/http_work
	$(B_FUZZ)/http_fuzz -max_total_time=$(FUZZ_SECONDS) -max_len=4096 -print_final_stats=1 \
	  -artifact_prefix=$(B_FUZZ)/ $(B_FUZZ)/http_work tests/fuzz/corpus/http

clean:
	rm -rf build
