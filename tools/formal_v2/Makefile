CXX = g++
ROCKSDB_ROOT = /home/wam/grad/rocksdb-v11.8.0
ROCKSDB_COMMIT ?= $(shell git -C $(ROCKSDB_ROOT) rev-parse HEAD 2>/dev/null || echo "unknown")
STUDY_COMMIT ?= $(shell git rev-parse HEAD 2>/dev/null || echo "unknown")

CXXFLAGS = -O2 -g -std=c++20 -fno-rtti -DNDEBUG -Wall -Wextra -pthread -DROCKSDB_READ_PATH_AUDIT \
           -DROCKSDB_GIT_COMMIT=\"$(ROCKSDB_COMMIT)\" -DSTUDY_GIT_COMMIT=\"$(STUDY_COMMIT)\"
INCLUDES = -I$(ROCKSDB_ROOT)/include -I$(ROCKSDB_ROOT) -I.
ROCKSDB_LIB = $(ROCKSDB_ROOT)/librocksdb.a
LIBS = $(ROCKSDB_LIB) -lssl -lcrypto -lpthread -lzstd -lsnappy -llz4 -lz -lbz2 -ldl

TARGET = /home/wam/grad/s14-range-delete-study/bin/formal_driver
SRCS = formal_driver.cc

TEST_TARGET = test_double_buffered_histogram
TEST_TSAN_TARGET = test_double_buffered_histogram_tsan

all: $(TARGET)

$(TARGET): $(SRCS) formal_config.h json_parser.h manifest_parser.h thread_local_histogram.h formal_event_listener.h worker_state_model.h kv_verifier.h rtp_mc_controller.h
	mkdir -p /home/wam/grad/s14-range-delete-study/bin
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SRCS) $(LIBS) -o $(TARGET)

test_hist: test_double_buffered_histogram.cc thread_local_histogram.h
	$(CXX) $(CXXFLAGS) -I. test_double_buffered_histogram.cc -o $(TEST_TARGET)

test_hist_tsan: test_double_buffered_histogram.cc thread_local_histogram.h
	$(CXX) $(CXXFLAGS) -fsanitize=thread -I. test_double_buffered_histogram.cc -o $(TEST_TSAN_TARGET)

test: test_hist
	./$(TEST_TARGET)

test_tsan: test_hist_tsan
	setarch $$(uname -m) -R ./$(TEST_TSAN_TARGET)

clean:
	rm -f $(TARGET) $(TEST_TARGET) $(TEST_TSAN_TARGET)

