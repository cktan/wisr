/*
 * log.h -- reporting through the caller's log hook (sm_set_log).
 */
#ifndef WISR_LOG_H
#define WISR_LOG_H

#include "sm.h"

namespace wisr {

/*
 * Format a message and hand it to the log hook, if one is set.
 * With no hook this costs one atomic load and formats nothing.
 */
void log_msg(sm_loglevel_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

} // namespace wisr

#endif /* WISR_LOG_H */
