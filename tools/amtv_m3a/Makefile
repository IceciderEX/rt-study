CXX = g++
AUDIT_ROOT = /home/wam/grad/build-m2d-audit
RELEASE_ROOT = /home/wam/grad/build-m2d-release
STUDY_ROOT = /home/wam/grad/s14-range-delete-study

AUDIT_TARGET = $(STUDY_ROOT)/bin/m3a_driver_audit
RELEASE_TARGET = $(STUDY_ROOT)/bin/m3a_driver_release
TEST_TARGET = $(STUDY_ROOT)/bin/m3a_audit_aggregator_test
SRCS = m3a_driver.cc
TEST_SRCS = m3a_audit_aggregator_test.cc

COMMON_LIBS = -lssl -lcrypto -lpthread -lzstd -lsnappy -llz4 -lz -lbz2 -ldl -lnuma

all: $(AUDIT_TARGET) $(RELEASE_TARGET) $(TEST_TARGET)

$(AUDIT_TARGET): $(SRCS) $(AUDIT_ROOT)/librocksdb.a
	mkdir -p $(STUDY_ROOT)/bin
	$(CXX) -O2 -g -std=c++20 -fno-rtti -DOS_LINUX -D_REENTRANT -DROCKSDB_PLATFORM_POSIX -DROCKSDB_LIB_IO_POSIX -DROCKSDB_READ_PATH_AUDIT -DROCKSDB_PTHREAD_ADAPTIVE_MUTEX -DNDEBUG -Wall -Wextra \
		-I$(AUDIT_ROOT)/include -I$(AUDIT_ROOT) -I$(STUDY_ROOT) \
		$(SRCS) $(AUDIT_ROOT)/librocksdb.a $(COMMON_LIBS) -o $(AUDIT_TARGET)

$(RELEASE_TARGET): $(SRCS) $(RELEASE_ROOT)/librocksdb.a
	mkdir -p $(STUDY_ROOT)/bin
	$(CXX) -O3 -DNDEBUG -std=c++20 -fno-rtti -DOS_LINUX -D_REENTRANT -DROCKSDB_PLATFORM_POSIX -DROCKSDB_LIB_IO_POSIX -DROCKSDB_PTHREAD_ADAPTIVE_MUTEX -Wall -Wextra \
		-I$(RELEASE_ROOT)/include -I$(RELEASE_ROOT) -I$(STUDY_ROOT) \
		$(SRCS) $(RELEASE_ROOT)/librocksdb.a $(COMMON_LIBS) -o $(RELEASE_TARGET)

$(TEST_TARGET): $(TEST_SRCS)
	mkdir -p $(STUDY_ROOT)/bin
	$(CXX) -O2 -g -std=c++20 -Wall -Wextra \
		-I$(AUDIT_ROOT)/include -I$(AUDIT_ROOT) -I$(STUDY_ROOT) \
		$(TEST_SRCS) -lpthread -o $(TEST_TARGET)

clean:
	rm -f $(AUDIT_TARGET) $(RELEASE_TARGET) $(TEST_TARGET)
