CXX ?= g++
CXXFLAGS ?= -O2 -g -std=c++20 -Wall -Wextra -pthread
ROCKSDB_DIR ?= /home/wam/grad/rocksdb-v11.8.0
INCLUDES = -I$(ROCKSDB_DIR)/include -I.
LIBS = $(ROCKSDB_DIR)/build/librocksdb.a -lpthread -lzstd -lsnappy -llz4 -lz -ldl

BIN_DIR = ../bin
TARGET = $(BIN_DIR)/workload_driver

SRCS = workload_driver.cc
HEADERS = config.h reference_model.h stats_collector.h

all: $(TARGET)

$(TARGET): $(SRCS) $(HEADERS)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SRCS) $(LIBS) -o $(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all clean
