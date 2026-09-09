#pragma once

#include <algorithm>

// Adaptive draft depth controller for MTP speculative decoding (draft-mtp-adaptive).
//
// Single credit-bucket state machine. Every verification result moves a bucket B
// by (n_accepted - depth), except that a full accept (n_accepted >= depth) is
// flipped to add max(1, n_accepted - 1) instead: a deeper or longer accept is
// worth more, so climbing accelerates with depth, and a strong run by another
// speculator (which can accept far more than the current depth in one round)
// climbs the depth one step per round while it lasts. Any shortfall drains
// depth - n_accepted, i.e. a miss of k tokens subtracts k. There is no special
// case for truncated drafts: a p_min-truncated draft is just a miss of the
// tokens it came up short, which keeps the controller honest about the depth
// it actually holds.
//
// B starts at the drop pressure D = max(60, 10*depth) on every depth change.
// When B reaches the cap T = D + climb_budget() the depth climbs one step; when
// B falls to 0 the depth drops one step. The surplus above the cap (bounded at
// the depth, so at most one step per update) carries into the new depth's
// bucket, and a deficit below zero carries down the same way, instead of being
// discarded.
//
// The bucket's drift (its expected change per verification round) is what
// locates the right depth, and its zero-crossing lands on the throughput
// optimum of every workload measured: code ~9, reasoning/prose/phase-switching
// at the floor, verbatim recall at the ceiling. That is the property the credit
// function above buys, and it needs no tuning. The constants do, because the
// delivery's acceptance is higher than mainline's and therefore raises the
// drift at every depth, which turns a careful upstream compromise into a
// trigger-happy one at the top of the range:
//
//   * climb_budget grows with depth, 20 + 6*(depth - 1). A flat 20 is small
//     enough that a lucky streak of full accepts fills the bucket, and because
//     the credit grows with depth the next few accepts cascade the depth up
//     several steps (measured: 9 -> 10 -> 11 -> 12 in 16 rounds after six
//     consecutive full accepts at depth 8, then a slow drain back down). A
//     depth-scaled budget makes the top of the range resist a streak the way
//     the floor already does.
//
//   * drop_pressure is steeper, max(60, 10*depth) rather than max(20, 4*depth),
//     which damps the slow 6 <-> 12 limit cycle the flat budget produced
//     (measured 40 depth changes in 477 verification rounds, and each change
//     costs a rollback and a re-draft).
//
//   * the cold start is cap - 3 rather than the floor. The expensive direction
//     is the climb: off the floor a step costs ~20 net full accepts, and the
//     controller burned a third of a 3000-token run reaching the plateau --
//     which is the entire advantage of a higher ceiling. Settling *down* is
//     cheap even for a workload whose equilibrium is the floor, because its
//     drift is strongly negative up there, so starting near the plateau costs a
//     reasoning or mixed workload almost nothing while a code workload starts
//     on its optimum.
struct common_speculative_adaptive {
    int n_cur    = 0; // current adaptive draft depth N
    int n_bucket = 0; // accumulated bucket: net accepted surplus over the depth

    // accumulated (n_cur - n_accepted) needed to drop one step from depth N
    int drop_pressure(void) const {
        return std::max(60, n_cur * 10);
    }

    // net full-accept-equivalents needed to climb one step from depth N; grows
    // with depth so one lucky streak cannot cascade the depth upward
    int climb_budget(void) const {
        return 20 + 6 * (n_cur - 1);
    }

    // bucket cap: drop pressure plus the climb budget, so climbing from the
    // reset point costs climb_budget() net full-accept-equivalents
    int bucket_cap(void) const {
        return drop_pressure() + climb_budget();
    }

    // cold start three steps below the ceiling, bounded by the floor; the drift
    // then settles the depth in whichever direction the workload calls for
    void reset(int n_max, int n_min_adaptive) {
        const int cap   = std::max(1, n_max);
        const int floor = std::max(1, n_min_adaptive);

        n_cur    = std::min(cap, std::max(floor, cap - 3));
        n_bucket = drop_pressure();
    }

    // feed one verification result: n_accepted is the number of tokens the
    // target accepted this round. When another speculator (ngram-mod) produced
    // the accepted draft, callers pass its accepted count (which can exceed the
    // current depth), so strong runs by the other speculator are credited like
    // full accepts
    void update(int n_accepted, int n_max, int n_min_adaptive) {
        const int cap   = std::max(1, n_max);
        const int floor = std::max(1, n_min_adaptive);

        int delta;

        if (n_accepted >= n_cur) {
            delta = std::max(1, n_accepted - 1);
        } else {
            delta = n_accepted - n_cur;
        }

        n_bucket += delta;

        // At the bucket boundaries adjust the current depth if possible. Otherwise
        // both n_cur and n_bucket always get clamped to their minimum/maximum values.
        // The surplus above the cap (clamped at the depth, so at most one climb per
        // update) carries into the new depth's bucket, and a deficit below zero
        // carries down the same way, instead of being discarded
        if (n_bucket >= bucket_cap()) {
            // At or above the high water mark
            if (n_cur < cap) {
                n_bucket = std::min(n_bucket, bucket_cap() + n_cur);
                n_bucket -= bucket_cap();
                n_cur++;
                n_bucket += drop_pressure();
            } else {
                n_cur = cap;
                n_bucket = bucket_cap();
            }
        } else if (n_bucket <= 0) {
            // At or below low water mark
            if (n_cur > floor) {
                n_bucket -= drop_pressure();
                n_cur--;
                n_bucket += bucket_cap();
            } else {
                n_cur = floor;
                n_bucket = 0;
            }
        }
    }
};
