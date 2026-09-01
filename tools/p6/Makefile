CXX = g++
CXXFLAGS = -O2 -g -std=c++20 -Wall -Wextra -pthread
ROCKSDB_DIR = /home/wam/grad/rocksdb-v11.8.0

INCLUDES = -I$(ROCKSDB_DIR)/include -I.
LIBS = $(ROCKSDB_DIR)/build/librocksdb.a -lssl -lcrypto -lpthread -lzstd -lsnappy -llz4 -lz -ldl

TARGET = ../../bin/p6_driver
SRCS = p6_driver.cc

all: $(TARGET)

$(TARGET): $(SRCS) p6_config.h p6_reference_model.h p6_stats_collector.h
	mkdir -p ../../bin
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SRCS) $(LIBS) -o $(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all clean
