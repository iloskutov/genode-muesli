#
# Host test of the log history
#   make -C repos/muesli/src/server/log_ring/test
#
CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O1 -g -Wall -Wextra -fno-exceptions -fno-rtti

all: test_log_ring
	./test_log_ring

test_log_ring: test_log_ring.cc ../../../../include/log_ring/ring.h
	$(CXX) $(CXXFLAGS) -I../../../../include -o $@ $<

clean:
	rm -f test_log_ring

.PHONY: all clean
