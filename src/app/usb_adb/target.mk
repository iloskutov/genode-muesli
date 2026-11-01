TARGET = usb_adb
SRC_CC = main.cc
INC_DIR += $(call select_from_repositories,src/lib/adb_engine)
LIBS   = base vfs sandbox
