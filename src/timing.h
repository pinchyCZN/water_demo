/*
 * timing.h - the clock and the frame pacer.
 *
 * WHAT THE ORIGINAL DID, so the divergence is deliberate rather than
 * accidental. Frame_LimitAndMeasure (0x00459fd0) is the whole of the
 * original's pacing, and it is three facts [verified]:
 *
 *   1  IT TARGETS 30 FPS. g_frameRateMax (0x004dc200) holds 30 in the
 *      image, and -FrameRateMax<XX> overrides it. The help text says
 *      "20 to 99, default=30, 0=no limit"; the parser actually stores
 *      only when 20 < n < 99, so 20 and 99 themselves are ignored.
 *
 *   2  IT BUSY-WAITS. There is no Sleep anywhere in MOTO.EXE. The
 *      limiter spins on timeGetTime until the budget is up, burning a
 *      core for whatever is left of the frame.
 *
 *   3  ITS CLOCK IS timeGetTime AND NOTHING ELSE. 75 references, and
 *      no QueryPerformanceCounter, no GetTickCount, no timeBeginPeriod
 *      - so the game's own clock ticks at the default ~15.6 ms. At a
 *      33 ms budget that is half a tick of jitter, which is survivable
 *      at 30 fps and is not survivable at 144.
 *
 * NONE OF THAT IS REPRODUCED. This module is a modernization, and
 * CLAUDE.md rule 5 is about the reconstruction matching observed
 * behaviour - not about inheriting a 1997 spin loop. What IS kept is
 * the shape: a cap expressed in frames per second, 0 meaning uncapped,
 * and a rolling average over the last 24 frames, because that is the
 * original's own ring size.
 *
 * See docs/subsystems/frame-loop.txt.
 */

#ifndef MRACER_TIMING_H
#define MRACER_TIMING_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * tm_init - start the clock. Call once, early.
 *
 * Establishes the QueryPerformanceCounter epoch, tries to create a
 * high-resolution waitable timer, and raises the system timer
 * resolution as a fallback. Safe to call when the high-resolution
 * timer is unavailable - tm_wait_until degrades rather than failing.
 */
void tm_init(void);
void tm_shutdown(void);

/*
 * tm_now - monotonic VIRTUAL seconds. The clock the game sees.
 *
 * Resolution is under a microsecond and it does not wrap in any
 * plausible session; timeGetTime's 15.6 ms quantum is exactly what
 * made the spinning logo judder even though every frame was being
 * presented on time.
 *
 * VIRTUAL, not real. It advances only when frame_begin advances it,
 * and while the debug pause is on it does not advance at all. That is
 * the whole mechanism - see "The pause" below.
 */
double tm_now(void);

/*
 * tm_real_now - monotonic REAL seconds since tm_init. Never pauses.
 *
 * For anything measuring the machine rather than the game: the pacer's
 * own deadline arithmetic, the frame-rate counter, profiling. Game
 * code wants tm_now.
 */
double tm_real_now(void);


/*
 * ---------------------------------------------------------------
 * The pause, and single-stepping.
 * ---------------------------------------------------------------
 *
 * THE PROBLEM THIS SOLVES, because a pause flag does not solve it.
 *
 * Game code reads the wall clock directly - page14_tick calls tm_now
 * and diffs it against the time its enter handler stamped, exactly as
 * MenuPage14_Title_Tick diffs timeGetTime. So simply not calling the
 * game for a while does not pause anything: the moment it is called
 * again it sees every second that passed while it was skipped, and
 * jumps forward to wherever the animation would have got to. Skipping
 * the update is not pausing, it is deferring.
 *
 * So the clock pauses, not the caller. tm_now stops advancing, every
 * subsystem that reads it freezes for free, and none of them has to
 * know that a pause exists. Real time stays available through
 * tm_real_now for the pacer and the fps counter, which must keep
 * running - a paused game still presents frames.
 *
 * Nothing else changes: the message pump, drawing and presenting all
 * continue, so the window keeps repainting and input keeps working
 * without a special case. The last frame stays on screen because the
 * game keeps redrawing it, identically, from a clock that is not
 * moving.
 *
 * WHAT DOES NOT FREEZE: anything that reads tm_real_now, and anything
 * driven by frame count rather than time. If something animates while
 * paused, it is reading the wrong clock - that is a bug in the caller,
 * and this is a decent way to find it.
 */

/* Nonzero while the virtual clock is frozen. */
int  tm_paused(void);
void tm_set_paused(int paused);
void tm_pause_toggle(void);

/*
 * tm_step_frames - advance the virtual clock by n frames, then hold.
 *
 * PAUSES FIRST IF RUNNING, so one key does both jobs: from a running
 * game it pauses and shows the next frame, and from a paused game it
 * shows the next frame. The caller never has to know which state it
 * was in.
 *
 * Requests accumulate, so holding the key down and letting the OS
 * repeat rate drive it plays the game in slow motion at whatever rate
 * the keyboard repeats. Each frame consumes at most one request.
 *
 * A "frame" is one period of the frame cap, or one simulation step
 * when uncapped. It is a fixed amount deliberately: stepping by the
 * real elapsed time would make every step a different size, and a
 * single-step that is not reproducible is not much of a debugging
 * tool.
 */
void tm_step_frames(int frames);
int  tm_steps_pending(void);

/*
 * tm_set_speed - run the virtual clock faster (or slower) than real
 * time. 1.0 is normal; the harness uses it for hold-to-fast-forward.
 *
 * It scales what frame_begin hands the game, so EVERYTHING driven by
 * tm_now moves together - menu animations, the page 25 intro, the
 * glass shatter. Nothing on tm_real_now is affected, which is the
 * point: the frame pacer keeps presenting at the true rate and the
 * fps counter keeps telling the truth, exactly as they do while
 * paused.
 *
 * o IT DOES NOTHING WHILE PAUSED. Speed applies to the running
 *   branch of clock_advance; a pending step is a fixed amount, for
 *   the reason tm_step_frames gives.
 *
 * o LARGE MULTIPLIERS MAKE LARGE dt, and that is the real limit. At
 *   8x a 16 ms frame hands the game 128 ms at once, so anything
 *   integrating per frame rather than per dt drifts. The menus are
 *   all dt-driven so they are safe; gameplay may not be, and the
 *   honest fix there is several normal-sized ticks per frame rather
 *   than one big one - frame_next_step already exists for it.
 *
 * o THE GAME'S OWN CLAMP IS NOT IN THE WAY. MenuPage14_Title_Tick
 *   replaces any dt above 500 ms with 100 ms [verified]. At 60 fps
 *   the multiplier would have to pass 30x before a frame crossed
 *   that, so it never bites in the range this is used for.
 *
 * Clamped to a sane range; 0 or negative is ignored.
 */
void   tm_set_speed(double speed);
double tm_speed(void);

/*
 * tm_wait_until - block until tm_now() reaches deadline.
 *
 * BETTER THAN Sleep(1), AND NOT A DirectX FACILITY - D3D9 has no
 * wait primitive beyond Present's vsync block. It is a WAITABLE
 * TIMER created with
 * CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, which Windows 10 1803 added
 * precisely for this. It gives roughly 0.5 ms accuracy against
 * Sleep(1)'s 1-15.6 ms, and it genuinely sleeps: the thread is off the
 * CPU, not spinning.
 *
 * The last fraction of a millisecond is still finished by a spin,
 * because no OS wait is exact. That spin is bounded by
 * TM_SPIN_MARGIN below - typically well under a millisecond of burn
 * per frame rather than the whole remainder, which is the difference
 * between this and what the original does.
 *
 * Returns immediately if the deadline has already passed.
 */
void tm_wait_until(double deadline);

/*
 * Nonzero when a high-resolution waitable timer was obtained. Worth
 * printing once at startup: on a machine without it the pacer still
 * works, but the spin does more of the work.
 */
int tm_have_highres_timer(void);

/*
 * tm_wait_margin - the spin margin actually in use, in seconds.
 *
 * With the high-resolution timer this is a constant 1.2 ms. Without
 * it, tm_init MEASURES how far a 1 ms wait really overshoots on this
 * machine and sizes the margin from that, rather than trusting a
 * number somebody wrote down.
 *
 * THE FALLBACK STILL REACHES 60. A cap is met by sleeping whenever
 * the budget comfortably exceeds this margin, and 16.7 ms against
 * 2-3 ms leaves about 14 ms of genuine sleep - so the fallback costs
 * a longer spin, not a lower frame rate. The cap only becomes
 * unreachable once the margin approaches the budget, which printing
 * this makes visible rather than leaving it to be inferred from a
 * disappointing fps counter.
 */
double tm_wait_margin(void);


/*
 * ---------------------------------------------------------------
 * The frame pacer.
 * ---------------------------------------------------------------
 *
 * Usage, once per frame:
 *
 *     dt = frame_begin(&pacer);
 *     while (frame_next_step(&pacer))
 *         update(pacer.fixedStep);        // fixed-rate simulation
 *     render(dt, frame_alpha(&pacer));    // as fast as the cap allows
 *     frame_end(&pacer);                  // wait out the budget
 *
 * THAT IS THE INTENT AND NOT YET THE PRACTICE. main.cpp runs the
 * shape above but its update step is empty - the race ticks from
 * page01_tick once per RENDERED frame, off the real frame delta,
 * not off pacer.fixedStep. frame_alpha has no callers at all.
 *
 * So the render cap is what holds the physics step steady today.
 * Read this block as the destination, not the current state, and
 * see frame-loop.txt 9.
 */

#define FRAME_RING 24       /* the original's ring size, 0x005809f8 */

/*
 * FIXED-TIMESTEP SIMULATION, VARIABLE-RATE RENDER.
 *
 * The original ties both to one 30 fps loop, so its physics step IS
 * its frame. Raising that loop to 144 would silently change the
 * simulation - a bike's behaviour would depend on the frame rate,
 * which is the classic way a port of a game this age breaks. Running
 * the simulation at a fixed rate and the renderer as fast as it likes
 * keeps the handling identical at any display rate.
 *
 * The step defaults to the original's own 30 Hz. Nothing forces that
 * once the physics is understood well enough to know it is stable at
 * other rates, but 30 is what the code was written against.
 */
typedef struct MR_FramePacer {
	double targetFps;               /* 0 = uncapped                  */
	double fixedStep;               /* seconds per simulation step   */
	double frameStart;              /* tm_now() at frame_begin       */
	double prevFrameStart;
	double dt;                      /* real seconds since last frame */
	double accumulator;             /* unconsumed simulation time    */
	double ring[FRAME_RING];        /* recent frame durations        */
	int    ringCount;
	int    ringHead;
	int    stepsThisFrame;
	int    started;
} MR_FramePacer;

/*
 * targetFps 0 means uncapped. fixedStepHz 0 means "use 30".
 */
void frame_init(MR_FramePacer *p, double targetFps, double fixedStepHz);
void frame_set_target(MR_FramePacer *p, double targetFps);

/*
 * frame_begin - stamp the frame and return the VIRTUAL elapsed seconds.
 *
 * This is also where the virtual clock advances, and it is the only
 * place that advances it. Normally the return is the real elapsed
 * time; while paused it is 0, or one frame's worth when a step has
 * been requested.
 *
 * The returned dt is CLAMPED. A frame that took longer than
 * FRAME_MAX_DT - because the window was dragged, or a texture load
 * blocked - would otherwise inject a huge step into the simulation and
 * teleport everything. Clamping makes the game run slow for one frame
 * instead, which is the right failure. Note this makes the pause a
 * degenerate case of a clamp that was needed anyway.
 *
 * The frame stamp kept for pacing is REAL time, so frame_end still
 * waits out a real budget while paused and the loop does not spin.
 */
double frame_begin(MR_FramePacer *p);

/*
 * frame_next_step - the fixed-timestep pump. Returns nonzero while
 * there is accumulated time left to simulate.
 *
 * Bounded by FRAME_MAX_STEPS so that a machine too slow to keep up
 * falls behind in real time rather than spiralling: each frame taking
 * longer would otherwise demand more steps, which take longer still.
 */
int frame_next_step(MR_FramePacer *p);

/*
 * frame_alpha - how far between simulation steps the render is, 0..1.
 *
 * For interpolating drawn positions between the last two simulation
 * states. Nothing uses it yet; it is here because a renderer running
 * faster than the simulation needs it to look smooth, and retrofitting
 * it later means touching every drawable.
 */
double frame_alpha(const MR_FramePacer *p);

/*
 * frame_end - wait out the remainder of the frame budget.
 *
 * A no-op when uncapped, or when the frame already overran, or when
 * vsync has already blocked in Present for longer than the budget.
 */
void frame_end(MR_FramePacer *p);

/* Rolling average over the last FRAME_RING frames. 0 before the first. */
double frame_fps(const MR_FramePacer *p);

#ifdef __cplusplus
}
#endif

#endif /* MRACER_TIMING_H */
