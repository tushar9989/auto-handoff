CXX ?= clang++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic

build/auto-handoff: auto-handoff.cpp
	mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $<

test: build/auto-handoff
	python3 tests/test_hook.py
	python3 tests/test_install.py

live: build/auto-handoff
	python3 tests/live.py

install: test
	python3 install.py

uninstall:
	python3 install.py --uninstall

.PHONY: test live install uninstall
