# SPDX-License-Identifier: GPL-2.0-or-later
# rsh: Ruby with command lines (docs/rsh.md). The interpreter is fetched at one pinned
# commit and checked by sha256 (never kept in the tree); MRUBY_TARBALL= hands in a local copy
# (the package build is offline). Needs: a C/C++ compiler, ruby and bison (the interpreter's build
# script), libpcre2-8 (Regexp).
MRUBY_COMMIT = 831da26b9021de0369d17b71b5667e2941a1a32d
MRUBY_SHA256 = e4fb1bbad8276878e84e3c3b6161ac35a4216853de404f8463675aa2d1920b4f
MRUBY_URL    = https://github.com/mruby/mruby/archive/$(MRUBY_COMMIT).tar.gz
MRUBY_TARBALL ?= build/mruby-$(MRUBY_COMMIT).tar.gz

PREFIX ?= /usr/local
DESTDIR ?=
CXX ?= c++
CC ?= cc
# LINK=static: one file with no shared libraries (needs libpcre2-8.a); LINK=dynamic (the package):
# glibc and libpcre2-8 shared, the C++ runtime linked in.
LINK ?= static
VERSION := $(shell cat VERSION)

M      = build/mruby
MLIB   = $(M)/build/rsh/lib/libmruby.a
OPT ?= -Os
CXXFLAGS += $(OPT) -std=c++20 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -fno-threadsafe-statics -ffunction-sections \
	-fdata-sections -I$(M)/include -I$(M)/build/rsh/include -I$(M)/mrbgems/mruby-io/include -DMRB_UTF8_STRING -DMRB_STR_LENGTH_MAX=0 -DMRB_ARY_LENGTH_MAX=0 -DRSH_VERSION='"$(VERSION)"'
PCRE   = $(shell pkg-config --cflags libpcre2-8 2>/dev/null) -DPCRE2_CODE_UNIT_WIDTH=8
ifeq ($(LINK),static)
LDFLAGS += -static
endif
STRIP ?= -s
LDFLAGS += -static-libstdc++ -static-libgcc -Wl,--gc-sections $(STRIP)
LIBS    = -lpcre2-8 -lm
# Where a build goes: build/ for the program, build/asan/ for the sanitizer build (make asan).
OUT ?= build

SRC  = $(wildcard src/*.cpp)
MRBC = $(M)/build/rsh/bin/mrbc
OBJ  = $(SRC:src/%.cpp=$(OUT)/obj/%.o)
LIBOBJ = $(filter-out $(OUT)/obj/main.o,$(OBJ))

all: $(OUT)/rsh

$(MRUBY_TARBALL):
	mkdir -p build
	curl -fsSL -o $@.part $(MRUBY_URL)
	mv $@.part $@

build/mruby/.ok: $(MRUBY_TARBALL) build_config.rb
	echo "$(MRUBY_SHA256)  $(MRUBY_TARBALL)" | sha256sum -c -
	rm -rf $(M) && mkdir -p $(M)
	tar xzf $(MRUBY_TARBALL) -C $(M) --strip-components=1
	touch $@

$(MLIB): build/mruby/.ok
	cd $(M) && MRUBY_CONFIG=$(CURDIR)/build_config.rb ruby ./minirake -j8 all >/dev/null

$(OUT)/obj/%.o: src/%.cpp $(wildcard src/*.hpp) $(MLIB) VERSION
	@mkdir -p $(OUT)/obj
	$(CXX) $(CXXFLAGS) $(PCRE) -c $< -o $@

# The Ruby half of rsh, compiled to bytecode and linked in (no parsing at start-up).
build/prelude.c: src/prelude.rb $(MLIB)
	$(MRBC) -Brsh_prelude -o $@ src/prelude.rb

# The Ruby side of Regexp: compiled in the same way, loaded on first use (src/rt.cpp, m_regexp).
build/prelude_regexp.c: src/prelude_regexp.rb $(MLIB)
	$(MRBC) -Brsh_prelude_regexp -o $@ src/prelude_regexp.rb

# Signal, trap, at_exit, Dir.home, Process.kill: loaded on first use (src/main.cpp, Rsh.__more).
build/prelude_more.c: src/prelude_more.rb $(MLIB)
	$(MRBC) -Brsh_prelude_more -o $@ src/prelude_more.rb

# Sh, FileUtils, Find, Shellwords, Dir.mktmpdir (0.9.0): loaded on first use (src/main.cpp, Rsh.__std).
build/prelude_std.c: src/prelude_std.rb $(MLIB)
	$(MRBC) -Brsh_prelude_std -o $@ src/prelude_std.rb

$(OUT)/obj/prelude.o: build/prelude.c
	@mkdir -p $(OUT)/obj
	$(CC) -Os -I$(M)/include -I$(M)/build/rsh/include -c $< -o $@

$(OUT)/obj/prelude_regexp.o: build/prelude_regexp.c
	@mkdir -p $(OUT)/obj
	$(CC) -Os -I$(M)/include -I$(M)/build/rsh/include -c $< -o $@

$(OUT)/obj/prelude_more.o: build/prelude_more.c
	@mkdir -p $(OUT)/obj
	$(CC) -Os -I$(M)/include -I$(M)/build/rsh/include -c $< -o $@

$(OUT)/obj/prelude_std.o: build/prelude_std.c
	@mkdir -p $(OUT)/obj
	$(CC) -Os -I$(M)/include -I$(M)/build/rsh/include -c $< -o $@

PRELUDE = $(OUT)/obj/prelude.o $(OUT)/obj/prelude_regexp.o $(OUT)/obj/prelude_more.o $(OUT)/obj/prelude_std.o

$(OUT)/rsh: $(OBJ) $(PRELUDE) $(MLIB)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ) $(PRELUDE) $(MLIB) $(LDFLAGS) $(LIBS)

$(OUT)/unit: tests/unit.cpp $(LIBOBJ) $(PRELUDE) $(MLIB)
	$(CXX) $(CXXFLAGS) $(PCRE) -Isrc -o $@ tests/unit.cpp $(LIBOBJ) $(PRELUDE) $(MLIB) $(LDFLAGS) $(LIBS)

$(OUT)/unit_regexp: tests/unit_regexp.cpp $(LIBOBJ) $(PRELUDE) $(MLIB)
	$(CXX) $(CXXFLAGS) $(PCRE) -Isrc -o $@ tests/unit_regexp.cpp $(LIBOBJ) $(PRELUDE) $(MLIB) $(LDFLAGS) $(LIBS)

# The release gate (docs/STABILITY.md): the program's tests, the frozen contract, the speed and
# memory limits (bench/run.sh --gate), then every test again on the sanitizer build (address +
# undefined behaviour, leaks checked; any report fails).
# SANITIZE=0 leaves the second half out (a build host without the sanitizer libraries); a release
# is never tagged from such a run.
SANITIZE ?= 1
test: $(OUT)/unit $(OUT)/unit_regexp $(OUT)/rsh
	./$(OUT)/unit
	./$(OUT)/unit_regexp
	RSH=$(CURDIR)/$(OUT)/rsh sh tests/run.sh
	sh tests/contract.sh $(OUT)/rsh
	sh tests/doctrine.sh $(OUT)/rsh
	@if command -v ruby >/dev/null; then ruby tests/interactive.rb $(OUT)/rsh; else echo 'interactive: ruby missing, skipped'; fi
	@if command -v ruby >/dev/null; then ruby tests/signals.rb $(OUT)/rsh; else echo 'signals: ruby missing, skipped'; fi
	sh tests/race.sh $(OUT)/rsh
	sh bench/run.sh --gate $(OUT)/rsh
ifeq ($(SANITIZE),1)
	$(MAKE) --no-print-directory asan
	$(ASAN_ENV) ./build/asan/unit
	$(ASAN_ENV) ./build/asan/unit_regexp
	$(ASAN_ENV) RSH=$(CURDIR)/build/asan/rsh sh tests/run.sh
	$(ASAN_ENV) sh tests/contract.sh build/asan/rsh
	@echo 'sanitizer run: ok'
else
	@echo 'sanitizer run: skipped (SANITIZE=0); not a release gate'
endif

# The sanitizer build: shared libraries (the sanitizers need them), symbols kept, -O1.
SAN = -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
ASAN_ENV = ASAN_OPTIONS=detect_leaks=1:abort_on_error=0:exitcode=99 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1:exitcode=98
asan: $(MLIB)
	$(MAKE) --no-print-directory OUT=build/asan LINK=dynamic STRIP= OPT='-O1 -g $(SAN)' build/asan/rsh build/asan/unit build/asan/unit_regexp

# Start-up (tests/bench.sh), then the speed and memory table (bench/run.sh; make test runs its gate).
bench: build/rsh
	sh tests/bench.sh build/rsh
	sh bench/run.sh build/rsh

# The reader fuzz (docs/STABILITY.md, 0.9.0, 1.0): tests/fuzz/reader.cpp under libFuzzer with the address
# and undefined-behaviour sanitizers (needs clang). The seeds are every test script; findings go to
# build/fuzz/crash-*. FUZZ_SECONDS=1800 is the release-gate run.
FUZZ_SECONDS ?= 60
FUZZ_CXX ?= clang++
build/fuzz/reader: tests/fuzz/reader.cpp src/transpile.cpp src/transpile.hpp src/ast.hpp $(MLIB)
	@mkdir -p build/fuzz
	$(FUZZ_CXX) -g -O1 -std=c++20 -fno-exceptions -fno-rtti -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all \
		-I$(M)/include -I$(M)/build/rsh/include -DMRB_UTF8_STRING -DMRB_STR_LENGTH_MAX=0 -DMRB_ARY_LENGTH_MAX=0 -Isrc \
		-o $@ tests/fuzz/reader.cpp src/transpile.cpp $(MLIB) -lm
fuzz: build/fuzz/reader
	@mkdir -p build/fuzz/corpus
	cp tests/golden/*.rsh tests/golden/*.rb tests/examples/*.rsh examples/meta/*.rsh build/fuzz/corpus/
	cd build/fuzz && ./reader -max_total_time=$(FUZZ_SECONDS) -timeout=5 -rss_limit_mb=2048 -max_len=16384 \
		-close_fd_mask=2 -print_final_stats=1 corpus

# make install [PREFIX=/usr] [DESTDIR=staging]
install: build/rsh
	install -Dm755 build/rsh $(DESTDIR)$(PREFIX)/bin/rsh
	install -Dm644 rsh.1 $(DESTDIR)$(PREFIX)/share/man/man1/rsh.1

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/rsh $(DESTDIR)$(PREFIX)/share/man/man1/rsh.1

clean:
	rm -rf build/obj build/rsh build/unit build/unit_regexp build/asan

distclean:
	rm -rf build

.PHONY: all test asan fuzz bench install uninstall clean distclean
