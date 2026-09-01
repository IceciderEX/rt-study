CXX = g++
CXXFLAGS = -O2 -g -std=c++20 -fno-rtti -Wall -Wextra -pthread
ROCKSDB_ROOT = /home/wam/grad/rocksdb-v11.8.0
INCLUDES = -I$(ROCKSDB_ROOT)/include -I.
# Use the prototype branch's current shared library. The legacy CMake static
# archive under build/ is a separate, stale build and does not contain the
# range tombstone controller.
ROCKSDB_LIB = $(ROCKSDB_ROOT)/librocksdb.so
RPATH = -Wl,-rpath,$(ROCKSDB_ROOT)
LIBS = -lssl -lcrypto -lpthread -lzstd -lsnappy -llz4 -lz -ldl

TARGET = /home/wam/grad/s14-range-delete-study/bin/formal_driver
SRCS = formal_driver.cc

all: $(TARGET)

$(TARGET): $(SRCS) formal_config.h json_parser.h manifest_parser.h thread_local_histogram.h formal_event_listener.h worker_state_model.h kv_verifier.h
	mkdir -p /home/wam/grad/s14-range-delete-study/bin
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SRCS) $(ROCKSDB_LIB) $(RPATH) $(LIBS) -o $(TARGET)

clean:
	rm -f $(TARGET)
