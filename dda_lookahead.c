
/** \file
  \brief Digital differential analyser - this is where we figure out which steppers need to move, and when they need to move
*/

#include "dda_lookahead.h"

#ifdef LOOKAHEAD

#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <math.h>

#include "dda_maths.h"
#include "dda.h"
#include "timer.h"
#include "delay.h"
#include "dda_queue.h"
#include "sersendf.h"
#include "pinio.h"
#include "memory_barrier.h"

#ifdef DEBUG
  // Total number of moves joined together.
  uint32_t lookahead_joined = 0;
  // Moves that did not compute in time to be actually joined.
  uint32_t lookahead_timeout = 0;
#endif

/// \var maximum_jerk_P
/// \brief maximum allowed feedrate jerk on each axis

//static const axes_uint32_t PROGMEM maximum_jerk_P = {
axes_uint32_t maximum_jerk_P = {
  MAX_JERK_X,
  MAX_JERK_Y,
  MAX_JERK_Z,
  MAX_JERK_E
};


/**
 * \brief Find maximum corner speed between two moves.
 * \details Find out how fast we can move around around a corner without
 * exceeding the expected jerk. Worst case this speed is zero, which means a
 * full stop between both moves. Best case it's the lower of the maximum speeds.
 *
 * This function is expected to be called from within dda_create().
 *
 * \param [in] prev is the DDA structure of the move previous to the current one.
 * \param [in] current is the DDA structure of the move currently created.
 *
 * \return dda->crossF
 */
void dda_find_crossing_speed(DDA *prev, DDA *current) {
  uint32_t F, dv,prevF, currF, speed_factor, max_speed_factor;
  //axes_int32_t ;
  enum axis_e i;

  // Bail out if there's nothing to join (e.g. G1 F1500).
  if ( ! prev)
    return;

  // We always look at the smaller of both combined speeds,
  // else we'd interpret intended speed changes as jerk.
  F = prev->endpoint.F;
  if (current->endpoint.F < F)
    F = current->endpoint.F;
  max_speed_factor = (uint32_t)2 << 8;
  if (DEBUG_DDA && (debug_flags & DEBUG_DDA))
    sersendf_P(PSTR("Distance: %lu, then %lu\n"),
               prev->distance, current->distance);

  // Find individual axis speeds.
  // TODO: this is eight expensive muldiv()s. It should be possible to store
  //       currF as prevF for the next calculation somehow, to save 4 of
  //       these 8 muldiv()s.
  //       Caveat: bail out condition above and some other non-continuous
  //               situations might need some extra code for handling.

  
  /**
if (DEBUG_DDA && (debug_flags & DEBUG_DDA))
    sersendf_P(PSTR("prevF: %ld  %ld  %ld  %ld\ncurrF: %ld  %ld  %ld  %ld\n"),
               prevF[X], prevF[Y], prevF[Z], prevF[E],
               currF[X], currF[Y], currF[Z], currF[E]);
   * What we want is (for each axis):
   *
   *   delta velocity = dv = |v1 - v2| < max_jerk
   *
   * In case this isn't satisfied, we can slow down by some factor x until
   * the equitation is satisfied:
   *
   *   x * |v1 - v2| < max_jerk
   *
   * Now computation is pretty straightforward:
   *
   *        max_jerk
   *   x = -----------
   *        |v1 - v2|
   *
   *   if x > 1: continue full speed
   *   if x < 1: v = v_max * x
   *
   * See also: https://github.com/Traumflug/Teacup_Firmware/issues/45
   */


//  for (i = X; i < AXIS_COUNT; i++) { // AXIS_COUNT
//    prevF[i] = prev->delta[i]?muldiv(prev->delta[i], F, prev->total_steps):0;
//  }
  for (i = X; i < AXIS_COUNT; i++) {
    if (!prev->delta[i])
      prevF = muldiv(prev->delta[i], F, prev->total_steps);
    else
      prevF = 0;
    if (!current->delta[i])
      currF = muldiv(current->delta[i], F, current->total_steps);
    else
      currF = 0;
    //if ((prev->delta[i]==0) && (current->delta[i]==0)) continue;  
    //currF = current->delta[i]?muldiv(current->delta[i], F, current->total_steps):0;
    if (get_direction(prev, i) == get_direction(current, i))
      dv = currF > prevF ? currF - prevF : prevF - currF;
    else
      dv = currF + prevF;
    if (dv) {
      speed_factor = ((uint32_t)(maximum_jerk_P[i]) << 8) / dv;
      if (speed_factor < max_speed_factor)
        max_speed_factor = speed_factor;
      if (DEBUG_DDA && (debug_flags & DEBUG_DDA))
        sersendf_P(PSTR("%c: dv %lu of %lu   factor %lu of %lu\n"),
                   'X' + i, dv, (uint32_t)(maximum_jerk_P[i]),
                   speed_factor, (uint32_t)1 << 8);
    }
  }

  if (max_speed_factor >= ((uint32_t)1 << 8))
    current->crossF = F;
  else
    current->crossF = (F * max_speed_factor) >> 8;
  //current->crossF = F;
  if (DEBUG_DDA && (debug_flags & DEBUG_DDA))
    sersendf_P(PSTR("Cross speed reduction from %lu to %lu\n"),
               F, current->crossF);

  return;
}

/**
 * \brief Join 2 moves by removing the full stop between them, where possible.
 * \details To join the moves, the deceleration ramp of the previous move and
 * the acceleration ramp of the current move are shortened, resulting in a
 * non-zero speed at that point. The target speed at the corner is already to
 * be found in dda->crossF. See dda_find_corner_speed().
 *
 * Ideally, both ramps can be reduced to actually have Fcorner at the corner,
 * but the surrounding movements might no be long enough to achieve this speed.
 * Analysing both moves to find the best result is done here.
 *
 * TODO: to achieve better results with short moves (move distance < both ramps),
 *       this function should be able to enhance the corner speed on repeated
 *       calls when reverse-stepping through the movement queue.
 *
 * \param [in] prev is the DDA structure of the move previous to the current one.
 * \param [in] current is the DDA structure of the move currently created.
 *
 * Premise: the 'current' move is not dispatched in the queue: it should remain
 * constant while this function is running.
 *
 * Note: the planner always makes sure the movement can be stopped within the
 * last move (= 'current'); as a result a lot of small moves will still limit the speed.
 */
void dda_join_moves(DDA *prev, DDA *current) {

  // Calculating the look-ahead settings can take a while; before modifying
  // the previous move, we need to locally store any values and write them
  // when we are done (and the previous move is not already active).
  uint32_t prev_F, prev_F_in_steps, prev_F_start_in_steps, prev_F_end_in_steps;
  uint32_t prev_rampup, prev_rampdown, prev_total_steps;
  uint32_t crossF, crossF_in_steps;
  uint8_t prev_id;
  // Similarly, we only want to modify the current move if we have the results of the calculations;
  // until then, we do not want to touch the current move settings.
  // Note: we assume 'current' will not be dispatched while this function runs, so we do not to
  // back up the move settings: they will remain constant.
  uint32_t this_F, this_F_in_steps, this_F_start_in_steps, this_rampup, this_rampdown, this_total_steps, this_fast_axis;
  uint8_t this_id;
  #ifdef LOOKAHEAD_DEBUG
  static uint32_t la_cnt = 0;     // Counter: how many moves did we join?
  static uint32_t moveno = 0;     // Debug counter to number the moves - helps while debugging
  moveno++;
  #endif

  // Bail out if there's nothing to join (e.g. G1 F1500).
   if ( ! prev || current->crossF == 0)
    return;

    // Show the proposed crossing speed - this might get adjusted below.
    if (DEBUG_DDA && (debug_flags & DEBUG_DDA))
      sersendf_P(PSTR("Initial crossing speed: %lu\n"), current->crossF);

  // Make sure we have 2 moves and the previous move is not already active
  if (prev->live == 0) { // originally ==
    // Copy DDA ids to verify later that nothing changed during calculations
    ATOMIC_START
      prev_id = prev->id;
      this_id = current->id;
    ATOMIC_END

    prev_F = prev->endpoint.F;
    prev_F_start_in_steps = prev->start_steps;
    prev_total_steps = prev->total_steps;
    crossF = current->crossF;
    this_total_steps = current->total_steps;
    this_fast_axis = current->fast_axis;

    // Here we have to distinguish between feedrate along the movement
    // direction and feedrate of the fast axis. They can differ by a factor
    // of 2.
    // Along direction: F, crossF.
    // Along fast axis already: start_steps, end_steps.
    //
    // All calculations here are done along the fast axis, so recalculate
    // F and crossF to match this, too.
    prev_F = muldiv(prev->fast_um, prev_F, prev->distance);
    this_F = muldiv(current->fast_um, current->endpoint.F, current->distance);
    crossF = muldiv(current->fast_um, crossF, current->distance);

    prev_F_in_steps = acc_ramp_len(prev_F, this_fast_axis);
    this_F_in_steps = acc_ramp_len(this_F, this_fast_axis);
    crossF_in_steps = acc_ramp_len(crossF, this_fast_axis);

    // Show the proposed crossing speed - this might get adjusted below
    if (DEBUG_DDA && (debug_flags & DEBUG_DDA))
      sersendf_P(PSTR("Initial crossing speed: %lu\n"), crossF_in_steps);

    // Compute the maximum speed we can reach for crossing.
    crossF_in_steps = MIN(crossF_in_steps, this_total_steps);
    crossF_in_steps = MIN(crossF_in_steps, prev_total_steps + prev_F_start_in_steps);

    if (crossF_in_steps == 0)
      return;

    // Build ramps for previous move.
    if (crossF_in_steps == prev_F_in_steps) {
      prev_rampup = prev_F_in_steps - prev_F_start_in_steps;
      prev_rampdown = 0;
    }
    else if (crossF_in_steps < prev_F_start_in_steps) {
      uint32_t extra, limit;

      prev_rampup = 0;
      prev_rampdown = prev_F_start_in_steps - crossF_in_steps;
      extra = (prev_total_steps - prev_rampdown) >> 1;
      limit = prev_F_in_steps - prev_F_start_in_steps;
      extra = MIN(extra, limit);

      prev_rampup += extra;
      prev_rampdown += extra;
    }
    else {
      uint32_t extra, limit;

      prev_rampup = crossF_in_steps - prev_F_start_in_steps;
      prev_rampdown = 0;
      extra = (prev_total_steps - prev_rampup) >> 1;
      limit = prev_F_in_steps - crossF_in_steps;
      extra = MIN(extra, limit);

      prev_rampup += extra;
      prev_rampdown += extra;
    }
    prev_rampdown = prev_total_steps - prev_rampdown;
    prev_F_end_in_steps = crossF_in_steps;

    // Build ramps for current move.
    if (crossF_in_steps == this_F_in_steps) {
      this_rampup = 0;
      this_rampdown = crossF_in_steps;
    }
    else {
      this_rampup = 0;
      this_rampdown = crossF_in_steps;

      uint32_t extra = (this_total_steps - this_rampdown) >> 1;
      uint32_t limit = this_F_in_steps - crossF_in_steps;
      extra = MIN(extra, limit);

      this_rampup += extra;
      this_rampdown += extra;
    }
    this_rampdown = this_total_steps - this_rampdown;
    this_F_start_in_steps = crossF_in_steps;

    if (DEBUG_DDA && (debug_flags & DEBUG_DDA)) {
      sersendf_P(PSTR("prev_F_start: %lu\n"), prev_F_start_in_steps);
      sersendf_P(PSTR("prev_F: %lu\n"), prev_F_in_steps);
      sersendf_P(PSTR("prev_rampup: %lu\n"), prev_rampup);
      sersendf_P(PSTR("prev_rampdown: %lu\n"),
                 prev_total_steps - prev_rampdown);
      sersendf_P(PSTR("crossF: %lu\n"), crossF_in_steps);
      sersendf_P(PSTR("this_rampup: %lu\n"), this_rampup);
      sersendf_P(PSTR("this_rampdown: %lu\n"),
                 this_total_steps - this_rampdown);
      sersendf_P(PSTR("this_F: %lu\n"), this_F_in_steps);
    }

    #ifdef DEBUG
      uint8_t timeout = 0;
    #endif

    ATOMIC_START
      // Evaluation: determine how we did...
      #ifdef DEBUG
        lookahead_joined++;
      #endif

      // Determine if we are fast enough - if not, just leave the moves
      // Note: to test if the previous move was already executed and replaced by a new
      // move, we compare the DDA id.
      if(prev->live == 0 && prev->id == prev_id && current->live == 0 && current->id == this_id) { // >= originally ==
        prev->end_steps = prev_F_end_in_steps;
        prev->rampup_steps = prev_rampup;
        prev->rampdown_steps = prev_rampdown;
        current->rampup_steps = this_rampup;
        current->rampdown_steps = this_rampdown;
        current->end_steps = 0;
        current->start_steps = this_F_start_in_steps;
        #ifdef LOOKAHEAD_DEBUG
          la_cnt++;
        #endif
      }
      #ifdef DEBUG
        else
          timeout = 1;
      #else
        else 
          sersendf_P(PSTR("// Notice: look ahead not fast enough\n"));
      #endif
    ATOMIC_END

    // If we were not fast enough, any feedback will happen outside the atomic block:
    #ifdef DEBUG
      if (timeout) {
        sersendf_P(PSTR("// Notice: look ahead not fast enough\n"));
        lookahead_timeout++;
      }
    #endif
  }
}

#endif /* LOOKAHEAD */
