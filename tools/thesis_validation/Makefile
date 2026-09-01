CXX = g++
CXXFLAGS = -O2 -g -std=c++20 -Wall -Wextra -pthread
ROCKSDB_DIR = /home/wam/grad/rocksdb-v11.8.0

INCLUDES = -I$(ROCKSDB_DIR)/include -I.
LIBS = $(ROCKSDB_DIR)/build/librocksdb.a -lssl -lcrypto -lpthread -lzstd -lsnappy -llz4 -lz -ldl

TARGET = ../../bin/tv_driver
SRCS = tv_driver.cc

all: $(TARGET)

$(TARGET): $(SRCS) tv_config.h tv_reference_model.h tv_stats_collector.h
	mkdir -p ../../bin
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SRCS) $(LIBS) -o $(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all clean
