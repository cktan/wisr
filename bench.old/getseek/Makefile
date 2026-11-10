# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT

# Build getseek against the static release RocksDB the bench's
# digres links (../../rocksdb/release), and sm1's own
# version codec, so "seek" is sm1's read as it is.
SM = ../../sm
RDB = ../../rocksdb/release/librocksdb.a
CXXFLAGS = -O2 -DNDEBUG -std=c++20 -fno-rtti -I$(SM)/sm1/src -I$(SM)/sm0/include

getseek: getseek.cc $(SM)/sm1/src/version.cc
	$(CXX) $(CXXFLAGS) -o $@ $^ $(RDB) -lsnappy -llz4 -lzstd -lz -lbz2 -lpthread -ldl

.PHONY: clean
clean:
	rm -f getseek
