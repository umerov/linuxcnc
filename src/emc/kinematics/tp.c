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
#include "rtapi_math.h"
#include "../motion/motion.h"
#include "hal.h"
#include "../motion/mot_priv.h"
#include "motion_debug.h"

//FIXME Roll this into RTAPI debug statements
#define TP_DEBUG_OUTPUT
#include "tp_debug.h"


extern emcmot_status_t *emcmotStatus;
extern emcmot_debug_t *emcmotDebug;

int output_chan = 0;
syncdio_t syncdio; //record tpSetDout's here
const PmQuaternion IDENTITY_QUAT = { 1.0, 0.0, 0.0, 0.0 };

/**
 * Create the trajectory planner structure with an empty queue.
 */
int tpCreate(TP_STRUCT * tp, int _queueSize, TC_STRUCT * tcSpace)
{
    if (0 == tp) {
        return -1;
    }

    if (_queueSize <= 0) {
        tp->queueSize = TP_DEFAULT_QUEUE_SIZE;
    } else {
        tp->queueSize = _queueSize;
    }

    /* create the queue */
    if (-1 == tcqCreate(&tp->queue, tp->queueSize, tcSpace)) {
        return -1;
    }

    /* init the rest of our data */
    return tpInit(tp);
}

/**
 * Clears any potential DIO toggles and anychanged.
 * If any DIOs need to be changed: dios[i] = 1, DIO needs to get turned on, -1
 * = off
 */
int tpClearDIOs() {
    //XXX: All IO's will be flushed on next synced aio/dio! Is it ok?
    int i;
    syncdio.anychanged = 0;
    syncdio.dio_mask = 0;
    syncdio.aio_mask = 0;
    for (i = 0; i < num_dio; i++)
        syncdio.dios[i] = 0;
    for (i = 0; i < num_aio; i++)
        syncdio.aios[i] = 0;

    return 0;
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
int tpClear(TP_STRUCT * tp)
{
    tcqInit(&tp->queue);
    tp->queueSize = 0;
    tp->goalPos = tp->currentPos;
    tp->nextId = 0;
    tp->execId = 0;
    tp->motionType = 0;
    tp->termCond = TC_TERM_COND_BLEND;
    tp->tolerance = 0.0;
    tp->done = 1;
    tp->depth = tp->activeDepth = 0;
    tp->aborting = 0;
    tp->pausing = 0;
    tp->vScale = emcmotStatus->net_feed_scale;
    tp->synchronized = 0;
    tp->velocity_mode = 0;
    tp->uu_per_rev = 0.0;
    emcmotStatus->spindleSync = 0;
    emcmotStatus->current_vel = 0.0;
    emcmotStatus->requested_vel = 0.0;
    emcmotStatus->distance_to_go = 0.0;
    ZERO_EMC_POSE(emcmotStatus->dtg);

    return tpClearDIOs();
}

/**
 * Fully initialize the tp structure.
 * Sets tp configuration to default values and calls tpClear to create a fresh,
 * empty queue.
 */
int tpInit(TP_STRUCT * tp)
{
    tp->cycleTime = 0.0;
    tp->vLimit = 0.0;
    tp->vScale = 1.0;
    tp->aMax = 0.0;
    tp->vMax = 0.0;
    tp->ini_maxvel = 0.0;
    tp->wMax = 0.0;
    tp->wDotMax = 0.0;

    ZERO_EMC_POSE(tp->currentPos);

    return tpClear(tp);
}

/**
 * Set the cycle time for the trajectory planner.
 */
int tpSetCycleTime(TP_STRUCT * tp, double secs)
{
    if (0 == tp || secs <= 0.0) {
        return -1;
    }

    tp->cycleTime = secs;

    return 0;
}

/**
 * Set requested velocity and absolute maximum velocity (bounded by machine).
 * This is called before adding lines or circles, specifying vMax (the velocity
 * requested by the F word) and ini_maxvel, the max velocity possible before
 * meeting a machine constraint caused by an AXIS's max velocity.  (the TP is
 * allowed to go up to this high when feed override >100% is requested)  These
 * settings apply to subsequent moves until changed.
 */
int tpSetVmax(TP_STRUCT * tp, double vMax, double ini_maxvel)
{
    if (0 == tp || vMax <= 0.0 || ini_maxvel <= 0.0) {
        return -1;
    }

    tp->vMax = vMax;
    tp->ini_maxvel = ini_maxvel;

    return 0;
}

/**
 * (?) Set the tool tip maximum velocity.
 * I think this is the [TRAJ] max velocity. This should be the max velocity of
 * the TOOL TIP, not necessarily any particular axis. This applies to
 * subsequent moves until changed.
 */
int tpSetVlimit(TP_STRUCT * tp, double vLimit)
{
    if (!tp) return -1;

    if (vLimit < 0.) 
        tp->vLimit = 0.;
    else
        tp->vLimit = vLimit;

    return 0;
}

/** Sets the max acceleration for the trajectory planner. */
int tpSetAmax(TP_STRUCT * tp, double aMax)
{
    if (0 == tp || aMax <= 0.0) {
        return -1;
    }

    tp->aMax = aMax;

    return 0;
}

/**
 * Sets the id that will be used for the next appended motions.  
 * nextId is incremented so that the next time a motion is appended its id will
 * be one more than the previous one, modulo a signed int. If you want your own
 * ids for each motion, call this before each motion you append and stick what
 * you want in here.
 */
int tpSetId(TP_STRUCT * tp, int id)
{

    if (!MOTION_ID_VALID(id)) {
        rtapi_print_msg(RTAPI_MSG_ERR, "tpSetId: invalid motion id %d\n", id);
        return -1;
    }

    if (0 == tp) {
        return -1;
    }

    tp->nextId = id;

    return 0;
}

/** Returns the id of the last motion that is currently
  executing.*/
int tpGetExecId(TP_STRUCT * tp)
{
    if (0 == tp) {
        return -1;
    }

    return tp->execId;
}

/**
 * Sets the termination condition for all subsequent queued moves. 
 * If cond is TC_TERM_STOP, motion comes to a stop before a subsequent move
 * begins. If cond is TC_TERM_BLEND, the following move is begun when the
 * current move decelerates.
 */
int tpSetTermCond(TP_STRUCT * tp, int cond, double tolerance)
{
    if (0 == tp) {
        return -1;
    }

    switch (cond) {
        //Purposeful waterfall for now
        case TC_TERM_COND_BLEND:
        case TC_TERM_COND_TANGENT:
        case TC_TERM_COND_STOP:
            tp->termCond = cond;
            tp->tolerance = tolerance;
            break;
        default:
            //Invalid condition
            return  -1;
    }

    return 0;
}

/**
 * Used to tell the tp the initial position. 
 * It sets the current position AND the goal position to be the same.  Used
 * only at TP initialization and when switching modes.
 */
int tpSetPos(TP_STRUCT * tp, EmcPose pos)
{
    if (0 == tp) {
        return -1;
    }

    tp->currentPos = pos;
    tp->goalPos = pos;

    return 0;
}

/**
 * Check for valid tp before queueing additional moves.
 */
int tpErrorCheck(TP_STRUCT *tp) {

    if (!tp) {
        rtapi_print_msg(RTAPI_MSG_ERR, "TP is null\n");
        return -1;
    }
    if (tp->aborting) {
        rtapi_print_msg(RTAPI_MSG_ERR, "TP is aborting\n");
        return -1;
    }
    return 0;
}


/**
 * Add a newly created motion segment to the tp queue.
 * Returns an error code if the queue operation fails.
 */
static inline int tpAddSegmentToQueue(TP_STRUCT* tp, TC_STRUCT tc, EmcPose end){

    if (tcqPut(&tp->queue, tc) == -1) {
        rtapi_print_msg(RTAPI_MSG_ERR, "tcqPut failed.\n");
        return -1;
    }

    tp->goalPos = end;      // remember the end of this move, as it's
    // the start of the next one.
    tp->done = 0;
    tp->depth = tcqLen(&tp->queue);
    tp->nextId++;

    return 0;
}


/**
 * Adds a rigid tap cycle to the motion queue.
 */
int tpAddRigidTap(TP_STRUCT *tp, EmcPose end, double vel, double ini_maxvel, 
        double acc, unsigned char enables) {
    TC_STRUCT tc;
    PmLine line_xyz;
    PmPose start_xyz, end_xyz;
    PmCartesian abc, uvw;

    if (tpErrorCheck(tp)<0) return -1;

    start_xyz.tran = tp->goalPos.tran;
    end_xyz.tran = end.tran;

    start_xyz.rot = IDENTITY_QUAT;
    end_xyz.rot = IDENTITY_QUAT;

    // abc cannot move
    abc.x = tp->goalPos.a;
    abc.y = tp->goalPos.b;
    abc.z = tp->goalPos.c;

    uvw.x = tp->goalPos.u;
    uvw.y = tp->goalPos.v;
    uvw.z = tp->goalPos.w;

    pmLineInit(&line_xyz, start_xyz, end_xyz);

    tc.sync_accel = 0;
    tc.cycle_time = tp->cycleTime;
    tc.coords.rigidtap.reversal_target = line_xyz.tmag;

    // allow 10 turns of the spindle to stop - we don't want to just go on forever
    tc.target = line_xyz.tmag + 10. * tp->uu_per_rev;

    tc.progress = 0.0;
    tc.reqvel = vel;
    tc.maxaccel = acc;
    tc.feed_override = 0.0;
    tc.maxvel = ini_maxvel;
    tc.id = tp->nextId;
    tc.active = 0;
    tc.atspeed = 1;

    tc.currentvel = 0.0;
    tc.blending = 0;
    tc.blend_vel = 0.0;
    tc.vel_at_blend_start = 0.0;

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
        return -1;
    }
    tc.synchronized = tp->synchronized;

    tc.uu_per_rev = tp->uu_per_rev;
    tc.velocity_mode = tp->velocity_mode;
    tc.enables = enables;
    tc.indexrotary = -1;

    tc.finalvel=0.0;

    if (syncdio.anychanged != 0) {
        tc.syncdio = syncdio; //enqueue the list of DIOs that need toggling
        tpClearDIOs(); // clear out the list, in order to prepare for the next time we need to use it
    } else {
        tc.syncdio.anychanged = 0;
    }

    //Assume non-zero error code is failure
    return tpAddSegmentToQueue(tp,tc,end);
}

/**
 * Add a straight line to the tc queue.
 * This is a coordinated move in any or all of the six axes. It goes from the
 * end of the previous move to the new end specified here at the
 * currently-active accel and vel settings from the tp struct.
 */
int tpAddLine(TP_STRUCT * tp, EmcPose end, int type, double vel, double ini_maxvel, double acc, unsigned char enables, char atspeed, int indexrotary)
{
    TC_STRUCT tc;
    PmLine line_xyz, line_uvw, line_abc;
    PmPose start_xyz, end_xyz;
    PmPose start_uvw, end_uvw;
    PmPose start_abc, end_abc;

    if (tpErrorCheck(tp)<0) return -1;

    start_xyz.tran = tp->goalPos.tran;
    end_xyz.tran = end.tran;

    start_uvw.tran.x = tp->goalPos.u;
    start_uvw.tran.y = tp->goalPos.v;
    start_uvw.tran.z = tp->goalPos.w;
    end_uvw.tran.x = end.u;
    end_uvw.tran.y = end.v;
    end_uvw.tran.z = end.w;

    start_abc.tran.x = tp->goalPos.a;
    start_abc.tran.y = tp->goalPos.b;
    start_abc.tran.z = tp->goalPos.c;
    end_abc.tran.x = end.a;
    end_abc.tran.y = end.b;
    end_abc.tran.z = end.c;

    start_xyz.rot = IDENTITY_QUAT;
    end_xyz.rot = IDENTITY_QUAT;
    start_uvw.rot = IDENTITY_QUAT;
    end_uvw.rot = IDENTITY_QUAT;
    start_abc.rot = IDENTITY_QUAT;
    end_abc.rot = IDENTITY_QUAT;

    pmLineInit(&line_xyz, start_xyz, end_xyz);
    pmLineInit(&line_uvw, start_uvw, end_uvw);
    pmLineInit(&line_abc, start_abc, end_abc);

    tc.sync_accel = 0;
    tc.cycle_time = tp->cycleTime;

    if (!line_xyz.tmag_zero) 
        tc.target = line_xyz.tmag;
    else if (!line_uvw.tmag_zero)
        tc.target = line_uvw.tmag;
    else
        tc.target = line_abc.tmag;

    tc.progress = 0.0;
    tc.reqvel = vel;
    tc.maxaccel = acc;
    tc.feed_override = 0.0;
    tc.maxvel = ini_maxvel;
    tc.id = tp->nextId;
    tc.active = 0;
    tc.atspeed = atspeed;

    tc.currentvel = 0.0;
    tc.blending = 0;
    tc.blend_vel = 0.0;
    tc.vel_at_blend_start = 0.0;

    tc.coords.line.xyz = line_xyz;
    tc.coords.line.uvw = line_uvw;
    tc.coords.line.abc = line_abc;
    tc.motion_type = TC_LINEAR;
    tc.canon_motion_type = type;

    tc.term_cond = tp->termCond;
    tc.tolerance = tp->tolerance;

    tc.synchronized = tp->synchronized;
    tc.velocity_mode = tp->velocity_mode;
    tc.uu_per_rev = tp->uu_per_rev;
    tc.enables = enables;
    tc.indexrotary = indexrotary;


    TC_STRUCT* last_tc;
    if (tcqLen(&tp->queue)>1)
        last_tc=tcqLast(&(tp->queue));
    else last_tc=NULL;
    //KLUDGE temporary hack to fake optimization from hand-tuned G-code
    tc.finalvel=0.0;
    if (last_tc && last_tc->term_cond == TC_TERM_COND_TANGENT){
        if ( tc.reqvel < last_tc->reqvel) last_tc->finalvel=tc.reqvel; // Assume single constant feedrate for speed test
        else last_tc->finalvel=last_tc->reqvel;
    }


    if (syncdio.anychanged != 0) {
        tc.syncdio = syncdio; //enqueue the list of DIOs that need toggling
        tpClearDIOs(); // clear out the list, in order to prepare for the next time we need to use it
    } else {
        tc.syncdio.anychanged = 0;
    }

    //Assume non-zero error code is failure
    return tpAddSegmentToQueue(tp,tc,end);
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
int tpAddCircle(TP_STRUCT * tp, EmcPose end,
        PmCartesian center, PmCartesian normal, int turn, int type,
        double vel, double ini_maxvel, double acc, unsigned char enables, char atspeed)
{
    TC_STRUCT tc;
    PmCircle circle;
    PmLine line_uvw, line_abc;
    PmPose start_xyz, end_xyz;
    PmPose start_uvw, end_uvw;
    PmPose start_abc, end_abc;
    double helix_z_component;   // z of the helix's cylindrical coord system
    double helix_length;

    if (tpErrorCheck(tp)<0) return -1;

    start_xyz.tran = tp->goalPos.tran;
    end_xyz.tran = end.tran;

    start_abc.tran.x = tp->goalPos.a;
    start_abc.tran.y = tp->goalPos.b;
    start_abc.tran.z = tp->goalPos.c;
    end_abc.tran.x = end.a;
    end_abc.tran.y = end.b;
    end_abc.tran.z = end.c;

    start_uvw.tran.x = tp->goalPos.u;
    start_uvw.tran.y = tp->goalPos.v;
    start_uvw.tran.z = tp->goalPos.w;
    end_uvw.tran.x = end.u;
    end_uvw.tran.y = end.v;
    end_uvw.tran.z = end.w;

    start_xyz.rot = IDENTITY_QUAT;
    end_xyz.rot = IDENTITY_QUAT;
    start_uvw.rot = IDENTITY_QUAT;
    end_uvw.rot = IDENTITY_QUAT;
    start_abc.rot = IDENTITY_QUAT;
    end_abc.rot = IDENTITY_QUAT;

    pmCircleInit(&circle, start_xyz, end_xyz, center, normal, turn);
    pmLineInit(&line_uvw, start_uvw, end_uvw);
    pmLineInit(&line_abc, start_abc, end_abc);

    // find helix length
    pmCartMag(circle.rHelix, &helix_z_component);
    helix_length = pmSqrt(pmSq(circle.angle * circle.radius) +
            pmSq(helix_z_component));

    tc.sync_accel = 0;
    tc.cycle_time = tp->cycleTime;
    tc.target = helix_length;
    tc.progress = 0.0;
    tc.reqvel = vel;
    //Assume acceleration ratio of 1
    tc.maxaccel = acc*0.7071;
    tc.feed_override = 0.0;
    tc.maxvel = ini_maxvel;
    tc.id = tp->nextId;
    tc.active = 0;
    tc.atspeed = atspeed;

    tc.currentvel = 0.0;
    tc.blending = 0;
    tc.blend_vel = 0.0;
    tc.vel_at_blend_start = 0.0;

    tc.coords.circle.xyz = circle;
    tc.coords.circle.uvw = line_uvw;
    tc.coords.circle.abc = line_abc;
    tc.motion_type = TC_CIRCULAR;
    tc.canon_motion_type = type;
    tc.term_cond = tp->termCond;
    tc.tolerance = tp->tolerance;

    tc.synchronized = tp->synchronized;
    tc.velocity_mode = tp->velocity_mode;
    tc.uu_per_rev = tp->uu_per_rev;
    tc.enables = enables;
    tc.indexrotary = -1;

    //TEMPORARY HACK
    TC_STRUCT* last_tc;
    if (tcqLen(&tp->queue)>1)
        last_tc=tcqLast(&(tp->queue));
    else last_tc=NULL;
    //KLUDGE temporary hack to fake optimization from hand-tuned G-code
    tc.finalvel=0.0;
    if (last_tc && last_tc->term_cond == TC_TERM_COND_TANGENT){
        if ( tc.reqvel < last_tc->reqvel) last_tc->finalvel=tc.reqvel; // Assume single constant feedrate for speed test
        else last_tc->finalvel=last_tc->reqvel;
    }

    if (syncdio.anychanged != 0) {
        tc.syncdio = syncdio; //enqueue the list of DIOs that need toggling
        tpClearDIOs(); // clear out the list, in order to prepare for the next time we need to use it
    } else {
        tc.syncdio.anychanged = 0;
    }

    //Assume non-zero error code is failure
    return tpAddSegmentToQueue(tp,tc,end);
}

/**
 * Handle potential overshoot for tangent segments.
 * Ending a TC with final velocity often leads to small "overshoots". Since we
 * are parameterized by arc length, and the next move is tangent, just apply
 * overshoot to the next TC.
 */
static void tpCheckOvershoot(TC_STRUCT* tc, TC_STRUCT* nexttc,EmcPose* secondary_before) {
    double overshoot=0.0;
    //Apply any overshoot to the next TC, and make sure its initial
    //velocity is the same. This gives C1 continuity at tangent blends.
    if (tc->progress > tc->target) {
        //Store previous position
        *secondary_before = tcGetPos(nexttc);
        overshoot = tc->progress - tc->target;
        if (nexttc){
            nexttc->progress = overshoot;
            nexttc->currentvel = tc->currentvel;
            tc->progress=tc->target;
            rtapi_print("Overshot by %f at end of move %d\n",overshoot,tc->id);
            rtapi_print("setting init vel to %f\n",nexttc->currentvel);
        }
        else {
            //Kludge fix to see if we can catch the crash at the end of the dummy toolpath
            tc->progress=tc->target;
        }
    }
    //NOTE: we're assuming that tangent blends mean there's enough length in
    //the next segment to deal with this.  TODO: either add a check here or
    //make damn sure that the optimizer does its job.
}

/**
 * Adjusts blend velocity and acceleration to safe limits.
 * If we are blending between tc and nexttc, then we need to figure out what a
 * safe blend velocity is based on the known trajectory parameters. This
 * function updates the TC_STRUCT data with a safe blend velocity.
 */
static double tpComputeBlendVelocity(TC_STRUCT* tc, TC_STRUCT* nexttc) {

    //Store local blend velocity copy
    double blend_vel=tc->blend_vel;

    if(nexttc && nexttc->maxaccel) {
        blend_vel = nexttc->maxaccel * 
            pmSqrt(nexttc->target / nexttc->maxaccel);
        if(blend_vel > nexttc->reqvel * nexttc->feed_override) {
            // segment has a cruise phase so let's blend over the 
            // whole accel period if possible
            blend_vel = nexttc->reqvel * nexttc->feed_override;
        }
        if(tc->maxaccel < nexttc->maxaccel)
            blend_vel *= tc->maxaccel/nexttc->maxaccel;

        if(tc->tolerance) {
            /* see diagram blend.fig.  T (blend tolerance) is given, theta
             * is calculated from dot(s1,s2)
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
            double dot;
            double theta;
            PmCartesian v1, v2;

            v1 = tcGetEndingUnitVector(tc);
            v2 = tcGetStartingUnitVector(nexttc);
            pmCartCartDot(v1, v2, &dot);

            theta = acos(-dot)/2.0; 
            if(cos(theta) > 0.001) {
                tblend_vel = 2.0 * pmSqrt(tc->maxaccel * tc->tolerance / cos(theta));
                if(tblend_vel < blend_vel)
                    blend_vel = tblend_vel;
            }
        }
    }
    return blend_vel;
    /*rtapi_print("Blend vel of id %d is %f\n",tc->id,tc->blend_vel);*/
}

/**
 * Clip (saturate) a value x to be within +/- max.
 */
static double saturate(double x, double max) {
    if ( x > max ) return max;
    else if ( x < (-max) ) return -max;
    else return x;
}

/**
 * Compute the updated position and velocity over one timestep.
 * @param tc trajectory component being processed
 * @param v optional velocity output as reference
 * @param on_final_decel optional output flag indicating we are slowing towards the goal speed.
 *
 * This function creates the trapezoidal velocity profile based on tc's
 * velocity and acceleration limits. The formula has been tweaked slightly to
 * allow a non-zero velocity at the instant the target is reached.
 */
void tcRunCycle(TP_STRUCT *tp, TC_STRUCT *tc, double *v, int *on_final_decel) {
    double discr, maxnewvel, newvel, newaccel=0, delta_pos;
    double discr_term1, discr_term2, discr_term3;

    // Find maximum allowed velocity from feed and machine limits
    double req_vel = tc->reqvel * tc->feed_override;
    double final_vel = tc->finalvel * tc->feed_override;

    if (req_vel > tc->maxvel) req_vel = tc->maxvel;
    //Clamp final velocity to the max velocity we can achieve
    if (final_vel > req_vel) final_vel = req_vel;
    // Need this to plan down to zero V
    if (tp->pausing) final_vel = 0.0;

    if (!tc->blending) tc->vel_at_blend_start = tc->currentvel;

    delta_pos = tc->target - tc->progress;

    discr_term1 = pmSq(final_vel);
    discr_term2 = tc->maxaccel * (2.0 * delta_pos - tc->currentvel * tc->cycle_time);
    discr_term3 = pmSq(tc->maxaccel * tc->cycle_time / 2.0);

    discr = discr_term1 + discr_term2 + discr_term3;

    // Descriminant is a little more complicated with final velocity term If
    // descriminant < 0, we've overshot (or are about to). Do the best we can
    // in this situation
    if (discr < 0.0) {
        newvel = maxnewvel = 0.0;
    } 
    else {
        newvel = maxnewvel = -0.5 * tc->maxaccel * tc->cycle_time + pmSqrt(discr);
    }

    if (newvel > req_vel) newvel = req_vel;

    if (newvel < 0.0 ) {
        //If we're not hitting a tangent move, then we need to throw out any overshoot to force an exact stop.
        //FIXME this means a momentary spike in acceleration, test to see if it's a problem
        newvel = newaccel = 0.0;

        if ( !(tc->term_cond == TC_TERM_COND_TANGENT) || (tc->progress < tc->target ) ) {
            debug("calculated newvel = %f, with T = %f, P = %f", newvel,tc->target,tc->progress);
            tc->progress = tc->target;
        }
    }
    else {

        bool is_pure_rotary = (tc->motion_type == TC_LINEAR) &&
            (tc->coords.line.xyz.tmag_zero) && (tc->coords.line.uvw.tmag_zero);
        bool is_position_synced = (!tc->synchronized) || tc->velocity_mode;

        // if the motion is not purely rotary axes (and therefore in angular
        // units), clamp motion's velocity at TRAJ MAX_VELOCITY (tooltip
        // maxvel) except when it's synced to spindle position.
        if (!is_pure_rotary && !is_position_synced && newvel > tp->vLimit) {
            newvel = tp->vLimit;
        }

        // get acceleration to reach newvel, bounded by machine maximum
        newaccel = (newvel - tc->currentvel) / tc->cycle_time;
        newaccel = saturate(newaccel,tc->maxaccel);

        newvel = tc->currentvel + newaccel * tc->cycle_time;
        // update position in this tc using trapezoidal integration
        // Note that progress can be greater than the target after this step. 
        // TODO: handle this in tp
        tc->progress += (newvel + tc->currentvel) * 0.5 * tc->cycle_time;
        tc->currentvel = newvel;
    }
    debug("TC result: v = %f,dtg = %f, vf = %f, T = %f, P = %f", newvel, tc->target-tc->progress, final_vel,tc->target,tc->progress);

    if (v) *v = newvel;
    if (on_final_decel) *on_final_decel = fabs(maxnewvel - newvel) < 0.001;
}

void tpToggleDIOs(TC_STRUCT * tc) {
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
 * during a reversal.
 * TODO: detailed documentation here
 */
static void tpHandleRigidTap(emcmot_status_t* emcmotStatus,TC_STRUCT* tc, tp_spindle_status_t* status ){

    static double old_spindlepos;
    double new_spindlepos = emcmotStatus->spindleRevs;
    if (emcmotStatus->spindle.direction < 0) new_spindlepos = -new_spindlepos;

    switch (tc->coords.rigidtap.state) {
        case TAPPING:
            debug("TAPPING");
            if (tc->progress >= tc->coords.rigidtap.reversal_target) {
                // command reversal
                emcmotStatus->spindle.speed *= -1;
                tc->coords.rigidtap.state = REVERSING;
            }
            break;
        case REVERSING:
            debug("REVERSING");
            if (new_spindlepos < old_spindlepos) {
                PmPose start, end;
                PmLine *aux = &tc->coords.rigidtap.aux_xyz;
                // we've stopped, so set a new target at the original position
                tc->coords.rigidtap.spindlerevs_at_reversal = new_spindlepos + status->offset;

                pmLinePoint(&tc->coords.rigidtap.xyz, tc->progress, &start);
                end = tc->coords.rigidtap.xyz.start;
                pmLineInit(aux, start, end);
                debug("old target = %f",tc->target);
                tc->coords.rigidtap.reversal_target = aux->tmag;
                tc->target = aux->tmag + 10. * tc->uu_per_rev;
                tc->progress = 0.0;
                debug("new target = %f",tc->target);

                tc->coords.rigidtap.state = RETRACTION;
            }
            old_spindlepos = new_spindlepos;
            debug("Spindlepos = %f",new_spindlepos);
            break;
        case RETRACTION:
            debug("RETRACTION");
            if (tc->progress >= tc->coords.rigidtap.reversal_target) {
                emcmotStatus->spindle.speed *= -1;
                tc->coords.rigidtap.state = FINAL_REVERSAL;
            }
            break;
        case FINAL_REVERSAL:
            debug("FINAL_REVERSAL");
            if (new_spindlepos > old_spindlepos) {
                PmPose start, end;
                PmLine *aux = &tc->coords.rigidtap.aux_xyz;
                pmLinePoint(aux, tc->progress, &start);
                end = tc->coords.rigidtap.xyz.start;
                pmLineInit(aux, start, end);
                tc->target = aux->tmag;
                tc->progress = 0.0;
                //No longer need spindle sync at this point
                tc->synchronized = 0;
                tc->reqvel = tc->maxvel;

                tc->coords.rigidtap.state = FINAL_PLACEMENT;
            }
            old_spindlepos = new_spindlepos;
            break;
        case FINAL_PLACEMENT:
            debug("FINAL_PLACEMENT\n");
            // this is a regular move now, it'll stop at target above.
            break;
    }
}


/**
 * Update emcMotStatus with information about trajectory motion.
 * Based on the specified trajectory segment tc, read its progress and status
 * flags. Then, update the emcmotStatus structure with this information.
 */
static void tpUpdateMovementStatus(TP_STRUCT* tp, TC_STRUCT* tc, emcmot_status_t* emcmotStatus){
    EmcPose target = tcGetEndpoint(tc);

    tp->motionType = tc->canon_motion_type;
    emcmotStatus->distance_to_go = tc->target - tc->progress;
    emcmotStatus->enables_queued = tc->enables;
    // report our line number to the guis
    tp->execId = tc->id;
    emcmotStatus->requested_vel = tc->reqvel;
    emcmotStatus->current_vel = tc->currentvel;

    emcmotStatus->dtg.tran.x = target.tran.x - tp->currentPos.tran.x;
    emcmotStatus->dtg.tran.y = target.tran.y - tp->currentPos.tran.y;
    emcmotStatus->dtg.tran.z = target.tran.z - tp->currentPos.tran.z;
    emcmotStatus->dtg.a = target.a - tp->currentPos.a;
    emcmotStatus->dtg.b = target.b - tp->currentPos.b;
    emcmotStatus->dtg.c = target.c - tp->currentPos.c;
    emcmotStatus->dtg.u = target.u - tp->currentPos.u;
    emcmotStatus->dtg.v = target.v - tp->currentPos.v;
    emcmotStatus->dtg.w = target.w - tp->currentPos.w;

}


/**
 * Do a parabolic blend by updating the nexttc.
 * Perform the actual blending process by updating the nexttc.
 */
static void tpDoParabolicBlend(TP_STRUCT* tp, TC_STRUCT* tc, TC_STRUCT* nexttc, double primary_vel){

    /*EmcPose secondary_before = tcGetPos(nexttc);*/
    //Store the actual requested velocity
    double save_vel = nexttc->reqvel;

    nexttc->reqvel = nexttc->feed_override > 0.0 ? 
        ((tc->vel_at_blend_start - primary_vel) / nexttc->feed_override) :
        0.0;
    tcRunCycle(tp, nexttc, NULL, NULL);
    //Restore the blend velocity
    nexttc->reqvel = save_vel;

}


/**
 * Calculate the displacement between a previous pose and the current tc position.
 * This function encapsulates the simple but verbose displacement calculation
 * based on an initial position. Because of the blending method, we need to use
 * displacement instead of absolute position when blending between moves.
 */
static void tpFindDisplacement(TC_STRUCT* tc, EmcPose before, EmcPose* displacement){

    EmcPose after = tcGetPos(tc);

    pmCartCartSub(after.tran, before.tran, 
            &(displacement->tran));
    displacement->a = after.a - before.a;
    displacement->b = after.b - before.b;
    displacement->c = after.c - before.c;

    displacement->u = after.u - before.u;
    displacement->v = after.v - before.v;
    displacement->w = after.w - before.w;
}


/**
 * Update the planner's position, given a displacement.
 */
static void tpUpdatePosition(TP_STRUCT* tp,EmcPose* displacement){

    pmCartCartAdd(tp->currentPos.tran, displacement->tran, 
            &(tp->currentPos.tran));
    tp->currentPos.a += displacement->a;
    tp->currentPos.b += displacement->b;
    tp->currentPos.c += displacement->c;

    tp->currentPos.u += displacement->u;
    tp->currentPos.v += displacement->v;
    tp->currentPos.w += displacement->w;
}


/**
 * Cleanup if tc is not valid (empty queue). 
 * This can represent the end of the
 * program OR QUEUE STARVATION.  In either case, I want to stop.  Some may not
 * agree that's what it should do.
 */
static void tpHandleEmptyQueue(TP_STRUCT* tp, emcmot_status_t* emcmotStatus){

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

//TODO document
static void tpSetRotaryUnlock(int axis, int unlock) {
    emcmotSetRotaryUnlock(axis, unlock);
}

static int tpGetRotaryIsUnlocked(int axis) {
    return emcmotGetRotaryIsUnlocked(axis);
}


/**
 * Cleanup after a trajectory segment is complete.
 * If the current move is complete and we're not waiting on the spindle for
 * this move, then pop if off the queue and perform cleanup operations.
 * Finally, get the next move in the queue.
 */
static TC_STRUCT* tpCompleteSegment(TP_STRUCT* tp, TC_STRUCT* tc, tp_spindle_status_t* status){
    // if we're synced, and this move is ending, save the
    // spindle position so the next synced move can be in
    // the right place.

    debug("Finished tc id %d",tc->id);
    if(tc->synchronized)
        status->offset += tc->target/tc->uu_per_rev;
    else
        status->offset = 0.0;

    if(tc->indexrotary != -1) {
        // this was an indexing move, so before we remove it we must
        // relock the axis
        tpSetRotaryUnlock(tc->indexrotary, 0);
        // if it is now locked, fall through and remove the finished move.
        // otherwise, just come back later and check again
        if(tpGetRotaryIsUnlocked(tc->indexrotary))
            return NULL;
    }

    // done with this move
    tcqRemove(&tp->queue, 1);

    // so get next move
    tc = tcqItem(&tp->queue, 0);

    if(!tc) return NULL;

    debug("Found next tc id %d\n",tc->id);
    return tc;
}

static int tpHandleAbort(TP_STRUCT* tp, TC_STRUCT* tc, TC_STRUCT* nexttc, tp_spindle_status_t* status){

    // an abort message has come
    if( MOTION_ID_VALID(status->waiting_for_index) ||
            MOTION_ID_VALID(status->waiting_for_atspeed) ||
            (tc->currentvel == 0.0 && !nexttc) || 
            (tc->currentvel == 0.0 && nexttc && nexttc->currentvel == 0.0) ) {
        tcqInit(&tp->queue);
        tp->goalPos = tp->currentPos;
        tp->done = 1;
        tp->depth = tp->activeDepth = 0;
        tp->aborting = 0;
        tp->execId = 0;
        tp->motionType = 0;
        tp->synchronized = 0;
        status->waiting_for_index = MOTION_INVALID_ID;
        status->waiting_for_atspeed = MOTION_INVALID_ID;
        emcmotStatus->spindleSync = 0;
        tpResume(tp);
        return 0;
    } else {
        tc->reqvel = 0.0;
        if(nexttc) nexttc->reqvel = 0.0;
    }
    //FIXME consistent error codes
    return 1;

}


/**
 * Check if the segment waiting for an index has changed.
 * If the current segment waiting for an index is not the current segment, then
 * something has gone wrong. The fix for now is to just update status so we're
 * waiting in the current segment instead. (Rob's understanding)
 */
static int tpCheckWaiting(TC_STRUCT* tc, tp_spindle_status_t* status){

    // this is no longer the segment we were waiting_for_index for
    if (MOTION_ID_VALID(status->waiting_for_index) && status->waiting_for_index != tc->id) 
    {
        rtapi_print_msg(RTAPI_MSG_ERR,
                "Was waiting for index on motion id %d, but reached id %d\n",
                status->waiting_for_index, tc->id);
        status->waiting_for_index = MOTION_INVALID_ID;
    }

    if (MOTION_ID_VALID(status->waiting_for_atspeed) && status->waiting_for_atspeed != tc->id)  
    {

        rtapi_print_msg(RTAPI_MSG_ERR,
                "Was waiting for atspeed on motion id %d, but reached id %d\n",
                status->waiting_for_atspeed, tc->id);
        status->waiting_for_atspeed = MOTION_INVALID_ID;
    }

    if (MOTION_ID_VALID(status->waiting_for_atspeed)) {
        if(!emcmotStatus->spindle_is_atspeed) {
            // spindle is still not at the right speed, so wait another cycle
            return 1;
        } else {
            status->waiting_for_atspeed = MOTION_INVALID_ID;
        }
    }

    return 0;
}


/**
 * Get a pointer to nexttc if we can, based on conditions.
 * Once an active TC is created in the planner, we want to know the nexttc if
 * we can get it. it's not an error if nexttc is missing (like in the MDI, or
 * at the end of a path).
 */
static TC_STRUCT* tpGetNextTC(TP_STRUCT* tp, TC_STRUCT* tc,int stepping){
    TC_STRUCT* nexttc = NULL;

    if(!stepping && tc->term_cond) 
        nexttc = tcqItem(&tp->queue, 1);
    else
        nexttc = NULL;

    int this_synch_pos = tc->synchronized && !tc->velocity_mode;
    int next_synch_pos = nexttc && nexttc->synchronized && !nexttc->velocity_mode;
    if(!this_synch_pos && next_synch_pos) {
        // we'll have to wait for spindle sync; might as well
        // stop at the right place (don't blend)
        tc->term_cond = TC_TERM_COND_STOP;
        nexttc = NULL;
    }

    if(nexttc && nexttc->atspeed) {
        // we'll have to wait for the spindle to be at-speed; might as well
        // stop at the right place (don't blend), like above
        // FIXME change the values so that 0 is exact stop mode
        tc->term_cond = 0;
        nexttc = NULL;
    }

    //TODO rest of nexttc here

    return nexttc;
}


/**
 * "Activate" a segment being read for the first time.
 * This function handles initial setup of a new segment read off of the queue
 * for the first time.
 */
static int tpActivateSegment(TP_STRUCT* tp, TC_STRUCT* tc, emcmot_status_t* emcmotStatus,
        tp_spindle_status_t* status ){

    // wait for atspeed, if motion requested it.  also, force
    // atspeed check for the start of all spindle synchronized
    // moves.
    bool needs_atspeed = tc->atspeed || 
        (tc->synchronized && !(tc->velocity_mode) && !(emcmotStatus->spindleSync));
    if( needs_atspeed && !(emcmotStatus->spindle_is_atspeed)) {
        status->waiting_for_atspeed = tc->id;
        return 0;
    }

    if (tc->indexrotary != -1) {
        // request that the axis unlock
        tpSetRotaryUnlock(tc->indexrotary, 1);
        // if it is unlocked, fall through and start the move.
        // otherwise, just come back later and check again
        if (!tpGetRotaryIsUnlocked(tc->indexrotary))
            return 0;
    }

    // Temporary debug message
    rtapi_print("Activate tc id %d\n",tc->id);

    tc->active = 1;
    tc->currentvel = 0;
    //FIXME activedepth might change meaning with lookahead?
    tp->depth = tp->activeDepth = 1;
    tp->motionType = tc->canon_motion_type;
    tc->blending = 0;

    // honor accel constraint in case we happen to make an acute angle
    // with the next segment.
    // TODO better acceleration constraints?
    if(tc->term_cond == TC_TERM_COND_BLEND) 
        tc->maxaccel /= 2.0;

    if(tc->synchronized) {
        if(!tc->velocity_mode && !(emcmotStatus->spindleSync)) {
            // if we aren't already synced, wait
            status->waiting_for_index = tc->id;
            // ask for an index reset
            emcmotStatus->spindle_index_enable = 1;
            status->offset = 0.0;
            rtapi_print("Waiting on sync...\n");
            // don't move: wait
            return 0;
        }
    }

    //Keep going:
    return 1;
}


static void tcSyncVelocityMode(TC_STRUCT* tc, TC_STRUCT* nexttc, double speed) {
    //NOTE: check for aborting outside of here

    double pos_error = fabs(speed) * tc->uu_per_rev;
    //Take into account blending?
    if(nexttc) pos_error -= nexttc->progress; /* ?? */
    tc->reqvel = pos_error;
}


static void tcSyncPositionMode(TC_STRUCT* tc, TC_STRUCT* nexttc,
        tp_spindle_status_t* status, double spindle_pos) {

    double spindle_vel, target_vel;
    double oldrevs = status->revs;

    /*double new_spindlepos = emcmotStatus->spindleRevs;*/

    if(tc->motion_type == TC_RIGIDTAP && 
            (tc->coords.rigidtap.state == RETRACTION || 
             tc->coords.rigidtap.state == FINAL_REVERSAL))
        status->revs = tc->coords.rigidtap.spindlerevs_at_reversal - 
            spindle_pos;
    else
        status->revs = spindle_pos;

    double pos_error = (status->revs - status->offset) * tc->uu_per_rev - tc->progress;

    if(nexttc) pos_error -= nexttc->progress;

    if(tc->sync_accel) {
        // detect when velocities match, and move the target accordingly.
        // acceleration will abruptly stop and we will be on our new target.
        spindle_vel = status->revs/(tc->cycle_time * tc->sync_accel++);
        target_vel = spindle_vel * tc->uu_per_rev;
        if(tc->currentvel >= target_vel) {
            // move target so as to drive pos_error to 0 next cycle
            status->offset = status->revs - tc->progress/tc->uu_per_rev;
            tc->sync_accel = 0;
            tc->reqvel = target_vel;
        } else {
            // beginning of move and we are behind: accel as fast as we can
            tc->reqvel = tc->maxvel;
        }
    } else {
        // we have synced the beginning of the move as best we can -
        // track position (minimize pos_error).
        double errorvel;
        spindle_vel = (status->revs - oldrevs) / tc->cycle_time;
        target_vel = spindle_vel * tc->uu_per_rev;
        errorvel = pmSqrt(fabs(pos_error) * tc->maxaccel);
        if(pos_error<0) errorvel = -errorvel;
        tc->reqvel = target_vel + errorvel;
    }
}

/**
 * Calculate an updated goal position for the next timestep.
 * This is the brains of the operation. It's called every TRAJ period and is
 * expected to set tp->currentPos to the new machine position. Lots of other
 * tp fields (depth, done, etc) have to be twiddled to communicate the status;
 * I think those are spelled out here correctly and I can't clean it up
 * without breaking the API that the TP presents to motion. It's not THAT bad
 Found next the interest of not touching stuff outside this directory, I'm going
 * to leave it for now.
 */
int tpRunCycle(TP_STRUCT * tp, long period)
{
    // vel = (new position - old position) / cycle time
    // (two position points required)
    //
    // acc = (new vel - old vel) / cycle time
    // (three position points required)

    //Pointer to next trajectory component
    TC_STRUCT *tc, *nexttc;

    double primary_vel;
    int on_final_decel;

    //Pose data used for intermediate calculations
    EmcPose primary_before, primary_displacement;
    EmcPose secondary_before, secondary_displacement;


    // Persistant spindle data for spindle-synchronized motion
    static tp_spindle_status_t spindle_status = {0.0,MOTION_INVALID_ID,
        MOTION_INVALID_ID,0.0};

    //Update motion status 
    emcmotStatus->tcqlen = tcqLen(&tp->queue);
    //FIXME why is this zero?
    emcmotStatus->requested_vel = 0.0;
    //Define TC as the "first" element in the queue
    //FIXME remove period field from this function
    tc = tcqItem(&tp->queue, 0);

    if(!tc) {
        tpHandleEmptyQueue(tp,emcmotStatus);
        return 0;
    }

    if (tc->target == tc->progress && spindle_status.waiting_for_atspeed != tc->id) {
        //If we can't get a valid tc (end of move, waiting on spindle), we're done for now.
        tc = tpCompleteSegment(tp, tc, &spindle_status);
        if (!tc)  return 0;
    }

    nexttc = tpGetNextTC(tp, tc, emcmotDebug->stepping);

    if(tp->aborting) {
        bool ready = tpHandleAbort(tp, tc, nexttc, &spindle_status);
        if (!ready){
            return 0;

            rtapi_print("  Early stop at tpHandleAbort?\n");
        }
    }

    int tc_wait = tpCheckWaiting(tc, &spindle_status);
    if (tc_wait) return 0;

    // FIXME: roll this into tpCheckWaiting and return an error code?
    // check for at-speed before marking the tc active
    if (MOTION_ID_VALID(spindle_status.waiting_for_atspeed)) {
        if(!emcmotStatus->spindle_is_atspeed) {
            /* spindle is still not at the right speed: wait */
            debug_ping();
            return 0;
        } else {
            spindle_status.waiting_for_atspeed = MOTION_INVALID_ID;
        }
    }

    //TODO refactor
    if(tc->active == 0) {
        bool ready = tpActivateSegment(tp, tc, emcmotStatus, &spindle_status);
        // Need to wait to continue motion, end planning here
        if (!ready) {
            debug_ping();
            return 0;
        }
    }

    if (MOTION_ID_VALID(spindle_status.waiting_for_index)) {
        if(emcmotStatus->spindle_index_enable) {
            /* haven't passed index yet */
            return 0;
        } else {
            /* passed index, start the move */
            emcmotStatus->spindleSync = 1;
            spindle_status.waiting_for_index = MOTION_INVALID_ID;
            tc->sync_accel=1;
            spindle_status.revs=0;
        }
    }

    if (tc->motion_type == TC_RIGIDTAP) tpHandleRigidTap(emcmotStatus, tc, &spindle_status);

    if(!tc->synchronized) emcmotStatus->spindleSync = 0;

    if(nexttc && nexttc->active == 0) {
        // this means this tc is being read for the first time.
        rtapi_print("Activate nexttc id %d\n",nexttc->id);
        nexttc->currentvel = 0;
        tp->depth = tp->activeDepth = 1;
        nexttc->active = 1;
        nexttc->blending = 0;

        // honor accel constraint if we happen to make an acute angle with the
        // above segment or the following one
        // TODO: replace this with better acceleration constraint
        if(tc->term_cond == TC_TERM_COND_BLEND || nexttc->term_cond == TC_TERM_COND_BLEND)
            nexttc->maxaccel /= 2.0;
    }


    /** If synchronized with spindle, calculate requested velocity to track spindle motion.*/
    if(tc->synchronized) {

        //Update requested velocity based on tracking error and sync mode
        if(tc->velocity_mode) {
            tcSyncVelocityMode(tc, nexttc, emcmotStatus->spindleSpeedIn);
            tc->feed_override = emcmotStatus->net_feed_scale;
        } else {
            //Find signed spindle position
            double spindle_pos = emcmotStatus->spindleRevs;
            if (emcmotStatus->spindle.direction < 0.0) spindle_pos*=-1.0;

            tcSyncPositionMode(tc, nexttc, &spindle_status, spindle_pos);

            tc->feed_override = 1.0;
        }
        if(tc->reqvel < 0.0) tc->reqvel = 0.0;

        if(nexttc) {
            if (nexttc->synchronized) {
                //If the next move is synchronized too, then match it's
                //requested velocity to the current move
                nexttc->reqvel = tc->reqvel;
                nexttc->feed_override = 1.0;
                if(nexttc->reqvel < 0.0) nexttc->reqvel = 0.0;
            } else {
                nexttc->feed_override = emcmotStatus->net_feed_scale;
            }
        }
    } 
    else {
        tc->feed_override = emcmotStatus->net_feed_scale;
        if(nexttc) nexttc->feed_override = emcmotStatus->net_feed_scale;
    }

    /* handle pausing */
    if(tp->pausing && (!tc->synchronized || tc->velocity_mode)) {
        tc->feed_override = 0.0;
        if(nexttc) {
            nexttc->feed_override = 0.0;
        }
    }

    if (tc->term_cond == TC_TERM_COND_BLEND){
        tc->blend_vel = tpComputeBlendVelocity(tc, nexttc);
    }

    primary_before = tcGetPos(tc);

    // Update the current tc
    tcRunCycle(tp, tc, &primary_vel, &on_final_decel);
    //If we've overshot the target, then fold this into the next move
    if (tc->term_cond == TC_TERM_COND_TANGENT) tpCheckOvershoot(tc,nexttc,&secondary_before);

    //Update 
    tpFindDisplacement(tc,primary_before,&primary_displacement);

    // Update the trajectory planner position based on the results

    /* BLENDING STUFF */
    // make sure we continue to blend this segment even when its 
    // accel reaches 0 (at the very end)
    // TODO make sure these conditions accept tangent "blends" as well
    bool is_blend_start = (tc->term_cond == TC_TERM_COND_BLEND ) && nexttc && on_final_decel && (primary_vel < tc->blend_vel);

    bool is_tangent_blend_start = (tc->term_cond == TC_TERM_COND_TANGENT ) && nexttc && tc->target==tc->progress;

    if (is_blend_start) tc->blending = 1;

    tpUpdatePosition(tp,&primary_displacement);
    if(tc->blending && nexttc ) {
        // hack to show blends in axis
        tp->motionType = 0;

        secondary_before = tcGetPos(nexttc);

        tpDoParabolicBlend(tp, tc, nexttc, primary_vel);
        tpFindDisplacement(nexttc,secondary_before,&secondary_displacement);

        //Add in contributions from both segments
        tpUpdatePosition(tp,&secondary_displacement);

        if(tc->currentvel > nexttc->currentvel) tpUpdateMovementStatus(tp, tc, emcmotStatus);
        else {
            tpToggleDIOs(nexttc);
            tpUpdateMovementStatus(tp, nexttc, emcmotStatus);
        }

        //Update velocity status based on both tc and nexttc
        emcmotStatus->current_vel = tc->currentvel + nexttc->currentvel;

    } 
    else {

        if (is_tangent_blend_start){
            rtapi_print("Found Tangency at %d, T-P of tc is %f at_endpt = %d\n",
                    tc->id,tc->target - tc->progress, tc->target == tc->progress);

            tpFindDisplacement(nexttc,secondary_before,&secondary_displacement);
            tpUpdatePosition(tp,&secondary_displacement);
        }
        tpToggleDIOs(tc); //check and do DIO changes
        tpUpdateMovementStatus(tp, tc, emcmotStatus);
    }

    return 0;
}

int tpSetSpindleSync(TP_STRUCT * tp, double sync, int mode) {
    if(sync) {
        tp->synchronized = 1;
        tp->uu_per_rev = sync;
        tp->velocity_mode = mode;
    } else
        tp->synchronized = 0;

    return 0;
}

int tpPause(TP_STRUCT * tp)
{
    if (0 == tp) {
        return -1;
    }
    tp->pausing = 1;
    return 0;
}

int tpResume(TP_STRUCT * tp)
{
    if (0 == tp) {
        return -1;
    }
    tp->pausing = 0;
    return 0;
}

int tpAbort(TP_STRUCT * tp)
{
    if (0 == tp) {
        return -1;
    }

    if (!tp->aborting) {
        /* to abort, signal a pause and set our abort flag */
        tpPause(tp);
        tp->aborting = 1;
    }
    return tpClearDIOs(); //clears out any already cached DIOs
}

int tpGetMotionType(TP_STRUCT * tp)
{
    return tp->motionType;
}

EmcPose tpGetPos(TP_STRUCT * tp)
{
    EmcPose retval;

    if (0 == tp) {
        ZERO_EMC_POSE(retval);
        return retval;
    }

    return tp->currentPos;
}

int tpIsDone(TP_STRUCT * tp)
{
    if (0 == tp) {
        return 0;
    }

    return tp->done;
}

int tpQueueDepth(TP_STRUCT * tp)
{
    if (0 == tp) {
        return 0;
    }

    return tp->depth;
}

int tpActiveDepth(TP_STRUCT * tp)
{
    if (0 == tp) {
        return 0;
    }

    return tp->activeDepth;
}

int tpSetAout(TP_STRUCT *tp, unsigned char index, double start, double end) {
    if (0 == tp) {
        return -1;
    }
    syncdio.anychanged = 1; //something has changed
    syncdio.aio_mask |= (1 << index);
    syncdio.aios[index] = start;
    return 0;
}

int tpSetDout(TP_STRUCT *tp, int index, unsigned char start, unsigned char end) {
    if (0 == tp) {
        return -1;
    }
    syncdio.anychanged = 1; //something has changed
    syncdio.dio_mask |= (1 << index);
    if (start > 0)
        syncdio.dios[index] = 1; // the end value can't be set from canon currently, and has the same value as start
    else 
        syncdio.dios[index] = -1;
    return 0;    
}

// vim:sw=4:sts=4:et:
