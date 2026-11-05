/*
 * pin.cc -- raw pins (../ROWSTORE.md §6.3): a raw pin keeps the iterator
 * that found a row, so the row is handed out without a copy, until
 * the pin is emptied. The caller owns each one; sm1 keeps no list
 * of them, and sm3 keeps the pool its clients' pins come from.
 */

#include "error.h"
#include "session.h"

using namespace wisr;

/* Make an empty raw pin; see sm1.h. */
sm1::rawpin_t *sm1::rawpin_new() {
  // Allocated as a handle, so sm1.h shows no RocksDB type.
  return new sm1::rawpin_t;
}

/* Empty a raw pin, then free it. */
void sm1::rawpin_free(sm1::rawpin_t *pin) { delete pin; }

/* Let go of the row a raw pin holds. */
void sm1::unpin(sm1::rawpin_t *pin) {
  if (!pin) {
    return;
  }
  // Let go of the iterator, and with it the blocks and the
  // SuperVersion it holds.
  pin->it.reset();
  pin->got.Reset();
  pin->row = rocksdb::Slice();
  pin->reuse = {};
}
