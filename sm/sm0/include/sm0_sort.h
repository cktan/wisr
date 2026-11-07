/*
 * sm0_sort.h -- a sort of records by key (../SORT_PLAN.md).
 *
 *   sm0::sort_t *s = nullptr;
 *   sm0::sort_open(dir, budget, false, s);
 *   sm0::sort_put(s, key, value);     // any order, any number
 *   sm0::sort_done(s);
 *   while (0 == sm0::sort_next(s, key, value)) { ... }
 *   sm0::sort_free(s);
 *
 * A key is bytes, ordered as memcmp orders them, a shorter key before
 * a longer one it starts. The sort knows no types: sm's encoded keys
 * already order so.
 *
 * A sort has one owner, on one thread. It belongs to no session and
 * no transaction. A call returns 0, or -1 with the thread's error set.
 */
#ifndef SM0_SORT_H
#define SM0_SORT_H

#include <cstddef>
#include <string_view>

namespace sm0 {

struct sort_t;

/*
 * Open a sort of up to budget bytes of memory, which keeps its runs in
 * the directory dir. With unique, two equal keys fail the sort with
 * SM_ERR_DUPKEY. Without, equal keys come back in no set order.
 *
 * Errors: SM_ERR_INVALID (no dir, or no budget).
 */
int sort_open(const char *dir, size_t budget, bool unique, sort_t *&out);

/*
 * Add a record, which the sort copies.
 *
 * Errors: SM_ERR_INVALID (the input has ended), SM_ERR_INTERRUPTED.
 */
int sort_put(sort_t *sort, std::string_view key, std::string_view value);

/*
 * End the input, and order the records.
 *
 * Errors: SM_ERR_INVALID (the input has ended), SM_ERR_DUPKEY,
 * SM_ERR_INTERRUPTED.
 */
int sort_done(sort_t *sort);

/*
 * The next record in key order: 0 with its key and value, good until
 * the next call; 1 past the last.
 *
 * Errors: SM_ERR_INVALID (the input has not ended).
 */
int sort_next(sort_t *sort, std::string_view &key, std::string_view &value);

/* Free a sort at any point; nullptr is a no-op. */
void sort_free(sort_t *sort);

} // namespace sm0

#endif /* SM0_SORT_H */
