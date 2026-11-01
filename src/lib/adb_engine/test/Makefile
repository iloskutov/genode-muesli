#
# Host tests of the adb protocol and of the log follow rule
#   make -C repos/muesli/src/lib/adb_engine/test
#
CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O1 -g -Wall -Wextra -fno-exceptions -fno-rtti

INC   := -I$(abspath ../../../../include) -I$(abspath ../../../../../pixel/include)
HDRS  := $(wildcard ../../../../include/usb_gadget/*.h ../../../../include/adb/*.h \
                    ../../../../include/mueslios/*.h)
TESTS := test_adb test_adb_sync test_adb_framer test_adb_service test_adb_shell_v2 \
         test_log_follow

all: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

test_%: test_%.cc $(HDRS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $<

clean:
	rm -f $(TESTS)

.PHONY: all clean
