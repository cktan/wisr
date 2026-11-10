/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * log.h -- reporting through the caller's log hook
 * (sm1::set_log): sm0's helpers (sm0_log.h), brought into
 * namespace wisr.
 */
#ifndef WISR_LOG_H
#define WISR_LOG_H

#include "sm0_log.h"

namespace wisr {

using sm0::fatal;
using sm0::log_enabled;
using sm0::log_msg;

} // namespace wisr

#endif /* WISR_LOG_H */
