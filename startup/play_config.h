#ifndef TOTK_PLAY_CONFIG_H
#define TOTK_PLAY_CONFIG_H
/* Build mode (one switch for the whole DLL; split_page_header.py reads the default from here).
 *
 * TOTK_PLAY=1 (default): the play build. Translated code counts no instructions and tests no
 *   checkpoints: no c->steps updates, no per-block budget/permission/trace checks. c->limit is a run
 *   token: nonzero = keep running, 0 = leave translated code at the next loop head or page entry with
 *   CH_BUDGET (a stop request: multi-core kick, boot_svc's limit hijack). Guest time comes from the
 *   host clock (guest_time.h). The probe still loads such a DLL, but it never stops at a budget.
 * TOTK_PLAY=0: the probe/coverage build (exact instruction counts, budget stops, checkpoints at any
 *   entry, trace ring). Build it with -DTOTK_PLAY=0 on every unit AND TOTK_PLAY=0 in the environment
 *   of split_page_header.py, or flip the default below. */
#ifndef TOTK_PLAY
#define TOTK_PLAY 1
#endif
#endif
