/*
 * toast.h -- a datum out of line (../TOAST.md §2): its chunks
 * in a store's TOAST relid, written, read and erased. rel.cc
 * decides which datums go there.
 */
#ifndef SM2_TOAST_H
#define SM2_TOAST_H

#include "sm2.h"

namespace sm2 {

/* The relid of a store's TOAST data (../ROWSTORE.md §8). */
constexpr uint16_t toast_relid = 0xFFFF;

/* Write data, the datum p names, as its chunks. */
int toast_write(sm1::session_t *session, const toast_ptr_t &p,
                std::string_view data);

/* Erase the chunks of the datum p names. */
int toast_erase(sm1::session_t *session, const toast_ptr_t &p);

/*
 * Read the datum p names into out: SM_ERR_NOTFOUND if none of it
 * is there, SM_ERR_CORRUPT if its chunks do not make its length.
 */
int toast_read(sm1::session_t *session, const toast_ptr_t &p, std::string &out);

/*
 * toast_read of a datum whose row was settled, by settled reads, with
 * no transaction (../BTREEINDEX.md §9). No lock is held, so a writer
 * may erase the datum after the row is read: a chunk gone or not
 * settled is SM_ERR_NOTFOUND.
 */
int toast_read_settled(sm1::session_t *session, const toast_ptr_t &p,
                       std::string &out);

} // namespace sm2

#endif
