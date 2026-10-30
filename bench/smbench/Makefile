# Build smbench against the release sm and RocksDB the bench's digres
# links (make -C ../../dg release).
SM = ../../sm
RDB = ../../rocksdb/release/librocksdb.a
CXXFLAGS = -O2 -DNDEBUG -std=c++20 -fno-rtti -I$(SM)/sm3/include \
           -I$(SM)/sm2/include -I$(SM)/sm1/include -I$(SM)/include \
           -I$(SM)/sm0/include -I$(SM)/sm1/src

smbench: smbench.cc $(SM)/build/release/libsm.a
	$(CXX) $(CXXFLAGS) -o $@ $< $(SM)/build/release/libsm.a $(RDB) \
	    -licui18n -licuuc -licudata -lsnappy -llz4 -lzstd -lz -lbz2 \
	    -lpthread -ldl

.PHONY: clean
clean:
	rm -f smbench
