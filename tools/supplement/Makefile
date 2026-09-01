CXX = g++
CXXFLAGS = -O2 -g -std=c++20 -Wall -Wextra -pthread
ROCKSDB_DIR = /home/wam/grad/rocksdb-v11.8.0

INCLUDES = -I$(ROCKSDB_DIR)/include -I.
LIBS = $(ROCKSDB_DIR)/build/librocksdb.a -lpthread -lzstd -lsnappy -llz4 -lz -ldl

TARGET = ../../bin/supp_workload_driver
SRCS = supp_workload_driver.cc

all: $(TARGET)

$(TARGET): $(SRCS) supp_config.h supp_reference_model.h supp_stats_collector.h
	mkdir -p ../../bin
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SRCS) $(LIBS) -o $(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all clean
