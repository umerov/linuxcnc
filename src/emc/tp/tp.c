/********************************************************************
* Description: tp.c
*   Trajectory planner based on TC elements
*
*   Derived from a work by Fred Proctor & Will Shackleford
*
* Author:
* License: GPL Version 2
* System: Linux
*
* Copyright (c) 2004 All rights reserved.
********************************************************************/

#include "rtapi.h"		/* rtapi_print_msg */
#include "rtapi_string.h"       /* NULL */
#include "posemath.h"
#include "tc.h"
#include "tp.h"
#include "emcpose.h"
#include "rtapi_math.h"
#include "../motion/motion.h"
#include "hal.h"
#include "../motion/mot_priv.h"
#include "motion_debug.h"
#include "motion_types.h"
#include "spherical_arc.h"

/**
 * @section tpdebugflags TP debugging flags
 * Enable / disable various debugging functions here.
 * These flags control debug printing from RTAPI. These functions are
 * admittedly kludged on top of the existing rtapi_print framework. As written,
 * though, it's an easy way to selectively compile functions as static or not,
 * and selectively compile in assertions and debug printing.
 */

#include "tp_debug.h"

#ifndef SIM
//Need manual definitions for these functions since they're missing from rtapi_math.h
STATIC inline double fmax(double a, double b) { return (a) > (b) ? (a) : (b); }
STATIC inline double fmin(double a, double b) { return (a) < (b) ? (a) : (b); }
#endif

#define TP_SHOW_BLENDS
#define TP_OPTIMIZATION_LAZY
#define TP_PEDANTIC

extern emcmot_status_t *emcmotStatus;
extern emcmot_debug_t *emcmotDebug;
extern emcmot_config_t *emcmotConfig;

/** static function primitives */
STATIC int tpComputeBlendVelocity(TP_STRUCT const * const tp, TC_STRUCT * const tc,
        TC_STRUCT * const nexttc, int planning, double * const blend_vel);

STATIC int tpCheckEndCondition(TP_STRUCT const * const tp, TC_STRUCT * const tc);

STATIC int tpUpdateCycle(TP_STRUCT * const tp,
        TC_STRUCT * const tc);

STATIC int tpRunOptimization(TP_STRUCT * const tp);
STATIC double saturate(double x, double max);

STATIC int tpCreateArcArcBlend(TP_STRUCT * const tp, TC_STRUCT const * const prev_tc, TC_STRUCT const * const tc, TC_STRUCT * const blend_tc);
STATIC inline int tpAddSegmentToQueue(TP_STRUCT * const tp, TC_STRUCT * const tc, int inc_id);

//Empty function to act as an assert for GDB in simulation
int gdb_fake_catch(int condition){
    return condition;
}

int gdb_fake_assert(int condition){
    if (condition) {
        return gdb_fake_catch(condition);
    }
    return condition;
}

/**
 * @section tputil Utility functions
 * Simple utility functions to perform common operations.
 */

#if 0
/**
 * Simple signum-like function to get sign of a double.
 * There's probably a better way to do this...
 */
STATIC double fsign(double f) {
    if (f>0) {
        return 1.0;
    } else if (f < 0) {
        return -1.0;
    } else {
        //Technically this should be NAN but that's a useless result for tp purposes
        return 0;
    }
}
#endif

/**
 * Saturate a value x to be within +/- max.
 */
STATIC double saturate(double x, double max) {
    if ( x > max ) {
        return max;
    }
    else if ( x < (-max) ) {
        return -max;
    }
    else {
        return x;
    }
}


/** In-place saturation function */
STATIC int sat_inplace(double * const x, double max) {
    if ( *x > max ) {
        *x = max;
        return 1;
    }
    else if ( *x < -max ) {
        *x = -max;
        return -1;
    }
    return 0;
}


/** Clip the input at the specified minimum (in place). */
STATIC int clip_min(double * const x, double min) {
    if ( *x < min ) {
        *x = min;
        return 1;
    }
    return 0;
}


/** Clip the input at the specified maximum (in place). */
STATIC int clip_max(double * const x, double max) {
    if ( *x > max ) {
        *x = max;
        return 1;
    }
    return 0;
}

/**
 * @section tpcheck Internal state check functions.
 * These functions compartmentalize some of the messy state checks.
 * Hopefully this makes changes easier to track as much of the churn will be on small functions.
 */


/**
 * Check if the tail of the queue has a parabolic blend condition and update tc appropriately.
 * This sets flags so that accelerations are correct due to the current segment
 * having to blend with the previous.
 */
STATIC int tcCheckLastParabolic(TC_STRUCT * const tc,
        TC_STRUCT const * const prev_tc) {
    if (prev_tc && prev_tc->term_cond == TC_TERM_COND_PARABOLIC) {
        tp_debug_print("prev segment parabolic, flagging blend_prev\n");
        tc->blend_prev = 1;
    }
    return TP_ERR_OK;
}

/**
 * Returns true if there is motion along ABC or UVW axes, false otherwise.
 */
STATIC int tpRotaryMotionCheck(TP_STRUCT const * const tp, TC_STRUCT const * const tc) {
    switch (tc->motion_type) {
        //Note lack of break statements due to every path returning
        case TC_RIGIDTAP:
            return false;
        case TC_LINEAR:
            if (tc->coords.line.abc.tmag_zero && tc->coords.line.uvw.tmag_zero) {
                return false;
            } else {
                return true;
            }
        case TC_CIRCULAR:
            if (tc->coords.circle.abc.tmag_zero && tc->coords.circle.uvw.tmag_zero) {
                return false;
            } else {
                return true;
            }
        case TC_SPHERICAL:
            return true;
        default:
            tp_debug_print("Unknown motion type!\n");
            return false;
    }
}


/** Returns true if segment has ONLY rotary motion, false otherwise. */
STATIC int tpPureRotaryCheck(TP_STRUCT const * const tp, TC_STRUCT const * const tc)
{
    return (tc->motion_type == TC_LINEAR) &&
        (tc->coords.line.xyz.tmag_zero) &&
        (tc->coords.line.uvw.tmag_zero);
}


/**
 * @section tpgetset Internal Get/Set functions
 * @brief Calculation / status functions for commonly used values.
 * These functions return the "actual" values of things like a trajectory
 * segment's feed override, while taking into account the status of tp itself.
 */


/**
 * Get a safe maximum acceleration based on X,Y, and Z.
 * Use the lowest bound on the linear axes, rather than using the
 * trajectory max accels. These are computed with the infinity norm, which
 * means we can't just assume that the smaller of the two is within the limits.
 */
STATIC int tpGetMachineAccelLimit(double * const acc_limit) {
    if (!acc_limit) {
        return TP_ERR_FAIL;
    }

    //FIXME check for number of axes first!
    double x = emcmotDebug->joints[0].acc_limit;
    double y = emcmotDebug->joints[1].acc_limit;
    double z = emcmotDebug->joints[2].acc_limit;

    *acc_limit=fmin(fmin(x,y),z);
    tp_debug_print(" arc blending a_max=%f\n", *acc_limit);
    return TP_ERR_OK;
}


/** Calculate the minimum of the three values in a PmCartesian. */
STATIC double pmCartMin(PmCartesian const * const in)
{
    return fmin(fmin(in->x,in->y),in->z);
}


/**
 * Calculate the diameter of a circle incscribed on a central cross section of a 3D
 * rectangular prism.
 *
 * @param normal normal direction of plane slicing prism.
 * @param extents distance from center to one corner of the prism.
 * @param diameter diameter of inscribed circle on cross section.
 *
 */
STATIC int tpGetPlanarLimit(PmCartesian const * const normal,
        PmCartesian * const extents, double * const diameter)
{
    if (!normal ) {
        return TP_ERR_MISSING_INPUT;
    }

    PmCartesian planar_x,planar_y,planar_z;

    //Find perpendicular component of unit directions
    // FIXME Assumes normal is unit length
    // FIXME use plane project?
    pmCartScalMult(normal, -normal->x, &planar_x);
    pmCartScalMult(normal, -normal->y, &planar_y);
    pmCartScalMult(normal, -normal->z, &planar_z);

    planar_x.x+=1.0;
    planar_y.y+=1.0;
    planar_z.z+=1.0;

    pmCartAbs(&planar_x, &planar_x);
    pmCartAbs(&planar_y, &planar_y);
    pmCartAbs(&planar_z, &planar_z);

    PmCartesian planar_scales;
    pmCartMag(&planar_x, &planar_scales.x);
    pmCartMag(&planar_y, &planar_scales.y);
    pmCartMag(&planar_z, &planar_scales.z);

    pmCartCartDiv(extents, &planar_scales, extents);

    *diameter = pmCartMin(extents);
    return TP_ERR_OK;
}


/**
 * Calculate acceleration bounds for blend arcs based on the plane containing
 * the two lines.
 * Since two linear moves will always lie in a common plane, a blend arc
 * between them will also lie in that plane, as will the acceleration vector.
 * This is useful if one axis has a low acceleration compared to the other two.
 * Calculating limits in the plane means that a slow Z axis will not affect
 * XY-only moves.
 */
STATIC int tpGetPlanarAccelLimit(PmCartesian const * const normal,
        double * const acc_limit)
{
    PmCartesian acc_bound;
    acc_bound.x = emcmotDebug->joints[0].acc_limit;
    acc_bound.y = emcmotDebug->joints[1].acc_limit;
    acc_bound.z = emcmotDebug->joints[2].acc_limit;
    int res = TP_ERR_OK;
    if (acc_bound.x == acc_bound.y && acc_bound.y == acc_bound.z) {
        *acc_limit = acc_bound.x;
    } else {
        res = tpGetPlanarLimit(normal, &acc_bound, acc_limit);
    }
    return res;
}

STATIC int tpGetPlanarVelLimit(PmCartesian const * const normal,
        double * const vel_limit)
{
    PmCartesian vel_bound;
    vel_bound.x = emcmotDebug->joints[0].vel_limit;
    vel_bound.y = emcmotDebug->joints[1].vel_limit;
    vel_bound.z = emcmotDebug->joints[2].vel_limit;
    int res = TP_ERR_OK;
    if (vel_bound.x == vel_bound.y && vel_bound.y == vel_bound.z) {
        *vel_limit = vel_bound.x;
    } else {
        res = tpGetPlanarLimit(normal, &vel_bound, vel_limit);
    }
    return res;
}

/**
 * Get a same maximum velocity for XYZ.
 * This function returns the worst-case safe velocity in any direction along XYZ.
 */
STATIC int tpGetMachineVelLimit(double * const vel_limit) {

    if (!vel_limit) {
        return TP_ERR_FAIL;
    }

    //FIXME check for number of axes first!
    double x = emcmotDebug->joints[0].vel_limit;
    double y = emcmotDebug->joints[1].vel_limit;
    double z = emcmotDebug->joints[2].vel_limit;

    *vel_limit = fmin(fmin(x,y),z);
    tp_debug_print(" arc blending v_max=%f\n", *vel_limit);
    return TP_ERR_OK;
}

/**
 * Get a segment's feed scale based on the current planner state and emcmotStatus.
 * @note depends on emcmotStatus for system information.
 */
STATIC double tpGetFeedScale(TP_STRUCT const * const tp,
        TC_STRUCT const * const tc) {
    //All reasons to disable feed override go here
    if (tp->pausing || tp->aborting) {
        tc_debug_print("pausing or aborting\n");
        return 0.0;
    } else if (tc->canon_motion_type == EMC_MOTION_TYPE_TRAVERSE || tc->synchronized == TC_SYNC_POSITION ) {
        return 1.0;
    } else {
        return emcmotStatus->net_feed_scale;
    }
}


/**
 * Get target velocity for a tc based on the trajectory planner state.
 * This gives the requested velocity, capped by the segments maximum velocity.
 */
STATIC inline double tpGetRealTargetVel(TP_STRUCT const * const tp,
        TC_STRUCT const * const tc) {
    return fmin(tc->target_vel * tpGetFeedScale(tp,tc),tc->maxvel);
}


/**
 * Get the worst-case target velocity for a segment based on the trajectory planner state.
 */
STATIC inline double tpGetMaxTargetVel(TP_STRUCT const * const tp, TC_STRUCT const * const tc) {
    double tc_maxvel = tc->maxvel;
    return fmin(tc->target_vel * emcmotConfig->maxFeedScale, tc_maxvel);
}


/**
 * Get final velocity for a tc based on the trajectory planner state.
 * This function factors in the feed override and TC limits. It clamps the
 * final velocity to the maximum velocity and the current target velocity.
 */
STATIC inline double tpGetRealFinalVel(TP_STRUCT const * const tp,
        TC_STRUCT const * const tc, double target_vel) {
    /* If we're stepping, then it doesn't matter what the optimization says, we want to end at a stop.
     * If the term_cond gets changed out from under us, detect this and force final velocity to zero
     */
    if (emcmotDebug->stepping || tc->term_cond != TC_TERM_COND_TANGENT) {
        return 0.0;
    } else {
        //Clamp final velocity to the max velocity we can achieve
        double finalvel = tc->finalvel * fmin(tpGetFeedScale(tp,tc),1.0);
        if (finalvel > target_vel) {
            finalvel = target_vel;
        }
        return finalvel;
    }
}

/**
 * Get acceleration for a tc based on the trajectory planner state.
 */
STATIC inline double tpGetScaledAccel(TP_STRUCT const * const tp,
        TC_STRUCT const * const tc) {
    double a_scale = tc->maxaccel;
    /* Parabolic blending conditions: If the next segment or previous segment
     * has a parabolic blend with this one, acceleration is scaled down by 1/2
     * so that the sum of the two does not exceed the maximum.
     */
    if (tc->term_cond == TC_TERM_COND_PARABOLIC || tc->blend_prev) {
        a_scale *= 0.5;
    }
    if (tc->motion_type == TC_CIRCULAR || tc->motion_type == TC_SPHERICAL) {
        //Limit acceleration for cirular arcs to allow for normal acceleration
        a_scale *= TP_ACC_RATIO_TANGENTIAL;
    }
    return a_scale;
}

/**
 * Cap velocity based on trajectory properties
 */
STATIC inline double tpGetSampleVelocity(double vel, double length, double dt) {
    //FIXME div by zero check
    double v_sample = length / dt;
    return fmin(vel,v_sample);
}

/**
 * Convert the 2-part spindle position and sign to a signed double.
 */
STATIC inline double tpGetSignedSpindlePosition(double spindle_pos, int spindle_dir) {
    if (spindle_dir < 0.0) {
        spindle_pos*=-1.0;
    }
    return spindle_pos;
}

/**
 * @section tpaccess tp class-like API
 */

/**
 * Create the trajectory planner structure with an empty queue.
 */
int tpCreate(TP_STRUCT * const tp, int _queueSize, TC_STRUCT * const tcSpace)
{
    if (0 == tp) {
        return TP_ERR_FAIL;
    }

    if (_queueSize <= 0) {
        tp->queueSize = TP_DEFAULT_QUEUE_SIZE;
    } else {
        tp->queueSize = _queueSize;
    }

    /* create the queue */
    if (-1 == tcqCreate(&tp->queue, tp->queueSize, tcSpace)) {
        return TP_ERR_FAIL;
    }

    /* init the rest of our data */
    return tpInit(tp);
}

/**
 * Clears any potential DIO toggles and anychanged.
 * If any DIOs need to be changed: dios[i] = 1, DIO needs to get turned on, -1
 * = off
 */
int tpClearDIOs(TP_STRUCT * const tp) {
    //XXX: All IO's will be flushed on next synced aio/dio! Is it ok?
    int i;
    tp->syncdio.anychanged = 0;
    tp->syncdio.dio_mask = 0;
    tp->syncdio.aio_mask = 0;
    for (i = 0; i < num_dio; i++) {
        tp->syncdio.dios[i] = 0;
    }
    for (i = 0; i < num_aio; i++) {
        tp->syncdio.aios[i] = 0;
    }

    return TP_ERR_OK;
}

/**
 *    "Soft initialize" the trajectory planner tp.
 *    This is a "soft" initialization in that TP_STRUCT configuration
 *    parameters (cycleTime, vMax, and aMax) are left alone, but the queue is
 *    cleared, and the flags are set to an empty, ready queue. The currentPos
 *    is left alone, and goalPos is set to this position.  This function is
 *    intended to put the motion queue in the state it would be if all queued
 *    motions finished at the current position.
 */
int tpClear(TP_STRUCT * const tp)
{
    tcqInit(&tp->queue);
    tp->queueSize = 0;
    tp->goalPos = tp->currentPos;
    tp->nextId = 0;
    tp->execId = 0;
    tp->motionType = 0;
    tp->termCond = TC_TERM_COND_PARABOLIC;
    tp->tolerance = 0.0;
    tp->done = 1;
    tp->depth = tp->activeDepth = 0;
    tp->aborting = 0;
    tp->pausing = 0;
    tp->synchronized = 0;
    tp->uu_per_rev = 0.0;
    emcmotStatus->spindleSync = 0;
    emcmotStatus->current_vel = 0.0;
    emcmotStatus->requested_vel = 0.0;
    emcmotStatus->distance_to_go = 0.0;
    ZERO_EMC_POSE(emcmotStatus->dtg);

    return tpClearDIOs(tp);
}

/**
 * Fully initialize the tp structure.
 * Sets tp configuration to default values and calls tpClear to create a fresh,
 * empty queue.
 */
int tpInit(TP_STRUCT * const tp)
{
    tp->cycleTime = 0.0;
    //Velocity limits
    tp->vLimit = 0.0;
    tp->ini_maxvel = 0.0;
    //Accelerations
    tp->aLimit = 0.0;
    tpGetMachineAccelLimit(&tp->aMax);
    //Angular limits
    tp->wMax = 0.0;
    tp->wDotMax = 0.0;

    tp->spindle.offset = 0.0;
    tp->spindle.revs = 0.0;
    tp->spindle.waiting_for_index = MOTION_INVALID_ID;
    tp->spindle.waiting_for_atspeed = MOTION_INVALID_ID;

    ZERO_EMC_POSE(tp->currentPos);

    tpGetMachineVelLimit(&tp->vMax);

    return tpClear(tp);
}

/**
 * Set the cycle time for the trajectory planner.
 */
int tpSetCycleTime(TP_STRUCT * const tp, double secs)
{
    if (0 == tp || secs <= 0.0) {
        return TP_ERR_FAIL;
    }

    tp->cycleTime = secs;

    return TP_ERR_OK;
}

/**
 * Set requested velocity and absolute maximum velocity (bounded by machine).
 * This is called before adding lines or circles, specifying vMax (the velocity
 * requested by the F word) and ini_maxvel, the max velocity possible before
 * meeting a machine constraint caused by an AXIS's max velocity.  (the TP is
 * allowed to go up to this high when feed override >100% is requested)  These
 * settings apply to subsequent moves until changed.
 */
int tpSetVmax(TP_STRUCT * const tp, double vMax, double ini_maxvel)
{
    if (0 == tp || vMax <= 0.0 || ini_maxvel <= 0.0) {
        return TP_ERR_FAIL;
    }

    tp->vMax = vMax;
    tp->ini_maxvel = ini_maxvel;

    return TP_ERR_OK;
}

/**
 * (?) Set the tool tip maximum velocity.
 * I think this is the [TRAJ] max velocity. This should be the max velocity of
 * const the TOOL TIP, not necessarily any particular axis. This applies to
 * subsequent moves until changed.
 */
int tpSetVlimit(TP_STRUCT * const tp, double vLimit)
{
    if (!tp) return TP_ERR_FAIL;

    if (vLimit < 0.)
        tp->vLimit = 0.;
    else
        tp->vLimit = vLimit;

    return TP_ERR_OK;
}

/** Sets the max acceleration for the trajectory planner. */
int tpSetAmax(TP_STRUCT * const tp, double aMax)
{
    if (0 == tp || aMax <= 0.0) {
        return TP_ERR_FAIL;
    }

    tp->aMax = aMax;

    return TP_ERR_OK;
}

/**
 * Sets the id that will be used for the next appended motions.
 * nextId is incremented so that the next time a motion is appended its id will
 * be one more than the previous one, modulo a signed int. If you want your own
 * ids for each motion, call this before each motion you append and stick what
 * you want in here.
 */
int tpSetId(TP_STRUCT * const tp, int id)
{

    if (!MOTION_ID_VALID(id)) {
        rtapi_print_msg(RTAPI_MSG_ERR, "tpSetId: invalid motion id %d\n", id);
        return TP_ERR_FAIL;
    }

    if (0 == tp) {
        return TP_ERR_FAIL;
    }

    tp->nextId = id;

    return TP_ERR_OK;
}

/** Returns the id of the last motion that is currently
  executing.*/
int tpGetExecId(TP_STRUCT * const tp)
{
    if (0 == tp) {
        return TP_ERR_FAIL;
    }

    return tp->execId;
}

/**
 * Sets the termination condition for all subsequent queued moves.
 * If cond is TC_TERM_COND_STOP, motion comes to a stop before a subsequent move
 * begins. If cond is TC_TERM_COND_PARABOLIC, the following move is begun when the
 * current move slows below a calculated blend velocity.
 */
int tpSetTermCond(TP_STRUCT * const tp, int cond, double tolerance)
{
    if (!tp) {
        return TP_ERR_FAIL;
    }

    switch (cond) {
        //Purposeful waterfall for now
        case TC_TERM_COND_PARABOLIC:
        case TC_TERM_COND_TANGENT:
        case TC_TERM_COND_STOP:
            tp->termCond = cond;
            tp->tolerance = tolerance;
            break;
        default:
            //Invalid condition
            return  -1;
    }

    return TP_ERR_OK;
}

/**
 * Used to tell the tp the initial position.
 * It sets the current position AND the goal position to be the same.  Used
 * only at TP initialization and when switching modes.
 */
int tpSetPos(TP_STRUCT * const tp, EmcPose pos)
{
    if (0 == tp) {
        return TP_ERR_FAIL;
    }

    tp->currentPos = pos;
    tp->goalPos = pos;

    return TP_ERR_OK;
}

/**
 * Check for valid tp before queueing additional moves.
 */
int tpErrorCheck(TP_STRUCT const * const tp) {

    if (!tp) {
        rtapi_print_msg(RTAPI_MSG_ERR, "TP is null\n");
        return TP_ERR_FAIL;
    }
    if (tp->aborting) {
        rtapi_print_msg(RTAPI_MSG_ERR, "TP is aborting\n");
        return TP_ERR_FAIL;
    }
    return TP_ERR_OK;
}


/**
 * Initialize a new queue segment with common parameters.
 * This function is mostly to save space in the tpAddXXX functions, since they
 * get pretty long. If you need a custom setting, overwrite your particular
 * field after calling this function.
 */
STATIC inline int tpInitializeNewSegment(TP_STRUCT const * const tp,
        TC_STRUCT * const tc, double vel, double ini_maxvel, double acc,
        unsigned char enables) {

    /** Segment settings passed down from interpreter*/
    tc->enables = enables;
    tc->cycle_time = tp->cycleTime;

    tc->id = -1; //ID to be set when added to queue (may change before due to blend arcs)

    tc->maxaccel = acc;

    //Always clamp max velocity by sample rate, since we require TP to hit every segment at least once.
    tc->maxvel = ini_maxvel;

    tc->reqvel = vel;
    tc->target_vel = vel;

    /** Segment settings (given values during setup optimization) */
    tc->blend_prev = 0;
    tc->optimization_state = 0;
    tc->finalvel = 0.0;
    tc->accel_mode = TC_ACCEL_TRAPZ;

    /** Segment status flags that are used during trajectory execution. */
    tc->active = 0;

    tc->progress = 0.0;
    tc->nominal_length = tc->target;

    tc->sync_accel = 0;
    tc->currentvel = 0.0;

    tc->vel_at_blend_start = 0.0;
    tc->blend_vel = 0.0;
    tc->blending_next = 0;
    tc->on_final_decel = 0;

    tc->splitting = 0;
    tc->remove = 0;
    tc->active_depth = 1;

    tc->finalized = 0;

    return TP_ERR_OK;
}


/**
 * Find the "peak" velocity a segment can acheive if its velocity profile is triangular.
 * This is used to estimate blend velocity, though by itself is not enough
 * (since requested velocity and max velocity could be lower).
 */
STATIC double tpCalculateTriangleVel(TP_STRUCT const * const tp, TC_STRUCT * const tc) {
    //Compute peak velocity for blend calculations
    double acc_scaled = tpGetScaledAccel(tp, tc);
    double triangle_vel = pmSqrt( acc_scaled * tc->target);
    return triangle_vel;
}


/**
 * Find the maximum angle allowed between "tangent" segments.
 * @param v speed of motion in worst case (i.e. at max feed).
 * @param acc magnitude of acceleration allowed during "kink".
 *
 * Since we are discretized by a timestep, the maximum allowable
 * "kink" in a trajectory is bounded by normal acceleration. A small
 * kink will effectively be one step along the tightest radius arc
 * possible at a given speed.
 */
STATIC inline double tpMaxTangentAngle(TP_STRUCT const * const tp, double v, double acc) {
    double dx = v / tp->cycleTime;
    if (dx > 0.0) {
        return (acc / dx);
    } else {
        tp_debug_print(" Velocity or period is negative!\n");
        //Should not happen...
        return TP_ANGLE_EPSILON;
    }
}


/**
 * Somewhat redundant function to calculate the segment intersection angle.
 * The intersection angle is half of the supplement of the "divergence" angle
 * between unit vectors. If two unit vectors are pointing in the same
 * direction, then the intersection angle is PI/2. This is based on the
 * simple_tp formulation for tolerances.
 */
STATIC inline int tpFindIntersectionAngle(PmCartesian const * const u1,
        PmCartesian const * const u2, double * const theta) {
    double dot;
    pmCartCartDot(u1, u2, &dot);

    /*tp_debug_print("u1 = %f %f %f u2 = %f %f %f\n", u1->x, u1->y, u1->z, u2->x, u2->y, u2->z);*/

    if (dot > 1.0 || dot < -1.0) {
        tp_debug_print("dot product %f outside domain of acos!\n",dot);
        sat_inplace(&dot,1.0);
    }

    *theta = acos(-dot)/2.0;
    return TP_ERR_OK;
}


/**
 * Calculate the angle between two unit cartesian vectors.
 */
STATIC inline int tpCalculateUnitCartAngle(PmCartesian const * const u1, PmCartesian const * const u2, double * const theta) {
    double dot;
    pmCartCartDot(u1, u2, &dot);

    if (dot > 1.0 || dot < -1.0) {
        tp_debug_print("dot product %f outside domain of acos!\n",dot);
        sat_inplace(&dot,1.0);
    }

    *theta = acos(dot);
    return TP_ERR_OK;
}


/**
 * Initialize a blend arc from its parent lines.
 * This copies and initializes properties from the previous and next lines to
 * initialize a blend arc. This function does not handle connecting the
 * segments together, however.
 */
STATIC int tpInitBlendArc(TP_STRUCT const * const tp, TC_STRUCT const * const prev_line_tc,
        TC_STRUCT* const blend_tc, double vel, double ini_maxvel, double acc) {

    if (tpErrorCheck(tp)<0) return TP_ERR_FAIL;

    // Treating arc as extension of prev_line_tc
    blend_tc->atspeed = prev_line_tc->atspeed;

    blend_tc->motion_type = TC_SPHERICAL;

#ifdef TP_SHOW_BLENDS
    blend_tc->canon_motion_type = EMC_MOTION_TYPE_ARC;
#else
    blend_tc->canon_motion_type = prev_line_tc->canon_motion_type;
#endif

    blend_tc->synchronized = prev_line_tc->synchronized;
    blend_tc->uu_per_rev = prev_line_tc->uu_per_rev;
    blend_tc->indexrotary = -1;
    blend_tc->enables = prev_line_tc->enables;

    blend_tc->syncdio = prev_line_tc->syncdio; //enqueue the list of DIOs that need toggling

    if (tpErrorCheck(tp)<0){
        return TP_ERR_FAIL;
    }

    // find "helix" length
    double length;
    arcLength(&blend_tc->coords.arc.xyz, &length);
    blend_tc->target = length;
    //Blend arc specific settings:
    tcSetTermCond(blend_tc, TC_TERM_COND_TANGENT);
    blend_tc->tolerance = 0.0;

    //KLUDGE this init function is a bit overkill now...
    tpInitializeNewSegment(tp, blend_tc, vel, ini_maxvel, acc, prev_line_tc->enables);
    //NOTE: blend arc radius and everything else is finalized, so set this to 1.
    //In the future, radius may be adjustable.
    blend_tc->finalized = 1;

    return TP_ERR_OK;
}


STATIC int tcFindBlendTolerance(TC_STRUCT const * const prev_tc,
        TC_STRUCT const * const tc, double * const T_blend, double * const nominal_tolerance)
{
    const double tolerance_ratio = 0.25;
    double T1 = prev_tc->tolerance;
    double T2 = tc->tolerance;
    //Detect zero tolerance = no tolerance and force to reasonable maximum
    if (T1 == 0) {
        T1 = prev_tc->nominal_length * tolerance_ratio;
    }
    if (T2 == 0) {
        T2 = tc->nominal_length * tolerance_ratio;
    }
    *nominal_tolerance = fmin(T1,T2);
    //Blend tolerance is the limit of what we can reach by blending alone,
    //consuming half a segment or less (parabolic equivalent)
    double blend_tolerance = fmin(fmin(*nominal_tolerance, 
                prev_tc->nominal_length * tolerance_ratio),
            tc->nominal_length * tolerance_ratio);
    *T_blend = blend_tolerance;
    return TP_ERR_OK;
}

/**
 * "Finalizes" a segment so that its length can't change.
 * By setting the finalized flag, we tell the optimizer that this segment's
 * length won't change anymore. Since any blends are already set up, we can
 * trust that the length will be the same, and so can use the length in the
 * velocity optimization.
 */
STATIC int tpFinalizeSegmentLength(TP_STRUCT const * const tp, TC_STRUCT * const tc) {
    //Apply velocity corrections
    if (!tc) {
        tp_debug_print("Missing prev_tc in finalize!\n");
        return TP_ERR_FAIL;
    }
    tp_debug_print("Finalizing tc id %d, type %d\n", tc->id, tc->motion_type);
    //TODO function to check for parabolic?
    int parabolic = (tc->blend_prev || tc->term_cond == TC_TERM_COND_PARABOLIC);
    tp_debug_print("blend_prev = %d, term_cond = %d\n",tc->blend_prev, tc->term_cond);

    if (tc->motion_type == TC_CIRCULAR && parabolic) {
        tp_debug_print("Setting parabolic maxvel\n");
        //TODO make this 0.5 a constant
        tc->maxvel *= pmSqrt(0.5);
    }
    tc->finalized = 1;
    return TP_ERR_OK;
}

STATIC int quadraticFormula(double A, double B, double C, double * const root0,
        double * const root1)
{
    double disc = pmSq(B) - 4.0 * A * C;
    if (disc < 0) {
        tp_debug_print("discriminant < 0\n");
        return TP_ERR_FAIL;
    }
    double t1 = pmSqrt(disc);
    *root0 = ( -B + t1) / (2.0 * A);
    *root1 = ( -B - t1) / (2.0 * A);
    return TP_ERR_OK;
}

STATIC int tpFindArcLineDist(double a, double b, double R1, double T,
        int convex, double * const d)
{
    /* Compute distance along line segment where tangent arc with tolerance T will hit.*/
    /*   a = (P-C1) . u2*/
    /*   b = (P-C1) . n2*/

    double sgn = 1.0;
    if (convex) {
        sgn=-1.0;
    } 

    double A = T/(b-sgn*R1)-1.0;
    double B = T*a/(b-sgn*R1);
    double C = pmSqrt(T);;

    double d0=0;
    double d1=0;

    quadraticFormula(A, B, C, &d0, &d1);
    if (d0>0 && d1>0) {
        *d=fmin(d0,d1);
    } else {
        *d=fmax(d0,d1);
    }
    return TP_ERR_OK;

}

STATIC int tpFindRadiusFromDist(double a, double b, double R1, double d, int convex, double * const R)
{
    //For the arc-line case, when a distance d is specified, find the corresponding radius
    double sgn = 1.0;
    if (convex) {
        sgn = -1;
    }

    double den = (R1-sgn*b);
    if (fabs(den) < TP_POS_EPSILON) {
        return TP_ERR_FAIL;
    }

    *R = sgn * (pmSq(d) / 2.0 + a * d) / den;

    return TP_ERR_OK;

}

STATIC int tpFindDistFromRadius(double a, double b, double R1, double R, int convex, double * const d)
{
    double sgn = 1.0;
    if (convex) {
        sgn=-1;
    }

    double K1=pmSq(b+R);
    double K2=pmSq(R1+sgn*R);
    double d0=0;
    double d1=0;

    quadraticFormula(1.0,2.0*a,K1-K2+pmSq(a),&d0,&d1);
    if (d0>0 && d1>0){
        *d=fmin(d0,d1);
    } else {
        *d=fmax(d0,d1);
    }
    return TP_ERR_OK;
}


STATIC int tpLineArcConvexTest(PmCartesian const * const C1,
        PmCartesian const * const P, PmCartesian const * const u2)
{
    //Check if an arc-line intersection is concave or convex
    double dot;
    PmCartesian diff;
    pmCartCartSub(P,C1,&diff);
    pmCartCartDot(&diff, u2, &dot);
    if (dot > 0) {
        return 0;
    } else {
        return 1;
    }
}

STATIC int lineArcComputeData(LineArcData * const linearc) {
 
    // Check for coplanarity
    // arc-line equations
    PmCartesian n2,binormal;

    pmCartCartCross(&linearc->u1, &linearc->u2, &binormal);
    pmCartUnit(&binormal,&binormal);
    pmCartCartCross(&binormal, &linearc->u2, &n2);

    int convex = tpLineArcConvexTest(&linearc->C1,&linearc->P,&linearc->u2);
    double sgn = 1.0;
    if (convex) {
        sgn=-1.0;
    } 

    //Parallel and perp. components of P-C1
    PmCartesian r_C1P;
    pmCartCartSub(&linearc->P,&linearc->C1, &r_C1P);

    //Project C1 - P onto u2 and n2
    double a, b;
    pmCartCartDot(&r_C1P, &linearc->u2, &a);
    pmCartCartDot(&r_C1P, &n2, &b);

    double d_tol;
    int err = tpFindArcLineDist(a, b, linearc->R1, linearc->tolerance, convex, &d_tol);
    if (err) {
        return err;
    }

    //Find distance bounded by length of line move
    double d_line = fmin(d_tol, linearc->L2 * 0.5);
    tp_debug_print("d_line = %f\n",d_line);

    //Find corresponding radius to d_line
    double R_line = 0;
    err = tpFindRadiusFromDist(a,b,linearc->R1,d_line,convex, &R_line);

    //New upper bound is the lower of the two
    //FIXME hard-code upper bound until we figure out a better way
    double R_bound = 10;
    double R_geom = fmin(R_line, R_bound);
    tp_debug_print("R_geom = %f\n",R_geom);

    //The new radius and line distance is found based on limits of v_req
    // Based on motion segments, compute the maximum velocity we can get based
    //on the requested blend radius and the normal acceleration bounds. Use this
    //to compute the actual upper limit on blend radius.

    //The nominal speed of the blend arc should be the higher of the two segment speeds

    double a_max;
    tpGetMachineAccelLimit(&a_max);
    linearc->a_max = a_max;

    double a_n_max=a_max * TP_ACC_RATIO_NORMAL;

    //Calculate limiting velocity due to radius and normal acceleration
    double v_normal = pmSqrt(a_n_max * R_geom);
    tp_debug_print("v_normal = %f\n",v_normal);

    //If the requested feed is lower than the peak velocity, reduce the arc size to match
    double v_upper = fmin(linearc->v_req, v_normal);

    double R_upper = pmSq(v_upper)/a_n_max;
    linearc->R = R_upper;

    double d_upper=0;
    tpFindDistFromRadius(a,b, linearc->R1, R_upper, convex, &d_upper);
    linearc->d = d_upper;

    tp_debug_print("R_upper = %f, d_upper = %f\n",R_upper,d_upper);

    //Store velocity
    linearc->v_plan = v_upper;

    //Find the blend arc's center
    /*double C = P + d_upper*u2 + R_upper*n2*/
    PmCartesian tmp;
    pmCartScalMult(&linearc->u2, d_upper, &linearc->C);
    pmCartScalMult(&n2, R_upper, &tmp);
    pmCartCartAdd(&linearc->P, &linearc->C, &linearc->C);
    pmCartCartAdd(&tmp, &linearc->C, &linearc->C);

    PmCartesian r_C1C, uc;
    pmCartCartSub(&linearc->C1, &linearc->C, &r_C1C);
    pmCartUnit(&r_C1C, &uc);

    //Calculate blend arc intersections with original segments
    //Q1=C+sgn*uc*R_upper
    pmCartScalMult(&uc, R_upper*sgn, &linearc->Q1);
    pmCartCartAdd(&linearc->Q1, &linearc->C, &linearc->Q1);
    //Q2=P+d_upper*u2
    pmCartScalMult(&linearc->u2, d_upper, &linearc->Q2);
    pmCartCartAdd(&linearc->P, &linearc->Q2, &linearc->Q2);

    //Calculate angle reduction for arc
    PmCartesian up;
    pmCartUnit(&r_C1P,&up);
    
    double dot = 0;
    pmCartCartDot(&up,&uc,&dot);
    //FIXME domain bound
    linearc->dphi1 = acos(dot);
    return TP_ERR_OK;
}


STATIC int tpCreateLineArcBlend(TP_STRUCT * const tp, TC_STRUCT const * const prev_tc, TC_STRUCT const * const tc, TC_STRUCT * const blend_tc)
{

    //TODO bail if there is spiral or helix
    double dot;

    LineArcData linearc;

    linearc.tolerance = tcFindBlendTolerance(prev_tc, tc);

    pmCartCartDot(&tc->coords.circle.xyz.normal, &prev_tc->coords.line.xyz.uVec, &dot);
    if (dot > TP_POS_EPSILON) {
        tp_debug_print("arc and line not coplanar, can't create blend arc\n");
        return TP_ERR_FAIL;
    }
    tcGetEndTangentUnitVector(prev_tc, &linearc.u2);
    tcGetStartTangentUnitVector(tc, &linearc.u1);
    pmCartScalMult(&linearc.u1,-1,&linearc.u1);
    pmCartScalMult(&linearc.u2,-1,&linearc.u2);
    linearc.P = prev_tc->coords.line.xyz.end;
    linearc.C1 = tc->coords.circle.xyz.center;
    linearc.R1= tc->coords.circle.xyz.radius;
    linearc.L2 = prev_tc->target;

    linearc.v_req = fmax(tpGetMaxTargetVel(tp,prev_tc), tpGetMaxTargetVel(tp,tc));

    lineArcComputeData(&linearc);


    //FIXME fail out for now until formulas are right
    return TP_ERR_FAIL;
}


STATIC int tpCreateArcLineBlend(TP_STRUCT * const tp, TC_STRUCT * const prev_tc, TC_STRUCT * const tc, TC_STRUCT * const blend_tc)
{

    //TODO bail if there is spiral or helix
    double dot;
    tp_debug_print("*-*-*-*-*-*-*-*-*-*-\n");

    LineArcData linearc;

    linearc.tolerance = tcFindBlendTolerance(prev_tc, tc);

    pmCartCartDot(&prev_tc->coords.circle.xyz.normal, &tc->coords.line.xyz.uVec, &dot);
    if (dot > TP_POS_EPSILON) {
        tp_debug_print("arc and line not coplanar, can't create blend arc\n");
        return TP_ERR_FAIL;
    }
    //Arc-line
    tcGetEndTangentUnitVector(prev_tc, &linearc.u1);
    tcGetStartTangentUnitVector(tc, &linearc.u2);
    linearc.P = tc->coords.line.xyz.start;
    linearc.C1 = prev_tc->coords.circle.xyz.center;
    linearc.R1= prev_tc->coords.circle.xyz.radius;
    linearc.L2 = tc->target;

    linearc.v_req = fmax(tpGetMaxTargetVel(tp,prev_tc), tpGetMaxTargetVel(tp,tc));

    lineArcComputeData(&linearc);
    //Setup actual velocity
    linearc.v_actual = fmin(fmax(tpGetRealTargetVel(tp,prev_tc),tpGetRealTargetVel(tp,tc)),linearc.v_plan);
    tp_debug_print("v_actual = %f\n",linearc.v_actual);
    tp_debug_print("a_max = %f\n",linearc.a_max);
    tpInitBlendArc(tp, prev_tc, blend_tc, linearc.v_actual, linearc.v_plan, linearc.a_max);

    //TODO shorten arc to Q1
    double new_angle = prev_tc->coords.circle.xyz.angle - linearc.dphi1;
    pmCircleStretch(&prev_tc->coords.circle.xyz,new_angle, 0);
    double new_len = tc->coords.line.xyz.tmag - linearc.d;
    pmCartLineStretch(&tc->coords.line.xyz, new_len,1);
    //TODO shorten line to Q2
    //TODO queue blend arc
    //FIXME fail out for now until formulas are right
    return TP_ERR_FAIL;
}

STATIC int tpCreateArcArcBlend(TP_STRUCT * const tp, TC_STRUCT const * const prev_tc, TC_STRUCT const * const tc, TC_STRUCT * const blend_tc)
{
#warning Not implemented yet
    return TP_ERR_FAIL;
}

/**
 * Compute arc segment to blend between two lines.
 * A workhorse function to calculate all the required parameters for a new
 * blend arc, then create and connect it to existing pair of line segments.
 * This function has grown rather large, but isn't easy to split up due to the
 * many variables that can be reused.
 */
STATIC int tpCreateLineLineBlend(TP_STRUCT * const tp, TC_STRUCT * const prev_tc,
        TC_STRUCT * const tc, TC_STRUCT * const blend_tc) {

    // Assume at this point that we've checked for dumb reasons not to
    // calculate the blend arc, like intersection angle
    // Calculate radius based on tolerances
    double theta = 0.0;
    int res = tpFindIntersectionAngle(&prev_tc->coords.line.xyz.uVec,
            &tc->coords.line.xyz.uVec, &theta);
    if (res) {
        //Can't get an intersection angle, bail
        tp_debug_print("#Failed to find intersection angle!\n");
        return TP_ERR_FAIL;
    }
    tp_debug_print("theta = %f\n",theta);

    double phi = (PM_PI - theta * 2.0);

    PmCartesian binormal;
    pmCartCartCross(&prev_tc->coords.line.xyz.uVec,
            &tc->coords.line.xyz.uVec,
            &binormal);
    pmCartUnitEq(&binormal);
    tp_debug_print("binormal = [%f %f %f]\n", binormal.x,binormal.y,binormal.z);
    double a_max;
    //TODO move this function into setup somewhere because this should be constant
    tpGetPlanarAccelLimit(&binormal, &a_max);
    /*tpGetMachineAccelLimit(&a_max);*/

    double a_n_max = a_max * TP_ACC_RATIO_NORMAL;
    tp_debug_print("a_max = %f, a_n_max = %f\n",a_max, a_n_max);

    //Find common velocity and acceleration
    double v_req = fmin(prev_tc->reqvel, tc->reqvel);
    double v_goal = v_req * emcmotConfig->maxFeedScale;
    double v_max = 0.0;
    tpGetPlanarVelLimit(&binormal,&v_max);
    /*tpGetMachineVelLimit(&a_max);*/
    v_goal = fmin(v_goal,v_max);

    tp_debug_print("vr1 = %f, vr2 = %f\n", prev_tc->reqvel, tc->reqvel);
    tp_debug_print("v_goal = %f, max scale = %f\n", v_goal, emcmotConfig->maxFeedScale);

    //Store trig functions for later use
    double Ctheta = cos(theta);
    double Stheta = sin(theta);
    double Ttheta = tan(theta);

    double greediness = fmin(emcmotConfig->arcBlendGreediness, 1.0);
    //Nominal length restriction prevents gobbling too much of parabolic blends
    double L1 = fmin(prev_tc->target, prev_tc->nominal_length * greediness);
    double L2 = tc->target * greediness;
    tp_debug_print("prev. nominal length = %f, next nominal_length = %f\n",
            prev_tc->nominal_length, tc->nominal_length);

    double min_segment_time = tp->cycleTime * TP_MIN_SEGMENT_CYCLES;

    //TODO get tolerance from blend here 
    double tolerance,nominal_tolerance;
    tcFindBlendTolerance(prev_tc,tc,&tolerance,&nominal_tolerance);
    double h_tol = tolerance / (1.0 - Stheta);
    double d_tol = Ctheta * h_tol;
    // Debug output for tolerances
    tp_debug_print(" d_tol = %f\n",d_tol);

    //Find min length due to segment limits
    double d_lengths = fmin(L1,L2);
    double d_geom = fmin(d_lengths,d_tol); 
    double R_geom = Ttheta * d_geom;

    tp_debug_print("d_tol = %f, d_prev = %f, d_next = %f\n", d_tol, L1, L2);

    double v_normal = pmSqrt(a_n_max * R_geom);
    tp_debug_print("v_normal = %f\n", v_normal);

    double v_plan = v_normal;
    double R_plan = R_geom;

    // If our goal velocity is lower, reduce the arc size proportionally
    if (v_normal > v_goal) {
        v_plan = v_goal;
        tp_debug_print("v_goal = %f\n", v_goal);
        //At this new limiting velocity, find the radius by the reverse formula
        R_plan = pmSq(v_plan) / a_n_max;
    }
    tp_debug_print("R_plan = %f\n", R_plan);
    double d_plan = R_plan / Ttheta;

    tp_debug_print("R_geom = %f\nd_plan = %f\n", R_geom, d_plan);


    tp_debug_print("v_plan = %f\n", v_plan);

    //Now we store the "actual" velocity. Recall that v_plan may be greater
    //than v_req by the max feed override. If our worst-case planned velocity is higher than the requested velocity, then clip at the requested velocity. This allows us to increase speed above the feed override limits
    //Check for segment length limits
#ifdef TP_DEBUG
    double a_n_effective = pmSq(v_plan)/R_plan;

    tp_debug_print("a_n_effective = %f\n",a_n_effective);
#endif

    double L_prev = prev_tc->target - d_plan;
    double prev_seg_time = L_prev / v_plan;
#ifdef TP_DEBUG
    double L_next = tc->target - d_plan;
#endif

    int consume = (prev_seg_time < emcmotConfig->arcBlendGapCycles * min_segment_time);
    double s_arc = phi * R_plan;
    if (consume) {
        s_arc += L_prev;
    }
    //Reduce velocity if necessary 
    double v_sample_arc = s_arc / min_segment_time;
    if (v_plan > v_sample_arc) {
        v_plan = v_sample_arc;
        tp_debug_print("#v_plan %f > v_sample %f for arc\n", v_plan, v_sample_arc);
    }
    tp_debug_print("s_arc = %f, L_prev = %f, L_next = %f, prev_seg_time = %f\n", s_arc, L_prev, L_next, prev_seg_time);

    double v_actual;
    if (v_plan > v_req) {
        v_actual = v_req;
    } else {
        v_actual = v_plan;
    }

    tp_debug_print("v_actual = %f\n", v_actual);

    if (emcmotConfig->arcBlendFallbackEnable) {
        double v_parabolic = 0.0;
        tpComputeBlendVelocity(tp, prev_tc, tc, 1, &v_parabolic);
        //This is the actual velocity at the center of the parabolic blend

        /* Additional quality / performance checks: If we aren't moving faster than
         * the equivalent parabolic blend, then fall back to parabolic
         */

        tp_debug_print("v_plan = %f, v_para = %f\n", v_plan, v_parabolic);
        if ( v_plan <= v_parabolic) {
            return TP_ERR_NO_ACTION;
        }
    } else {
        //If for some reason we get too small a radius, the blend will fail. This
        //shouldn't happen if everything upstream is working.
        if (R_plan < TP_POS_EPSILON) {
            tp_debug_print("#Blend radius too small, aborting...\n");
            return TP_ERR_FAIL;
        }
    }

    PmCartesian circ_start, circ_end;

    double h_plan = R_plan / Stheta;
    arcFromLines(&blend_tc->coords.arc.xyz,
            &prev_tc->coords.line.xyz,
            &tc->coords.line.xyz, R_plan, d_plan, h_plan, &circ_start, &circ_end, consume);
    tp_debug_print("angle = %f\n",blend_tc->coords.arc.xyz.angle);

    tp_debug_print("R_plan = %f, radius_calc = %f\n", R_plan, blend_tc->coords.arc.xyz.radius);

    // Note that previous restrictions don't allow ABC or UVW movement, so the
    // end and start points should be identical
    blend_tc->coords.arc.abc = prev_tc->coords.line.abc.end;
    blend_tc->coords.arc.uvw = prev_tc->coords.line.uvw.end;
    //set the max velocity to v_plan, since we'll violate constraints otherwise.
    tp_debug_print("arc line length = %f\n", blend_tc->coords.arc.xyz.line_length);
    tpInitBlendArc(tp, prev_tc, blend_tc, v_actual, v_plan, a_max);

    int retval = 0;

    if (consume) {
        //Since we're consuming the previous segment, pop the last line off of the queue
        retval = tcqPopBack(&tp->queue);
        tp_debug_print("consume previous line\n");
        if (retval) {
            tp_debug_print("PopBack failed\n");
            return TP_ERR_FAIL;
        }
        //Since the blend arc meets the end of the previous line, we only need
        //to "connect" to the next line
        retval = tcConnectBlendArc(NULL, tc, &circ_start, &circ_end);
    } else {
        tp_debug_print("keeping previous line\n");
        blend_tc->coords.arc.xyz.line_length = 0;
        retval = tcConnectBlendArc(prev_tc, tc, &circ_start, &circ_end);
    }
    return retval;

}


/**
 * Add a newly created motion segment to the tp queue.
 * Returns an error code if the queue operation fails, otherwise adds a new
 * segment to the queue and updates the end point of the trajectory planner.
 */
STATIC inline int tpAddSegmentToQueue(TP_STRUCT * const tp, TC_STRUCT * const tc, int inc_id) {

    tc->id = tp->nextId;
    if (tcqPut(&tp->queue, tc) == -1) {
        rtapi_print_msg(RTAPI_MSG_ERR, "tcqPut failed.\n");
        return TP_ERR_FAIL;
    }
    if (inc_id) {
        tp->nextId++;
    }

    // Store end of current move as new final goal of TP
    tcGetEndpoint(tc, &tp->goalPos);
    tp->done = 0;
    tp->depth = tcqLen(&tp->queue);
    //Fixing issue with duplicate id's?
    tp_debug_print("Adding TC id %d of type %d\n",tc->id,tc->motion_type);

    return TP_ERR_OK;
}


/**
 * Adds a rigid tap cycle to the motion queue.
 */
int tpAddRigidTap(TP_STRUCT * const tp, EmcPose end, double vel, double ini_maxvel,
        double acc, unsigned char enables) {
    TC_STRUCT tc, *prev_tc;
    PmCartLine line_xyz;
    PmCartesian start_xyz, end_xyz;
    PmCartesian abc, uvw;

    if (tpErrorCheck(tp)) return TP_ERR_FAIL;

    //Slightly more allocation this way, but much easier to read
    emcPoseToPmCartesian(&(tp->goalPos), &start_xyz, &abc, &uvw);
    emcPoseGetXYZ(&end, &end_xyz);

    pmCartLineInit(&line_xyz, &start_xyz, &end_xyz);

    tc.coords.rigidtap.reversal_target = line_xyz.tmag;

    // allow 10 turns of the spindle to stop - we don't want to just go on forever
    tc.target = line_xyz.tmag + 10. * tp->uu_per_rev;

    tc.atspeed = 1;

    tc.coords.rigidtap.xyz = line_xyz;
    tc.coords.rigidtap.abc = abc;
    tc.coords.rigidtap.uvw = uvw;
    tc.coords.rigidtap.state = TAPPING;
    tc.motion_type = TC_RIGIDTAP;
    tc.canon_motion_type = 0;
    tc.term_cond = 0;
    tc.tolerance = tp->tolerance;

    if(!tp->synchronized) {
        rtapi_print_msg(RTAPI_MSG_ERR, "Cannot add unsynchronized rigid tap move.\n");
        return TP_ERR_FAIL;
    }
    tc.synchronized = tp->synchronized;

    tc.uu_per_rev = tp->uu_per_rev;
    tc.indexrotary = -1;

    if (tp->syncdio.anychanged != 0) {
        tc.syncdio = tp->syncdio; //enqueue the list of DIOs that need toggling
        tpClearDIOs(tp); // clear out the list, in order to prepare for the next time we need to use it
    } else {
        tc.syncdio.anychanged = 0;
    }

    tpInitializeNewSegment(tp,&tc,vel,ini_maxvel,acc,enables);

    //Assume non-zero error code is failure
    prev_tc = tcqLast(&tp->queue);
    tpFinalizeSegmentLength(tp, prev_tc);
    int retval = tpAddSegmentToQueue(tp, &tc, true);
    tpRunOptimization(tp);
    return retval;
}

STATIC blend_type_t tpCheckBlendArcType(TP_STRUCT const * const tp,
        TC_STRUCT const * const prev_tc,
        TC_STRUCT const * const tc, double period) {
    double omega = 0.0;

    if (!prev_tc || !tc) {
        tp_debug_print("prev_tc or tc doesn't exist\n");
        return BLEND_NONE;
    }

    //If exact stop, we don't compute the arc
    if (prev_tc->term_cond != TC_TERM_COND_PARABOLIC) {
        tp_debug_print("Wrong term cond = %u\n", prev_tc->term_cond);
        return BLEND_NONE;
    }

    //If we have any rotary axis motion, then don't create a blend arc
    if (tpRotaryMotionCheck(tp, tc) || tpRotaryMotionCheck(tp, prev_tc)) {
        tp_debug_print("One of the segments has rotary motion, aborting blend arc\n");
        return BLEND_NONE;
    }

    //If the corner is too tight, a circular arc would have zero radius. Fall
    //back to default blend.
    const double min_angle = TP_MIN_ARC_ANGLE;
    if ((PM_PI - omega) < min_angle ) {
        tp_debug_print("Corner angle omega = %f < min angle %f\n", omega, min_angle);
        return BLEND_NONE;
    }

    if (tc->finalized || prev_tc->finalized) {
        tp_debug_print("Can't create blend when segment lengths are finalized\n");
        return BLEND_NONE;
    }

    tp_debug_print("Motion types: prev_tc = %u, tc = %u\n",
            prev_tc->motion_type,tc->motion_type);
    //If not linear blends, we can't easily compute an arc
    if ((prev_tc->motion_type == TC_LINEAR) && (tc->motion_type == TC_LINEAR)) {
        return BLEND_LINE_LINE;
    } else if (prev_tc->motion_type == TC_LINEAR && tc->motion_type == TC_CIRCULAR) {
        return BLEND_LINE_ARC;
    } else if (prev_tc->motion_type == TC_CIRCULAR && tc->motion_type == TC_LINEAR) {
        return BLEND_ARC_LINE;
    } else if (prev_tc->motion_type == TC_CIRCULAR && tc->motion_type == TC_CIRCULAR) {
        return BLEND_ARC_ARC;
    } else {
        return BLEND_NONE;
    }
}


/**
 * Based on the nth and (n-1)th segment, find a safe final velocity for the (n-1)th segment.
 * This function also caps the target velocity if velocity ramping is enabled. If we
 * don't do this, then the linear segments (with higher tangential
 * acceleration) will speed up and slow down to reach their target velocity,
 * creating "humps" in the velocity profile.
 */
STATIC int tpComputeOptimalVelocity(TP_STRUCT const * const tp, TC_STRUCT * const tc, TC_STRUCT * const prev1_tc) {
    //Calculate the maximum starting velocity vs_back of segment tc, given the
    //trajectory parameters
    double acc_this = tpGetScaledAccel(tp, tc);

    // Find the reachable velocity of tc, moving backwards in time
    double vs_back = pmSqrt(pmSq(tc->finalvel) + 2.0 * acc_this * tc->target);
    // Find the reachable velocity of prev1_tc, moving forwards in time

    double vf_limit_this = tc->maxvel;
    //Limit the PREVIOUS velocity by how much we can overshoot into
    double vf_limit_prev = prev1_tc->maxvel;
    double vf_limit = fmin(vf_limit_this, vf_limit_prev);

    if (vs_back >= vf_limit ) {
        //If we've hit the requested velocity, then prev_tc is definitely a "peak"
        vs_back = vf_limit;
        prev1_tc->optimization_state = TC_OPTIM_AT_MAX;
        tp_debug_print("found peak due to v_limit\n");
    }

    //Limit tc's target velocity to avoid creating "humps" in the velocity profile
    prev1_tc->finalvel = vs_back;

    //Reduce max velocity to match sample rate
    double sample_maxvel = tc->target / (tp->cycleTime * TP_MIN_SEGMENT_CYCLES);
    tc->maxvel = fmin(tc->maxvel, sample_maxvel);

    tp_info_print(" prev1_tc-> fv = %f, tc->fv = %f, capped target = %f\n",
            prev1_tc->finalvel, tc->finalvel, tc->target_vel);

    return TP_ERR_OK;
}


/**
 * Do "rising tide" optimization to find allowable final velocities for each queued segment.
 * Walk along the queue from the back to the front. Based on the "current"
 * segment's final velocity, calculate the previous segment's maximum allowable
 * final velocity. The depth we walk along the queue is controlled by the
 * TP_LOOKAHEAD_DEPTH constant for now. The process safetly aborts early due to
 * a short queue or other conflicts.
 */
STATIC int tpRunOptimization(TP_STRUCT * const tp) {
    // Pointers to the "current", previous, and 2nd previous trajectory
    // components. Current in this context means the segment being optimized,
    // NOT the currently excecuting segment.

    TC_STRUCT *tc;
    TC_STRUCT *prev1_tc;

    int ind, x;
    int len = tcqLen(&tp->queue);
    //TODO make lookahead depth configurable from the INI file

    int hit_peaks = 0;

    /* Starting at the 2nd to last element in the queue, work backwards towards
     * the front. We can't do anything with the very last element because its
     * length may change if a new line is added to the queue.*/

    for (x = 1; x < emcmotConfig->arcBlendOptDepth + 2; ++x) {
        tp_info_print("==== Optimization step %d ====\n",x-2);

        // Update the pointers to the trajectory segments in use
        ind = len-x;
        tc = tcqItem(&tp->queue, ind);
        prev1_tc = tcqItem(&tp->queue, ind-1);

        if ( !prev1_tc || !tc) {
            tp_debug_print(" Reached end of queue in optimization\n");
            return TP_ERR_OK;
        }
        if (!tc->finalized) {
            tp_debug_print("Segment %d, type %d not finalized, continuing\n",tc->id,tc->motion_type);
            continue;
        }

        // stop optimizing if we hit a non-tangent segment (final velocity
        // stays zero)
        if (prev1_tc->term_cond != TC_TERM_COND_TANGENT) {
            tp_debug_print("Found non-tangent segment, stopping optimization\n");
            return TP_ERR_OK;
        }

        //Abort if a segment is already in progress, so that we don't step on
        //split cycle calculation
        if (prev1_tc->progress>0) {
            tp_debug_print("segment %d already started, progress is %f!\n",
                    ind-1, prev1_tc->progress);
            return TP_ERR_OK;
        }

        tp_info_print("  current term = %u, type = %u, id = %u, accel_mode = %d\n",
                tc->term_cond, tc->motion_type, tc->id, tc->accel_mode);
        tp_info_print("  prev term = %u, type = %u, id = %u, accel_mode = %d\n",
                prev1_tc->term_cond, prev1_tc->motion_type, prev1_tc->id, prev1_tc->accel_mode);

        if (tc->atspeed) {
            //Assume worst case that we have a stop at this point. This may cause a
            //slight hiccup, but the alternative is a sudden hard stop.
            tp_debug_print("Found atspeed at id %d\n",tc->id);
            tc->finalvel = 0.0;
        }

        tpComputeOptimalVelocity(tp, tc, prev1_tc);

        tc->active_depth = x - 2 - hit_peaks;
#ifdef TP_OPTIMIZATION_LAZY
        if (tc->optimization_state == TC_OPTIM_AT_MAX) {
            hit_peaks++;
        }
        if (hit_peaks > TP_OPTIMIZATION_CUTOFF) {
            return TP_ERR_OK;
        }
#endif

    }
    tp_debug_print("Reached optimization depth limit\n");
    return TP_ERR_OK;
}


/**
 * Check for tangency between the current segment and previous segment.
 * If the current and previous segment are tangent, then flag the previous
 * segment as tangent, and limit the current segment's velocity by the sampling
 * rate.
 */
STATIC int tpSetupTangent(TP_STRUCT const * const tp,
        TC_STRUCT * const prev_tc, TC_STRUCT * const tc) {
    if (!tc || !prev_tc) {
        tp_debug_print("missing tc or prev tc in tangent check\n");
        return TP_ERR_FAIL;
    }
    //If we have ABCUVW movement, then don't check for tangency
    if (tpRotaryMotionCheck(tp, tc) || tpRotaryMotionCheck(tp, prev_tc)) {
        tp_debug_print("found rotary axis motion, aborting tangent check\n");
        return TP_ERR_NO_ACTION;
    }

    if (emcmotConfig->arcBlendOptDepth < 2) {
        tp_debug_print("Optimization depth %d too low, ignoring any tangents\n",
                emcmotConfig->arcBlendOptDepth);
        return TP_ERR_NO_ACTION;
    }

    PmCartesian prev_tan, this_tan;

    int err1 = tcGetEndTangentUnitVector(prev_tc, &prev_tan);
    int err2 = tcGetStartTangentUnitVector(tc, &this_tan);
#ifdef TP_PEDANTIC
    if (err1 || err2) {
        tp_debug_print("Got %d and %d from tangent vector calc\n",err1,err2);
    }
#endif

    tp_debug_print("prev tangent vector: %f %f %f\n", prev_tan.x, prev_tan.y, prev_tan.z);
    tp_debug_print("this tangent vector: %f %f %f\n", this_tan.x, this_tan.y, this_tan.z);

    double theta;
    int failed = tpFindIntersectionAngle(&prev_tan, &this_tan, &theta);
    if (failed) {
        return TP_ERR_FAIL;
    }

    double phi = PM_PI - 2.0 * theta;
    tp_debug_print("phi = %f\n", phi);

    double v_reachable = fmax(tpGetMaxTargetVel(tp, tc),
            tpGetMaxTargetVel(tp, prev_tc));
    double acc_limit;
    //TODO move this to setup
    tpGetMachineAccelLimit(&acc_limit);

    // FIXME hard-coded max "normal" acceleration for a tangent intersection
    const double TP_ACC_RATIO_TANGENT_NORMAL = TP_ACC_RATIO_NORMAL * 0.2;
    double acc_margin = TP_ACC_RATIO_TANGENT_NORMAL * acc_limit;
    tp_debug_print("acc_margin = %f\n", acc_margin);
    double max_angle = tpMaxTangentAngle(tp, v_reachable, acc_margin);
    double a_n_actual = 2.0 * sin(phi/2.0) * v_reachable / tp->cycleTime;
    double a_t_ratio = 1.0 - a_n_actual / acc_limit;
    tp_debug_print("a_t_ratio = %f\n", a_t_ratio);

    if (phi <= max_angle) {
        tp_debug_print(" New segment tangent with angle %g\n", phi);
        tcSetTermCond(prev_tc, TC_TERM_COND_TANGENT);
        //Calculate actual normal acceleration during tangent transition

        prev_tc->maxaccel *= a_t_ratio;
        tc->maxaccel *= a_t_ratio;

        //Clip maximum velocity by sample rate
        prev_tc->maxvel = fmin(prev_tc->maxvel, prev_tc->target /
                tp->cycleTime / TP_MIN_SEGMENT_CYCLES);
        return TP_ERR_OK;
    } else {
        tp_debug_print(" New segment not tangent, angle %g\n", phi);
        return TP_ERR_NO_ACTION;
    }

}


/**
 * Handle creating a blend arc when a new line segment is about to enter the queue.
 * This function handles the checks, setup, and calculations for creating a new
 * blend arc. Essentially all of the blend arc functions are called through
 * here to isolate the process.
 */
STATIC int tpHandleBlendArc(TP_STRUCT * const tp, TC_STRUCT * const tc) {

    tp_debug_print("********************\nHandle Blend Arc\n");

    TC_STRUCT *prev_tc;
    prev_tc = tcqLast(&tp->queue);

    //If the previous segment has already started, then don't create a blend
    //arc for the next pair.
    // TODO May be able to lift this restriction if we can ensure that we leave
    // 1 timestep's worth of distance in prev_tc
    if ( !prev_tc) {
        tp_debug_print(" queue empty\n");
        return TP_ERR_FAIL;
    }
    if (prev_tc->progress > 0.0) {
        tp_debug_print(" prev_tc progress = %f, aborting arc\n", prev_tc->progress);
        return TP_ERR_FAIL;
    }

    if (TP_ERR_OK == tpSetupTangent(tp, prev_tc, tc)) {
        //Marked segment as tangent
        return TP_ERR_OK;
    }

    TC_STRUCT blend_tc;

    blend_type_t type = tpCheckBlendArcType(tp, prev_tc, tc, tp->cycleTime);
    int res=TP_ERR_FAIL;
    switch (type) { 
        case BLEND_LINE_LINE:
            res = tpCreateLineLineBlend(tp, prev_tc, tc, &blend_tc);
            break;
        case BLEND_LINE_ARC:
            return TP_ERR_FAIL;
            res = tpCreateLineArcBlend(tp, prev_tc, tc, &blend_tc);
            break;
        case BLEND_ARC_LINE:
            res = tpCreateArcLineBlend(tp, prev_tc, tc, &blend_tc);
            break;
        case BLEND_ARC_ARC:
            return TP_ERR_FAIL;
            res = tpCreateArcArcBlend(tp, prev_tc, tc, &blend_tc);
            break;
        default:
            tp_debug_print("blend arc NOT created\n");
            break;
    }

    if (res == TP_ERR_OK) {
        //Need to do this here since the length changed
        tpAddSegmentToQueue(tp, &blend_tc, false);
    } else {
        return res;
    }

    return TP_ERR_OK;
}

/**
 * Add a straight line to the tc queue.
 * end of the previous move to the new end specified here at the
 * currently-active accel and vel settings from the tp struct.
 */
int tpAddLine(TP_STRUCT * const tp, EmcPose end, int type, double vel, double
        ini_maxvel, double acc, unsigned char enables, char atspeed, int indexrotary) {
    TC_STRUCT tc, *prev_tc;
    PmCartLine line_xyz, line_uvw, line_abc;
    PmCartesian start_xyz, end_xyz;
    PmCartesian start_uvw, end_uvw;
    PmCartesian start_abc, end_abc;

    if (tpErrorCheck(tp)<0) {
        return TP_ERR_FAIL;
    }
    tp_info_print("===============\n");

    emcPoseToPmCartesian(&(tp->goalPos), &start_xyz, &start_abc, &start_uvw);
    emcPoseToPmCartesian(&end, &end_xyz, &end_abc, &end_uvw);

    int xyz_fail = pmCartLineInit(&line_xyz, &start_xyz, &end_xyz);
    int abc_fail = pmCartLineInit(&line_abc, &start_abc, &end_abc);
    int uvw_fail = pmCartLineInit(&line_uvw, &start_uvw, &end_uvw);

    if (xyz_fail || abc_fail || uvw_fail) {
        rtapi_print_msg(RTAPI_MSG_ERR,"Failed to initialize Line9, err codes %d, %d, %d\n",
                xyz_fail,abc_fail,uvw_fail);
        return TP_ERR_FAIL;
    }

    if (!line_xyz.tmag_zero) {
        tc.target = line_xyz.tmag;
    } else if (!line_uvw.tmag_zero) {
        tc.target = line_uvw.tmag;
    } else {
        tc.target = line_abc.tmag;
    }

    tc.atspeed = atspeed;

    tc.coords.line.xyz = line_xyz;
    tc.coords.line.uvw = line_uvw;
    tc.coords.line.abc = line_abc;
    tc.motion_type = TC_LINEAR;
    tc.canon_motion_type = type;

    tc.term_cond = tp->termCond;

    tc.tolerance = tp->tolerance;
    tc.synchronized = tp->synchronized;
    tc.uu_per_rev = tp->uu_per_rev;
    tc.indexrotary = indexrotary;

    if (tp->syncdio.anychanged != 0) {
        tc.syncdio = tp->syncdio; //enqueue the list of DIOs that need toggling
        tpClearDIOs(tp); // clear out the list, in order to prepare for the next time we need to use it
    } else {
        tc.syncdio.anychanged = 0;
    }

    tpInitializeNewSegment(tp, &tc, vel, ini_maxvel, acc, enables);

    prev_tc = tcqLast(&tp->queue);
    if (emcmotConfig->arcBlendEnable){
        //TODO add check for two spaces in queue?
        tpHandleBlendArc(tp, &tc);
    }
    tcCheckLastParabolic(&tc, prev_tc);
    tpFinalizeSegmentLength(tp, prev_tc);

    //Flag this as blending with previous segment if the previous segment is
    //set to blend with this one

    int retval = tpAddSegmentToQueue(tp, &tc, true);
    //Run speed optimization (will abort safely if there are no tangent segments)
    tpRunOptimization(tp);

    return retval;
}


STATIC double pmCircleActualMaxVel(PmCircle * const circle, double v_max, double a_max)
{
    double a_n_max = TP_ACC_RATIO_NORMAL * a_max;
    double v_max_acc = pmSqrt(a_n_max * circle->radius);
    if (v_max_acc < v_max) {
        tp_debug_print("Maxvel limited from %f to %f for tangential acceleration\n", v_max, v_max_acc);
        return v_max_acc;
    } else {
        tp_debug_print("v_max %f is within limit of v_max_acc %f\n",v_max, v_max_acc);
        return v_max;
    }
}

/**
 * Adds a circular (circle, arc, helix) move from the end of the
 * last move to this new position.
 *
 * @param end is the xyz/abc point of the destination.
 *
 * see pmCircleInit for further details on how arcs are specified. Note that
 * degenerate arcs/circles are not allowed. We are guaranteed to have a move in
 * xyz so the target is always the circle/arc/helical length.
 */
int tpAddCircle(TP_STRUCT * const tp, EmcPose end,
        PmCartesian center, PmCartesian normal, int turn, int type,
        double vel, double ini_maxvel, double acc, unsigned char enables, char atspeed)
{
    tp_debug_print("[in tpAddCircle]\n");
    TC_STRUCT tc;
    PmCircle circle;
    //TODO replace these placeholder variables with pointers to TC fields
    PmCartLine line_uvw, line_abc;
    PmCartesian start_xyz, end_xyz;
    PmCartesian start_uvw, end_uvw;
    PmCartesian start_abc, end_abc;
    double helix_z_component;   // z of the helix's cylindrical coord system
    double helix_length;

    if (tpErrorCheck(tp)<0) {
        return TP_ERR_FAIL;
    }

    emcPoseToPmCartesian(&(tp->goalPos), &start_xyz, &start_abc, &start_uvw);
    emcPoseToPmCartesian(&end, &end_xyz, &end_abc, &end_uvw);

    int xyz_fail = pmCircleInit(&circle, &start_xyz, &end_xyz, &center, &normal, turn);
    int abc_fail = pmCartLineInit(&line_abc, &start_abc, &end_abc);
    int uvw_fail = pmCartLineInit(&line_uvw, &start_uvw, &end_uvw);

    if (xyz_fail || abc_fail || uvw_fail) {
        rtapi_print_msg(RTAPI_MSG_ERR,"Failed to initialize Circle9, err codes %d, %d, %d\n",
                xyz_fail, abc_fail, uvw_fail);
        return TP_ERR_FAIL;
    }

    // find helix length
    pmCartMag(&circle.rHelix, &helix_z_component);
    helix_length = pmSqrt(pmSq(circle.angle * circle.radius) +
            pmSq(helix_z_component));

    tc.target = helix_length;
    tc.atspeed = atspeed;

    tc.coords.circle.xyz = circle;
    tc.coords.circle.uvw = line_uvw;
    tc.coords.circle.abc = line_abc;
    tc.canon_motion_type = type;
    tc.tolerance = tp->tolerance;

    tc.synchronized = tp->synchronized;
    tc.uu_per_rev = tp->uu_per_rev;
    tc.indexrotary = -1;

    if (tp->syncdio.anychanged != 0) {
        tc.syncdio = tp->syncdio; //enqueue the list of DIOs that need toggling
        tpClearDIOs(tp); // clear out the list, in order to prepare for the next time we need to use it
    } else {
        tc.syncdio.anychanged = 0;
    }

    // Motion parameters
    tc.motion_type = TC_CIRCULAR;
    tc.term_cond = tp->termCond;

    double v_max_actual = pmCircleActualMaxVel(&tc.coords.circle.xyz, ini_maxvel, acc);
    tpInitializeNewSegment(tp, &tc, vel, v_max_actual, acc, enables);

    TC_STRUCT *prev_tc;
    prev_tc = tcqLast(&tp->queue);

    if (emcmotConfig->arcBlendEnable){
        tpHandleBlendArc(tp, &tc);
    }
    tcCheckLastParabolic(&tc, prev_tc);
    tpFinalizeSegmentLength(tp, prev_tc);

    int retval = tpAddSegmentToQueue(tp, &tc, true);

    tpRunOptimization(tp);
    return retval;
}


/**
 * Adjusts blend velocity and acceleration to safe limits.
 * If we are blending between tc and nexttc, then we need to figure out what a
 * safe blend velocity is based on the known trajectory parameters. This
 * function updates the TC_STRUCT data with a safe blend velocity.
 */
STATIC int tpComputeBlendVelocity(TP_STRUCT const * const tp,
        TC_STRUCT * const tc, TC_STRUCT * const nexttc,
        int planning, double * const v_parabolic) {
    /* Pre-checks for valid pointers */
    if (!nexttc || !tc) {

        tp_debug_print("missing nexttc in compute vel?\n");
        return TP_ERR_FAIL;
    }

    if (tc->term_cond != TC_TERM_COND_PARABOLIC && !planning) {
        return TP_ERR_NO_ACTION;
    }

    double acc_this = tpGetScaledAccel(tp, tc);
    double acc_next = tpGetScaledAccel(tp, nexttc);

    // cap the blend velocity at the current requested speed (factoring in feed override)
    double target_vel_this;
    double target_vel_next;
    if (planning) {
        target_vel_this = tpGetMaxTargetVel(tp, tc);
        target_vel_next = tpGetMaxTargetVel(tp, nexttc);
    } else {
        target_vel_this = tpGetRealTargetVel(tp, tc);
        target_vel_next = tpGetRealTargetVel(tp, nexttc);
    }

    double v_reachable_this = fmin(tpCalculateTriangleVel(tp,tc), target_vel_this);
    double v_reachable_next = fmin(tpCalculateTriangleVel(tp,nexttc), target_vel_next);
    /* Scale blend velocity to match blends between current and next segment.
     *
     * The blend time t_b should be the same for this segment and the next
     * segment. This is the time it takes to decelerate from v_blend_this to 0
     * at a rate of acc_this , and accelerate from 0 to v_blend next at a rate
     * of acc_next.
     *
     * t_b = v_blend_this / acc_this = v_blend_next / acc_next
     *
     * Solving for v_blend_this by cross multiplying, we get:
     *
     * v_blend_this = v_blend_next * acc_this / acc_next
     *
     * TODO figure illustrating this
     */

    double v_blend_this, v_blend_next;

    v_blend_this = v_reachable_next * acc_this / acc_next;
    v_blend_next = v_reachable_next;

    //The shorter of the two segments is our constraint
    if (v_reachable_this < v_reachable_next) {
        v_blend_this = fmin(v_reachable_this, v_blend_this);
        v_blend_next = fmin(v_reachable_this * acc_next / acc_this, v_blend_next);
    } else {
        v_blend_this = fmin(v_blend_this, v_reachable_next * acc_this / acc_next);
        v_blend_next = fmin(v_blend_next, v_reachable_next);
    }

    double theta;
    if (tc->tolerance > 0 || planning) {
        /* see diagram blend.fig.  T (blend tolerance) is given, theta
         * is calculated from dot(s1, s2)
         *
         * blend criteria: we are decelerating at the end of segment s1
         * and we pass distance d from the end.
         * find the corresponding velocity v when passing d.
         *
         * in the drawing note d = 2T/cos(theta)
         *
         * when v1 is decelerating at a to stop, v = at, t = v/a
         * so required d = .5 a (v/a)^2
         *
         * equate the two expressions for d and solve for v
         */
        double tblend_vel;
        PmCartesian v1, v2;

        tcGetEndAccelUnitVector(tc, &v1);
        tcGetStartAccelUnitVector(nexttc, &v2);
        tpFindIntersectionAngle(&v1, &v2, &theta);
        /* Minimum value of cos(theta) to prevent numerical instability */
        const double min_cos_theta = cos(PM_PI / 2.0 - TP_MIN_ARC_ANGLE);
        if (cos(theta) > min_cos_theta) {
            tblend_vel = 2.0 * pmSqrt(acc_this * tc->tolerance / cos(theta));
            v_blend_this = fmin(v_blend_this, tblend_vel);
            v_blend_next = fmin(v_blend_next, tblend_vel);
        }

    //Output blend velocity for reference if desired
        if (v_parabolic) {
            //Crude law of cosines

            double vsq = pmSq(v_blend_this) + pmSq(v_blend_next) - 2.0 *
                v_blend_this * v_blend_next * cos(2.0 * theta);
            *v_parabolic = pmSqrt(vsq) / 2.0;
        }
    }
    //Store blend velocities for use during parabolic blending
    if (!planning) {
        tc->blend_vel = v_blend_this;
        nexttc->blend_vel = v_blend_next;
    }
    return TP_ERR_OK;
}


/**
 * Calculate distance update from velocity and acceleration.
 */
STATIC int tcUpdateDistFromAccel(TC_STRUCT * const tc, double acc, double vel_desired)
{
    // If the resulting velocity is less than zero, than we're done. This
    // causes a small overshoot, but in practice it is very small.
    double v_next = tc->currentvel + acc * tc->cycle_time;
    // update position in this tc using trapezoidal integration
    // Note that progress can be greater than the target after this step.
    if (v_next < 0.0) {
        v_next = 0.0;
        //KLUDGE: the trapezoidal planner undershoots by half a cycle time, so
        //forcing the endpoint here is necessary. However, velocity undershoot
        //also occurs during pausing and stopping, which can happen far from
        //the end. If we could "cruise" to the endpoint within a cycle at our
        //current speed, then assume that we want to be at the end.
        if ((tc->target - tc->progress) < (tc->currentvel *  tc->cycle_time)) {
            tc->progress = tc->target;
        }
    } else {
        double displacement = (v_next + tc->currentvel) * 0.5 * tc->cycle_time;
        tc->progress += displacement;
        clip_max(&tc->progress,tc->target);
    }
    tc->currentvel = v_next;

    // Check if we can make the desired velocity
    tc->on_final_decel = (fabs(vel_desired - tc->currentvel) < TP_VEL_EPSILON) && (acc < 0.0);

    return TP_ERR_OK;
}

STATIC void tpDebugCycleInfo(TP_STRUCT const * const tp, TC_STRUCT const * const tc, double acc) {
#ifdef TC_DEBUG
    // Find maximum allowed velocity from feed and machine limits
    double tc_target_vel = tpGetRealTargetVel(tp, tc);
    // Store a copy of final velocity
    double tc_finalvel = tpGetRealFinalVel(tp, tc, tc_target_vel);

    /* Debug Output */
    rtapi_print("tc state: vr = %f, vf = %f, maxvel = %f\n",
            tc_target_vel, tc_finalvel, tc->maxvel);
    rtapi_print("          currentvel = %f, fs = %f, tc = %f, term = %d\n",
            tc->currentvel, tpGetFeedScale(tp,tc), tc->cycle_time, tc->term_cond);
    rtapi_print("          acc = %f,T = %f, P = %f\n", acc,
            tc->target, tc->progress);

    if (tc->on_final_decel) {
        rtapi_print(" on final decel\n");
    }
#endif
}

/**
 * Compute updated position and velocity for a timestep based on a trapezoidal
 * motion profile.
 * @param tc trajectory segment being processed.
 *
 * Creates the trapezoidal velocity profile based on the segment's velocity and
 * acceleration limits. The formula has been tweaked slightly to allow a
 * non-zero velocity at the instant the target is reached.
 */
void tpCalculateTrapezoidalAccel(TP_STRUCT const * const tp, TC_STRUCT * const tc,
        double * const acc, double * const vel_desired)
{
    tc_debug_print("using trapezoidal acceleration\n");

    // Find maximum allowed velocity from feed and machine limits
    double tc_target_vel = tpGetRealTargetVel(tp, tc);
    // Store a copy of final velocity
    double tc_finalvel = tpGetRealFinalVel(tp, tc, tc_target_vel);

#ifdef TP_PEDANTIC
    if (tc_finalvel > 0.0 && tc->term_cond != TC_TERM_COND_TANGENT) {
        rtapi_print_msg(RTAPI_MSG_ERR, "Final velocity of %f with non-tangent segment!\n",tc_finalvel);
        tc_finalvel = 0.0;
    }
#endif

    /* Calculations for desired velocity based on trapezoidal profile */
    double dx = tc->target - tc->progress;
    double maxaccel = tpGetScaledAccel(tp, tc);

    double discr_term1 = pmSq(tc_finalvel);
    double discr_term2 = maxaccel * (2.0 * dx - tc->currentvel * tc->cycle_time);
    double tmp_adt = maxaccel * tc->cycle_time * 0.5;
    double discr_term3 = pmSq(tmp_adt);

    double discr = discr_term1 + discr_term2 + discr_term3;

    // Descriminant is a little more complicated with final velocity term. If
    // descriminant < 0, we've overshot (or are about to). Do the best we can
    // in this situation
#ifdef TP_PEDANTIC
    if (discr < 0.0) {
        rtapi_print_msg(RTAPI_MSG_ERR,
                "discriminant %f < 0 in velocity calculation!\n", discr);
    }
#endif
    //Start with -B/2 portion of quadratic formula
    double maxnewvel = -tmp_adt;

    //If the discriminant term brings our velocity above zero, add it to the total
    //We can ignore the calculation otherwise because negative velocities are clipped to zero
    if (discr > discr_term3) {
        maxnewvel += pmSqrt(discr);
    }

    // Find bounded new velocity based on target velocity
    // Note that we use a separate variable later to check if we're on final decel
    double newvel = saturate(maxnewvel, tc_target_vel);

    // If we have cartesian motion that's not synched with spindle position,
    // then clamp the tool tip velocity at the limit specified in the INI file.
    if (!tpPureRotaryCheck(tp, tc) && (tc->synchronized != TC_SYNC_POSITION)){
        sat_inplace(&newvel, tp->vLimit);
    }

    // Calculate acceleration needed to reach newvel, bounded by machine maximum
    double maxnewaccel = (newvel - tc->currentvel) / tc->cycle_time;
    *acc = saturate(maxnewaccel, maxaccel);
    *vel_desired = maxnewvel;
}

/**
 * Calculate "ramp" acceleration for a cycle.
 */
STATIC int tpCalculateRampAccel(TP_STRUCT const * const tp,
        TC_STRUCT * const tc, double * const acc, double * const vel_desired)
{
    tc_debug_print("using ramped acceleration\n");
    //Initial guess at dt for next round
    double dx = tc->target - tc->progress;

    if (!tc->blending_next) {
        tc->vel_at_blend_start = tc->currentvel;
    }

    double target_vel = tpGetRealTargetVel(tp, tc);
    double vel_final = tpGetRealFinalVel(tp, tc, target_vel);

    /* Check if the final velocity is too low to properly ramp up.
     */
    if (vel_final < TP_VEL_EPSILON) {
        tp_debug_print(" vel_final %f too low for velocity ramping\n", vel_final);
        return TP_ERR_FAIL;
    }

    double vel_avg = (tc->currentvel + vel_final) / 2.0;

    // Calculate time remaining in this segment assuming constant acceleration
    double dt = 1e-16;
    if (vel_avg > TP_VEL_EPSILON) {
        dt = fmax( dx / vel_avg, 1e-16);
    }

    // Calculate velocity change between final and current velocity
    double dv = vel_final - tc->currentvel;

    // Estimate constant acceleration required
    double acc_final = dv / dt;

    // Saturate estimated acceleration against maximum allowed by segment
    double acc_max = tpGetScaledAccel(tp, tc);

    // Output acceleration and velocity for position update
    *acc = saturate(acc_final, acc_max);
    *vel_desired = vel_final;

    return TP_ERR_OK;
}

void tpToggleDIOs(TC_STRUCT * const tc) {

    int i=0;
    if (tc->syncdio.anychanged != 0) { // we have DIO's to turn on or off
        for (i=0; i < num_dio; i++) {
            if (!(tc->syncdio.dio_mask & (1 << i))) continue;
            if (tc->syncdio.dios[i] > 0) emcmotDioWrite(i, 1); // turn DIO[i] on
            if (tc->syncdio.dios[i] < 0) emcmotDioWrite(i, 0); // turn DIO[i] off
        }
        for (i=0; i < num_aio; i++) {
            if (!(tc->syncdio.aio_mask & (1 << i))) continue;
            emcmotAioWrite(i, tc->syncdio.aios[i]); // set AIO[i]
        }
        tc->syncdio.anychanged = 0; //we have turned them all on/off, nothing else to do for this TC the next time
    }
}


/**
 * Handle special cases for rigid tapping.
 * This function deals with updating the goal position and spindle position
 * during a rigid tap cycle. In particular, the target and spindle goal need to
 * be carefully handled since we're reversing direction.
 */
STATIC void tpUpdateRigidTapState(TP_STRUCT const * const tp,
        TC_STRUCT * const tc) {

    static double old_spindlepos;
    double new_spindlepos = emcmotStatus->spindleRevs;
    if (emcmotStatus->spindle.direction < 0) new_spindlepos = -new_spindlepos;

    switch (tc->coords.rigidtap.state) {
        case TAPPING:
            rtapi_print_msg(RTAPI_MSG_DBG, "TAPPING");
            if (tc->progress >= tc->coords.rigidtap.reversal_target) {
                // command reversal
                emcmotStatus->spindle.speed *= -1.0;
                tc->coords.rigidtap.state = REVERSING;
            }
            break;
        case REVERSING:
            rtapi_print_msg(RTAPI_MSG_DBG, "REVERSING");
            if (new_spindlepos < old_spindlepos) {
                PmCartesian start, end;
                PmCartLine *aux = &tc->coords.rigidtap.aux_xyz;
                // we've stopped, so set a new target at the original position
                tc->coords.rigidtap.spindlerevs_at_reversal = new_spindlepos + tp->spindle.offset;

                pmCartLinePoint(&tc->coords.rigidtap.xyz, tc->progress, &start);
                end = tc->coords.rigidtap.xyz.start;
                pmCartLineInit(aux, &start, &end);
                rtapi_print_msg(RTAPI_MSG_DBG, "old target = %f", tc->target);
                tc->coords.rigidtap.reversal_target = aux->tmag;
                tc->target = aux->tmag + 10. * tc->uu_per_rev;
                tc->progress = 0.0;
                rtapi_print_msg(RTAPI_MSG_DBG, "new target = %f", tc->target);

                tc->coords.rigidtap.state = RETRACTION;
            }
            old_spindlepos = new_spindlepos;
            rtapi_print_msg(RTAPI_MSG_DBG, "Spindlepos = %f", new_spindlepos);
            break;
        case RETRACTION:
            rtapi_print_msg(RTAPI_MSG_DBG, "RETRACTION");
            if (tc->progress >= tc->coords.rigidtap.reversal_target) {
                emcmotStatus->spindle.speed *= -1;
                tc->coords.rigidtap.state = FINAL_REVERSAL;
            }
            break;
        case FINAL_REVERSAL:
            rtapi_print_msg(RTAPI_MSG_DBG, "FINAL_REVERSAL");
            if (new_spindlepos > old_spindlepos) {
                PmCartesian start, end;
                PmCartLine *aux = &tc->coords.rigidtap.aux_xyz;
                pmCartLinePoint(aux, tc->progress, &start);
                end = tc->coords.rigidtap.xyz.start;
                pmCartLineInit(aux, &start, &end);
                tc->target = aux->tmag;
                tc->progress = 0.0;
                //No longer need spindle sync at this point
                tc->synchronized = 0;
                tc->target_vel = tc->maxvel;

                tc->coords.rigidtap.state = FINAL_PLACEMENT;
            }
            old_spindlepos = new_spindlepos;
            break;
        case FINAL_PLACEMENT:
            rtapi_print_msg(RTAPI_MSG_DBG, "FINAL_PLACEMENT\n");
            // this is a regular move now, it'll stop at target above.
            break;
    }
}


/**
 * Update emcMotStatus with information about trajectory motion.
 * Based on the specified trajectory segment tc, read its progress and status
 * flags. Then, update the emcmotStatus structure with this information.
 */
STATIC void tpUpdateMovementStatus(TP_STRUCT * const tp, TC_STRUCT const * const tc ) {
    EmcPose target;
    tcGetEndpoint(tc, &target);

    tc_debug_print("tc id = %u canon_type = %u mot type = %u\n",
            tc->id, tc->canon_motion_type, tc->motion_type);
    tp->motionType = tc->canon_motion_type;
    tp->activeDepth = tc->active_depth;
    emcmotStatus->distance_to_go = tc->target - tc->progress;
    emcmotStatus->enables_queued = tc->enables;
    // report our line number to the guis
    tp->execId = tc->id;
    emcmotStatus->requested_vel = tc->reqvel;
    emcmotStatus->current_vel = tc->currentvel;

    emcPoseSub(&target, &tp->currentPos, &emcmotStatus->dtg);
}


/**
 * Do a parabolic blend by updating the nexttc.
 * Perform the actual blending process by updating the target velocity for the
 * next segment, then running a cycle update.
 */
STATIC void tpUpdateBlend(TP_STRUCT * const tp, TC_STRUCT * const tc,
        TC_STRUCT * const nexttc) {

    double save_vel = nexttc->target_vel;

    if (tpGetFeedScale(tp,nexttc) > TP_VEL_EPSILON) {
        double dv = tc->vel_at_blend_start - tc->currentvel;
        //TODO check for divide by zero
        double blend_progress = fmin(dv / tc->vel_at_blend_start, 1.0);
        double blend_scale = tc->vel_at_blend_start / tc->blend_vel;
        nexttc->target_vel = blend_progress * nexttc->blend_vel * blend_scale;
    } else {
        nexttc->target_vel = 0.0;
    }

    tpUpdateCycle(tp, nexttc);
    //Restore the blend velocity
    nexttc->target_vel = save_vel;

}


/**
 * Cleanup if tc is not valid (empty queue).
 * If the program ends, or we hit QUEUE STARVATION, do a soft reset on the trajectory planner.
 * TODO merge with tpClear?
 */
STATIC void tpHandleEmptyQueue(TP_STRUCT * const tp,
        emcmot_status_t * const emcmotStatus) {

    tcqInit(&tp->queue);
    tp->goalPos = tp->currentPos;
    tp->done = 1;
    tp->depth = tp->activeDepth = 0;
    tp->aborting = 0;
    tp->execId = 0;
    tp->motionType = 0;
    tpResume(tp);
    // when not executing a move, use the current enable flags
    emcmotStatus->enables_queued = emcmotStatus->enables_new;
}

/** Wrapper function to unlock rotary axes */
STATIC void tpSetRotaryUnlock(int axis, int unlock) {
    emcmotSetRotaryUnlock(axis, unlock);
}

/** Wrapper function to check rotary axis lock */
STATIC int tpGetRotaryIsUnlocked(int axis) {
    return emcmotGetRotaryIsUnlocked(axis);
}


/**
 * Cleanup after a trajectory segment is complete.
 * If the current move is complete and we're not waiting on the spindle for
 * const this move, then pop if off the queue and perform cleanup operations.
 * Finally, get the next move in the queue.
 */
STATIC int tpCompleteSegment(TP_STRUCT * const tp,
        TC_STRUCT const * const tc) {

    if (tp->spindle.waiting_for_atspeed == tc->id) {
        return TP_ERR_FAIL;
    }

    // if we're synced, and this move is ending, save the
    // spindle position so the next synced move can be in
    // the right place.
    if(tc->synchronized != TC_SYNC_NONE) {
        tp->spindle.offset += tc->target / tc->uu_per_rev;
    } else {
        tp->spindle.offset = 0.0;
    }

    if(tc->indexrotary != -1) {
        // this was an indexing move, so before we remove it we must
        // relock the axis
        tpSetRotaryUnlock(tc->indexrotary, 0);
        // if it is now locked, fall through and remove the finished move.
        // otherwise, just come back later and check again
        if(tpGetRotaryIsUnlocked(tc->indexrotary)) {
            return TP_ERR_FAIL;
        }
    }

    // done with this move
    tcqRemove(&tp->queue, 1);
    tp_debug_print("Finished tc id %d\n", tc->id);

    return TP_ERR_OK;
}


/**
 * Handle an abort command.
 * Based on the current motion state, handle the consequences of an abort command.
 */
STATIC int tpHandleAbort(TP_STRUCT * const tp, TC_STRUCT * const tc,
        TC_STRUCT * const nexttc) {

    if(!tp->aborting) {
        //Don't need to do anything if not aborting
        return TP_ERR_NO_ACTION;
    }
    //If the motion has stopped, then it's safe to reset the TP struct.
    if( MOTION_ID_VALID(tp->spindle.waiting_for_index) ||
            MOTION_ID_VALID(tp->spindle.waiting_for_atspeed) ||
            (tc->currentvel == 0.0 && (!nexttc || nexttc->currentvel == 0.0))) {
        tcqInit(&tp->queue);
        tp->goalPos = tp->currentPos;
        tp->done = 1;
        tp->depth = tp->activeDepth = 0;
        tp->aborting = 0;
        tp->execId = 0;
        tp->motionType = 0;
        tp->synchronized = 0;
        tp->spindle.waiting_for_index = MOTION_INVALID_ID;
        tp->spindle.waiting_for_atspeed = MOTION_INVALID_ID;
        emcmotStatus->spindleSync = 0;
        tpResume(tp);
        return TP_ERR_STOPPED;
    }  //FIXME consistent error codes
    return TP_ERR_SLOWING;
}


/**
 * Check if the segment waiting for an index has changed.
 * If the current segment waiting for an index is not the current segment, then
 * something has gone wrong. The fix for now is to just update status so we're
 * waiting in the current segment instead. (Rob's understanding)
 */
STATIC int tpHandleWaiting(TP_STRUCT * const tp, TC_STRUCT * const tc) {

    // this is no longer the segment we were waiting_for_index for
    if (MOTION_ID_VALID(tp->spindle.waiting_for_index) && tp->spindle.waiting_for_index != tc->id)
    {
        rtapi_print_msg(RTAPI_MSG_ERR,
                "Was waiting for index on motion id %d, but reached id %d\n",
                tp->spindle.waiting_for_index, tc->id);
        tp->spindle.waiting_for_index = MOTION_INVALID_ID;
    }

    if (MOTION_ID_VALID(tp->spindle.waiting_for_atspeed) && tp->spindle.waiting_for_atspeed != tc->id)
    {

        rtapi_print_msg(RTAPI_MSG_ERR,
                "Was waiting for atspeed on motion id %d, but reached id %d\n",
                tp->spindle.waiting_for_atspeed, tc->id);
        tp->spindle.waiting_for_atspeed = MOTION_INVALID_ID;
    }

    if (MOTION_ID_VALID(tp->spindle.waiting_for_atspeed)) {
        if(!emcmotStatus->spindle_is_atspeed) {
            // spindle is still not at the right speed, so wait another cycle
            return TP_ERR_WAITING;
        } else {
            tp->spindle.waiting_for_atspeed = MOTION_INVALID_ID;
        }
    }

    if (MOTION_ID_VALID(tp->spindle.waiting_for_index)) {
        if (emcmotStatus->spindle_index_enable) {
            /* haven't passed index yet */
            return TP_ERR_WAITING;
        } else {
            /* passed index, start the move */
            emcmotStatus->spindleSync = 1;
            tp->spindle.waiting_for_index = MOTION_INVALID_ID;
            tc->sync_accel = 1;
            tp->spindle.revs = 0;
        }
    }

    return TP_ERR_OK;
}

/**
 * Finalize the length of a segment and re-run optimization.
 * This function is a kludgy fix for the problem of finalizing the very last
 * segment in a program. Since the last segment is never blending with a "next"
 * segment, it's never marked as finalized. 
 *
 * @param tp trajectory planner struct pointer
 * @param tc segment to check for finalized length
 *
 * Usage: call this function on a near-future segment in tpRunCycle (at least 2
 * segments ahead of the "current" segment). If we detect that tc is not
 * finalized, then force it to be finalized and re-run optimization. 
 *
 * If this isn't actually the end (say we have queue starvation), the blend arc
 * functions will detect that the prev. line is finalized and skip that blend
 * arc.
 */
STATIC int tpForceFinalizeSegment(TP_STRUCT * const tp,
        TC_STRUCT * const tc) {

    if (!tc) {
        return TP_ERR_NO_ACTION;
    }

    if(tc->finalized == 0) {
        //The next segment is not finalized, but if we've reached it, it means it won't change.
        tc->finalized = 1;
        tpRunOptimization(tp);
        return TP_ERR_OK;
    } else {
        return TP_ERR_NO_ACTION;
    }

}

/**
 * Check for early stop conditions.
 * If a variety of conditions are true, then we can't do blending as we expect.
 * This function checks for any conditions that force us to stop on the current
 * segment. This is different from pausing or aborting, which can happen any
 * time.
 */
STATIC int tpFlagEarlyStop(TP_STRUCT * const tp,
        TC_STRUCT * const tc, TC_STRUCT * const nexttc) {

    if (!tc || !nexttc) {
        return TP_ERR_NO_ACTION;
    }

    if(tc->synchronized != TC_SYNC_POSITION && nexttc->synchronized == TC_SYNC_POSITION) {
        // we'll have to wait for spindle sync; might as well
        // stop at the right place (don't blend)
        tc_debug_print("waiting on spindle sync for tc %d\n", tc->id);
        tcSetTermCond(tc, TC_TERM_COND_STOP);
    }

    if(nexttc->atspeed) {
        // we'll have to wait for the spindle to be at-speed; might as well
        // stop at the right place (don't blend), like above
        // FIXME change the values so that 0 is exact stop mode
        tc_debug_print("waiting on spindle atspeed for tc %d\n", tc->id);
        tcSetTermCond(tc, TC_TERM_COND_STOP);
    }

    return TP_ERR_OK;
}


/**
 * "Activate" a segment being read for the first time.
 * This function handles initial setup of a new segment read off of the queue
 * for the first time.
 */
STATIC int tpActivateSegment(TP_STRUCT * const tp, TC_STRUCT * const tc) {

    //Check if already active
    if (!tc || tc->active) {
        return TP_ERR_OK;
    }

    // Test if we need ramping or trapezoidal acceleration for this move
    // FIXME: move this to INI setting
    const double cutoff_freq = 50.0; //Hz
    double cutoff_time = 1.0 / (cutoff_freq);

    double length = (tc->target-tc->progress);
    double segment_time = 2.0 * length / (tc->currentvel + tc->finalvel);
    /*double segment_cycles = segment_time / tp->cycle_time;*/

    if (segment_time < cutoff_time &&
            tc->canon_motion_type != EMC_MOTION_TYPE_TRAVERSE &&
            tc->term_cond == TC_TERM_COND_TANGENT)
    {
        tp_debug_print("segment_time = %f, cutoff_time = %f, ramping\n",
                segment_time, cutoff_time);
        tc->accel_mode = TC_ACCEL_RAMP;
    }

    // Do at speed checks
    int needs_atspeed = tc->atspeed ||
        (tc->synchronized == TC_SYNC_POSITION && !(emcmotStatus->spindleSync));

    if ( needs_atspeed && !(emcmotStatus->spindle_is_atspeed)) {
        tp->spindle.waiting_for_atspeed = tc->id;
        return TP_ERR_WAITING;
    }

    if (tc->indexrotary != -1) {
        // request that the axis unlock
        tpSetRotaryUnlock(tc->indexrotary, 1);
        // if it is unlocked, fall through and start the move.
        // otherwise, just come back later and check again
        if (!tpGetRotaryIsUnlocked(tc->indexrotary))
            return TP_ERR_WAITING;
    }

    // Temporary debug message
    tp_debug_print("Activate tc id = %d target_vel = %f final_vel = %f length = %f\n",
            tc->id, tc->target_vel,tc->finalvel,tc->target);

    tc->active = 1;
    //Do not change initial velocity here, since tangent blending already sets this up
    tp->motionType = tc->canon_motion_type;
    tc->blending_next = 0;
    tc->on_final_decel = 0;

    if (TC_SYNC_POSITION == tc->synchronized && !(emcmotStatus->spindleSync)) {
        tp_debug_print("Setting up position sync\n");
        // if we aren't already synced, wait
        tp->spindle.waiting_for_index = tc->id;
        // ask for an index reset
        emcmotStatus->spindle_index_enable = 1;
        tp->spindle.offset = 0.0;
        rtapi_print_msg(RTAPI_MSG_DBG, "Waiting on sync...\n");
        return TP_ERR_WAITING;
    }

    return TP_ERR_OK;
}


/**
 * Run velocity mode synchronization.
 * Update requested velocity to follow the spindle's velocity (scaled by feed rate).
 */
STATIC void tpSyncVelocityMode(TP_STRUCT * const tp, TC_STRUCT * const tc, TC_STRUCT const * nexttc) {
    //NOTE: check for aborting outside of here
    double speed = emcmotStatus->spindleSpeedIn;
    double pos_error = fabs(speed) * tc->uu_per_rev;
    //Take into account blending_next?
    if(nexttc) pos_error -= nexttc->progress; /* ?? */
    tc->target_vel = pos_error;
}


/**
 * Run position mode synchronization.
 * Updates requested velocity for a trajectory segment to track the spindle's position.
 */
STATIC void tpSyncPositionMode(TP_STRUCT * const tp, TC_STRUCT * const tc,
        TC_STRUCT * const nexttc ) {

    double spindle_pos = tpGetSignedSpindlePosition(emcmotStatus->spindleRevs,
            emcmotStatus->spindle.direction);
    tp_debug_print("Spindle at %f\n",spindle_pos);
    double spindle_vel, target_vel;
    double oldrevs = tp->spindle.revs;

    if ((tc->motion_type == TC_RIGIDTAP) && (tc->coords.rigidtap.state == RETRACTION ||
                tc->coords.rigidtap.state == FINAL_REVERSAL)) {
            tp->spindle.revs = tc->coords.rigidtap.spindlerevs_at_reversal -
                spindle_pos;
    } else {
        tp->spindle.revs = spindle_pos;
    }

    double pos_desired = (tp->spindle.revs - tp->spindle.offset) * tc->uu_per_rev;
    double pos_error = pos_desired - tc->progress;

    if(nexttc) {
        pos_error -= nexttc->progress;
    }

    if(tc->sync_accel) {
        // detect when velocities match, and move the target accordingly.
        // acceleration will abruptly stop and we will be on our new target.
        spindle_vel = tp->spindle.revs / (tc->cycle_time * tc->sync_accel++);
        target_vel = spindle_vel * tc->uu_per_rev;
        if(tc->currentvel >= target_vel) {
            tc_debug_print("Hit accel target in pos sync\n");
            // move target so as to drive pos_error to 0 next cycle
            tp->spindle.offset = tp->spindle.revs - tc->progress / tc->uu_per_rev;
            tc->sync_accel = 0;
            tc->target_vel = target_vel;
        } else {
            tc_debug_print("accelerating in pos_sync\n");
            // beginning of move and we are behind: accel as fast as we can
            tc->target_vel = tc->maxvel;
        }
    } else {
        // we have synced the beginning of the move as best we can -
        // track position (minimize pos_error).
        tc_debug_print("tracking in pos_sync\n");
        double errorvel;
        spindle_vel = (tp->spindle.revs - oldrevs) / tp->cycleTime;
        target_vel = spindle_vel * tc->uu_per_rev;
        errorvel = pmSqrt(fabs(pos_error) * tpGetScaledAccel(tp,tc));
        if(pos_error<0) {
            errorvel *= -1.0;
        }
        tc->target_vel = target_vel + errorvel;
    }

    //Finally, clip requested velocity at zero
    if (tc->target_vel < 0.0) {
        tc->target_vel = 0.0;
    }

    if (nexttc && nexttc->synchronized) {
        //If the next move is synchronized too, then match it's
        //requested velocity to the current move
        nexttc->target_vel = tc->target_vel;
    }
}


/**
 * Perform parabolic blending if needed between segments and handle status updates.
 * This isolates most of the parabolic blend stuff to make the code path
 * between tangent and parabolic blends easier to follow.
 */
STATIC int tpDoParabolicBlending(TP_STRUCT * const tp, TC_STRUCT * const tc,
        TC_STRUCT * const nexttc) {

    tc_debug_print("in DoParabolicBlend\n");
    tpUpdateBlend(tp,tc,nexttc);

    /* Status updates */
    //Decide which segment we're in depending on which is moving faster
    if(tc->currentvel > nexttc->currentvel) {
        tpUpdateMovementStatus(tp, tc);
    } else {
        tpToggleDIOs(nexttc);
        tpUpdateMovementStatus(tp, nexttc);
    }
#ifdef TP_SHOW_BLENDS
    // hack to show blends in axis
    tp->motionType = 0;
#endif

    //Update velocity status based on both tc and nexttc
    emcmotStatus->current_vel = tc->currentvel + nexttc->currentvel;

    return TP_ERR_OK;
}


/**
 * Do a complete update on one segment.
 * Handles the majority of updates on a single segment for the current cycle.
 */
STATIC int tpUpdateCycle(TP_STRUCT * const tp,
        TC_STRUCT * const tc) {

    //placeholders for position for this update
    EmcPose before;

    //Store the current position due to this TC
    tcGetPos(tc, &before);

    // Update the start velocity if we're not blending yet
    if (!tc->blending_next) {
        tc->vel_at_blend_start = tc->currentvel;
    }

    // Run cycle update with stored cycle time
    int res = 1;
    double acc, vel_desired;

    // If the slowdown is not too great, use velocity ramping instead of trapezoidal velocity
    // Also, don't ramp up for parabolic blends
    if (tc->accel_mode && tc->term_cond == TC_TERM_COND_TANGENT) {
        res = tpCalculateRampAccel(tp, tc, &acc, &vel_desired);
    }

    // Check the return in case the ramp calculation failed, fall back to trapezoidal
    if (res != TP_ERR_OK) {
        tpCalculateTrapezoidalAccel(tp, tc, &acc, &vel_desired);
    }

    tcUpdateDistFromAccel(tc, acc, vel_desired);
    tpDebugCycleInfo(tp, tc, acc);

    //Check if we're near the end of the cycle and set appropriate changes
    tpCheckEndCondition(tp, tc);

    EmcPose displacement;

    //Calculate displacement
    tcGetPos(tc, &displacement);
    emcPoseSelfSub(&displacement, &before);

    emcPoseSelfAdd(&tp->currentPos, &displacement);

#ifdef TC_DEBUG
    double mag;
    emcPoseMagnitude(&displacement, &mag);
    tc_debug_print("cycle movement = %f\n",mag);
#endif

    return TP_ERR_OK;
}


/**
 * Send default values to status structure.
 */
STATIC int tpUpdateInitialStatus(TP_STRUCT const * const tp) {
    // Update queue length
    emcmotStatus->tcqlen = tcqLen(&tp->queue);
    // Set default value for requested speed
    emcmotStatus->requested_vel = 0.0;
    return TP_ERR_OK;
}


/**
 * Flag a segment as needing a split cycle.
 * In addition to flagging a segment as splitting, do any preparations to store
 * data for the next cycle.
 */
STATIC inline int tcSetSplitCycle(TC_STRUCT * const tc, double split_time,
        double v_f)
{
    tp_debug_print("split time for id %d is %f\n", tc->id, split_time);
    tc->splitting = 1;
    tc->cycle_time = split_time;
    tc->vel_at_blend_start = v_f;
    return 0;
}


/**
 * Check remaining time in a segment and calculate split cycle if necessary.
 * This function estimates how much time we need to complete the next segment.
 * If it's greater than one timestep, then we do nothing and carry on. If not,
 * then we flag the segment as "splitting", so that during the next cycle,
 * it handles the transition to the next segment.
 */
STATIC int tpCheckEndCondition(TP_STRUCT const * const tp, TC_STRUCT * const tc) {

    //Assume no split time unless we find otherwise
    tc->cycle_time = tp->cycleTime;

    //Initial guess at dt for next round
    double dx = tc->target - tc->progress;
    tc_debug_print("tpCheckEndCondition: dx = %e\n",dx);

    if (dx <= TP_POS_EPSILON) {
        //If the segment is close to the target position, then we assume that it's done.
        tp_debug_print("close to target, dx = %.12f\n",dx);
        //Force progress to land exactly on the target to prevent numerical errors.
        tc->progress = tc->target;
        tcSetSplitCycle(tc, 0.0, tc->currentvel);
        if (tc->term_cond != TC_TERM_COND_TANGENT) {
            //Non-tangent segments don't need a split cycle, so flag removal here
            tc->remove = 1;
        }
        return TP_ERR_OK;
    } else if (tc->term_cond != TC_TERM_COND_TANGENT) {
        // Abort check here since split cycles are not handled for
        // non-tangent segments.
        return TP_ERR_NO_ACTION;
    }

    tp_debug_print("in tpCheckEndCondition\n");

    double target_vel = tpGetRealTargetVel(tp, tc);
    double v_f = tpGetRealFinalVel(tp, tc, target_vel);
    double v_avg = (tc->currentvel + v_f) / 2.0;

    //Check that we have a non-zero "average" velocity between now and the
    //finish. If not, it means that we have to accelerate from a stop, which
    //will take longer than the minimum 2 timesteps that each segment takes, so
    //we're safely far form the end.

    //Get dt assuming that we can magically reach the final velocity at
    //the end of the move.
    //
    //KLUDGE: start with a value below the cutoff
    double dt = TP_TIME_EPSILON / 2.0;
    if (v_avg > TP_VEL_EPSILON) {
        //Get dt from distance and velocity (avoid div by zero)
        dt = fmax(dt, dx / v_avg);
    } else {
        if ( dx > (v_avg * tp->cycleTime) && dx > TP_POS_EPSILON) {
            tp_debug_print(" below velocity threshold, assuming far from end\n");
            return TP_ERR_NO_ACTION;
        }
    }

    //Calculate the acceleration this would take:

    double dv = v_f - tc->currentvel;
    double a_f = dv / dt;

    //If this is a valid acceleration, then we're done. If not, then we solve
    //for v_f and dt given the max acceleration allowed.
    double a_max = tpGetScaledAccel(tp,tc);

    //If we exceed the maximum acceleration, then the dt estimate is too small.
    double a = a_f;
    int recalc = sat_inplace(&a, a_max);

    //Need to recalculate vf and above
    if (recalc) {
        tp_debug_print(" recalculating with a_f = %f, a = %f\n", a_f, a);
        double disc = pmSq(tc->currentvel / a) + 2.0 / a * dx;
        if (disc < 0 ) {
            //Should mean that dx is too big, i.e. we're not close enough
            tp_debug_print(" dx = %f, too large, not at end yet\n",dx);
            return TP_ERR_NO_ACTION;
        }

        if (disc < TP_TIME_EPSILON * TP_TIME_EPSILON) {
            tp_debug_print("disc too small, skipping sqrt\n");
            dt =  -tc->currentvel / a;
        } else if (a>0) {
            tp_debug_print("using positive sqrt\n");
            dt = -tc->currentvel / a + pmSqrt(disc);
        } else {
            tp_debug_print("using negative sqrt\n");
            dt = -tc->currentvel / a - pmSqrt(disc);
        }

        tp_debug_print(" revised dt = %f\n", dt);
        //Update final velocity with actual result
        v_f = tc->currentvel + dt * a;
    }

    if (dt < TP_TIME_EPSILON) {
        //Close enough, call it done
        tp_debug_print("revised dt small, finishing tc\n");
        tc->progress = tc->target;
        tcSetSplitCycle(tc, 0.0, v_f);
    } else if (dt < tp->cycleTime ) {
        tp_debug_print(" corrected v_f = %f, a = %f\n", v_f, a);
        tcSetSplitCycle(tc, dt, v_f);
    } else {
        tp_debug_print(" dt = %f, not at end yet\n",dt);
    }
    return TP_ERR_OK;
}


STATIC int tpHandleSplitCycle(TP_STRUCT * const tp, TC_STRUCT * const tc,
        TC_STRUCT * const nexttc)
{
    if (tc->remove) {
        //Don't need to update since this segment is flagged for removal
        return TP_ERR_NO_ACTION;
    }

    //Pose data to calculate movement due to finishing current TC
    EmcPose before;
    tcGetPos(tc, &before);

    tp_debug_print("tc id %d splitting\n",tc->id);
    //Shortcut tc update by assuming we arrive at end
    tc->progress = tc->target;
    //Get displacement from prev. position
    EmcPose displacement;
    tcGetPos(tc, &displacement);
    emcPoseSelfSub(&displacement, &before);

    //Update tp's position
    emcPoseSelfAdd(&tp->currentPos, &displacement);

#ifdef TC_DEBUG
    double mag;
    emcPoseMagnitude(&displacement, &mag);
    tc_debug_print("cycle movement = %f\n",mag);
#endif

    //Run remaining cycle time in nexttc
    if (nexttc && tc->term_cond == TC_TERM_COND_TANGENT){
        nexttc->cycle_time = tp->cycleTime - tc->cycle_time;
        nexttc->currentvel = tc->vel_at_blend_start;
        tp_debug_print("Doing tangent split\n");
        tpUpdateCycle(tp, nexttc);
        //Update status for the split portion
        if (tc->cycle_time > nexttc->cycle_time) {
            //Majority of time spent in current segment
            tpToggleDIOs(tc);
            tpUpdateMovementStatus(tp, tc);
        } else {
            tpToggleDIOs(nexttc);
            tpUpdateMovementStatus(tp, nexttc);
        }
    }
    //This is the only place remove should be triggered
    tc->remove = 1;
    return TP_ERR_OK;
}

STATIC int tpHandleRegularCycle(TP_STRUCT * const tp, TC_STRUCT * const tc,
        TC_STRUCT * const nexttc)
{
    if (tc->remove) {
        //Don't need to update since this segment is flagged for removal
        return TP_ERR_NO_ACTION;
    }
    //Run with full cycle time
    tc_debug_print("Normal cycle\n");
    tc->cycle_time = tp->cycleTime;
    tpUpdateCycle(tp, tc);

    /* Parabolic blending */

    tpComputeBlendVelocity(tp, tc, nexttc, false, NULL);
    if (nexttc && tcIsBlending(tc)) {
        tpDoParabolicBlending(tp, tc, nexttc);
    } else {
        //Update status for a normal step
        tpToggleDIOs(tc);
        tpUpdateMovementStatus(tp, tc);
    }
    return TP_ERR_OK;
}

/**
 * Calculate an updated goal position for the next timestep.
 * This is the brains of the operation. It's called every TRAJ period and is
 * expected to set tp->currentPos to the new machine position. Lots of other
 * const tp fields (depth, done, etc) have to be twiddled to communicate the
 * status; I think those are spelled out here correctly and I can't clean it up
 * without breaking the API that the TP presents to motion.
 */
int tpRunCycle(TP_STRUCT * const tp, long period)
{
    //Pointers to current and next trajectory component
    TC_STRUCT *tc;
    TC_STRUCT *nexttc;
    TC_STRUCT * next2_tc;

    /* Get pointers to current and relevant future segments. It's ok here if
     * future segments don't exist (NULL pointers) as we check for this later).
     */
    tc = tcqItem(&tp->queue, 0);
    nexttc = tcqItem(&tp->queue, 1);
    next2_tc = tcqItem(&tp->queue, 2);

    //Set GUI status to "zero" state
    tpUpdateInitialStatus(tp);

    //If we have a NULL pointer, then the queue must be empty, so we're done.
    if(!tc) {
        tpHandleEmptyQueue(tp, emcmotStatus);
        return TP_ERR_WAITING;
    }

    tc_debug_print("-------------------\n");

#ifdef TC_DEBUG
    //Hack debug output for timesteps
    static double time_elapsed = 0;
    time_elapsed+=tp->cycleTime;
#endif

    //Check if we need to stop after this segment due to synchronization with
    //spindle or other conditions
    tpFlagEarlyStop(tp, tc, nexttc);
    tpFlagEarlyStop(tp, nexttc, next2_tc);
    tpForceFinalizeSegment(tp, next2_tc);

    if (tpHandleAbort(tp, tc, nexttc) == TP_ERR_STOPPED) {
        return TP_ERR_STOPPED;
    }

    //Return early if we have a reason to wait (i.e. not ready for motion)
    if (tpHandleWaiting(tp, tc) != TP_ERR_OK){
        return TP_ERR_WAITING;
    }

    if(tc->active == 0) {
        int res = tpActivateSegment(tp, tc);
        // Need to wait to continue motion, end planning here
        if (res == TP_ERR_WAITING) {
            return TP_ERR_WAITING;
        }
    }

    if (tc->motion_type == TC_RIGIDTAP) {
        tpUpdateRigidTapState(tp, tc);
    }

    //TODO revisit this logic and pack this into the status update function
    if(!tc->synchronized) {
        emcmotStatus->spindleSync = 0;
    }

    /** If synchronized with spindle, calculate requested velocity to track
     * spindle motion.*/
    switch (tc->synchronized) {
        case TC_SYNC_NONE:
            break;
        case TC_SYNC_VELOCITY:
            tp_debug_print("sync velocityyn");
            tpSyncVelocityMode(tp, tc, nexttc);
            break;
        case TC_SYNC_POSITION:
            tp_debug_print("sync position\n");
            tpSyncPositionMode(tp, tc, nexttc);
            break;
        default:
            tp_debug_print("unrecognized spindle sync state!\n");
            break;
    }

#ifdef TC_DEBUG
    EmcPose pos_before = tp->currentPos;
#endif

    // Update the current tc
    if (tc->splitting) {
        tpHandleSplitCycle(tp, tc, nexttc);
    } else {
        tpHandleRegularCycle(tp, tc, nexttc);
    }

#ifdef TC_DEBUG
    double mag;
    EmcPose disp;
    emcPoseSub(&tp->currentPos,&pos_before, &disp);
    emcPoseMagnitude(&disp, &mag);
    tc_debug_print("time: %.12e total movement = %.12e vel = %.12e\n",
            time_elapsed,
            mag, emcmotStatus->current_vel);
#endif

    // If TC is complete, remove it from the queue.
    if (tc->remove) {
        tpCompleteSegment(tp, tc);
    }

    return TP_ERR_OK;
}

int tpSetSpindleSync(TP_STRUCT * const tp, double sync, int mode) {
    //TODO update these fields to match new TC fields
    if(sync) {
        if (mode) {
            tp->synchronized = TC_SYNC_VELOCITY;
        } else {
            tp->synchronized = TC_SYNC_POSITION;
        }
        tp->uu_per_rev = sync;
    } else
        tp->synchronized = 0;

    return TP_ERR_OK;
}

int tpPause(TP_STRUCT * const tp)
{
    if (0 == tp) {
        return TP_ERR_FAIL;
    }
    tp->pausing = 1;
    return TP_ERR_OK;
}

int tpResume(TP_STRUCT * const tp)
{
    if (0 == tp) {
        return TP_ERR_FAIL;
    }
    tp->pausing = 0;
    return TP_ERR_OK;
}

int tpAbort(TP_STRUCT * const tp)
{
    if (0 == tp) {
        return TP_ERR_FAIL;
    }

    if (!tp->aborting) {
        /* const to abort, signal a pause and set our abort flag */
        tpPause(tp);
        tp->aborting = 1;
    }
    return tpClearDIOs(tp); //clears out any already cached DIOs
}

int tpGetMotionType(TP_STRUCT * const tp)
{
    return tp->motionType;
}

int tpGetPos(TP_STRUCT const * const tp, EmcPose * const pos)
{

    if (0 == tp) {
        ZERO_EMC_POSE((*pos));
        return TP_ERR_FAIL;
    } else {
        *pos = tp->currentPos;
    }

    return TP_ERR_OK;
}

int tpIsDone(TP_STRUCT * const tp)
{
    if (0 == tp) {
        return TP_ERR_OK;
    }

    return tp->done;
}

int tpQueueDepth(TP_STRUCT * const tp)
{
    if (0 == tp) {
        return TP_ERR_OK;
    }

    return tp->depth;
}

int tpActiveDepth(TP_STRUCT * const tp)
{
    if (0 == tp) {
        return TP_ERR_OK;
    }

    return tp->activeDepth;
}

int tpSetAout(TP_STRUCT * const tp, unsigned char index, double start, double end) {
    if (0 == tp) {
        return TP_ERR_FAIL;
    }
    tp->syncdio.anychanged = 1; //something has changed
    tp->syncdio.aio_mask |= (1 << index);
    tp->syncdio.aios[index] = start;
    return TP_ERR_OK;
}

int tpSetDout(TP_STRUCT * const tp, int index, unsigned char start, unsigned char end) {
    if (0 == tp) {
        return TP_ERR_FAIL;
    }
    tp->syncdio.anychanged = 1; //something has changed
    tp->syncdio.dio_mask |= (1 << index);
    if (start > 0)
        tp->syncdio.dios[index] = 1; // the end value can't be set from canon currently, and has the same value as start
    else
        tp->syncdio.dios[index] = -1;
    return TP_ERR_OK;
}

// vim:sw=4:sts=4:et:
