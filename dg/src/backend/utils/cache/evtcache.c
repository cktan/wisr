/*-------------------------------------------------------------------------
 *
 * evtcache.c
 *	  Special-purpose cache for event trigger data.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/evtcache.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/evtcache.h"

/*
 * Search the event cache by trigger event: always empty.  Event triggers
 * are rows of pg_event_trigger, and digres has no catalog tables, so none
 * can exist.
 */
List *EventCacheLookup(EventTriggerEvent event) { return NIL; }
