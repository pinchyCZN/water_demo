/*
 * timing.c - see timing.h.
 */

#include <windows.h>
#include <mmsystem.h>
#include <string.h>

#include "timing.h"

/*
 * Windows 10 1803 added this flag. The pinned SDK may predate it, and
 * defining it here costs nothing: on an older OS the call fails with
 * ERROR_INVALID_PARAMETER and we fall through to the plain timer,
 * which is exactly the behaviour we want anyway.
 */
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#ifndef TIMER_ALL_ACCESS
#define TIMER_ALL_ACCESS 0x1F0003
#endif

/*
 * How early to stop waiting and start spinning. No OS wait is exact;
 * a high-resolution waitable timer lands within roughly half a
 * millisecond, so leaving a little more than that and finishing by
 * hand costs a very short burn and removes the overshoot.
 *
 * Without the high-resolution timer the margin has to cover Sleep's
 * whole quantum, so it goes up and the spin does more work. That is
 * the degraded path, not the normal one.
 */
#define TM_SPIN_MARGIN_HIRES  0.0012    /* 1.2 ms */
#define TM_SPIN_MARGIN_FLOOR  0.0010    /* never go below this  */
#define TM_SPIN_MARGIN_CEIL   0.0200    /* nor above; see below */

#define FRAME_MAX_DT     0.25           /* clamp, seconds            */
#define FRAME_MAX_STEPS  8              /* anti-spiral bound         */

static LARGE_INTEGER s_freq;
static LARGE_INTEGER s_epoch;
static HANDLE        s_timer;
static int           s_hires;
static int           s_periodRaised;
static int           s_ready;
static double        s_margin;

/* The virtual clock. s_vnow is what tm_now returns. */
static double        s_vnow;
static int           s_paused;
static double        s_speed = 1.0;
static int           s_stepFrames;

void tm_init(void)
{
	if (s_ready)
		return;

	QueryPerformanceFrequency(&s_freq);
	QueryPerformanceCounter(&s_epoch);

	/*
	 * Ask for the high-resolution timer first. If the OS is too old
	 * the flag is rejected, so try again without it - a plain waitable
	 * timer is still better than Sleep, because it takes an absolute
	 * deadline and does not round to the scheduler quantum as coarsely.
	 */
	s_timer = CreateWaitableTimerExW(NULL, NULL,
	                                 CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
	                                 TIMER_ALL_ACCESS);
	if (s_timer != NULL) {
		s_hires = 1;
	} else {
		s_timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
		s_hires = 0;
	}

	/*
	 * Raise the system timer resolution regardless. It bounds how long
	 * an unlucky wait can overshoot even with the high-resolution
	 * timer, and it is what makes the fallback path tolerable. Paired
	 * with timeEndPeriod in tm_shutdown - leaving it raised is
	 * antisocial, it costs battery on every process on the machine.
	 */
	if (timeBeginPeriod(1) == TIMERR_NOERROR)
		s_periodRaised = 1;

	s_ready = 1;

	/*
	 * MEASURE THE WAIT GRANULARITY RATHER THAN ASSUMING IT.
	 *
	 * The margin has to cover how far a wait actually overshoots, and
	 * that is a property of the machine, not of a constant somebody
	 * wrote down. Ask for 1 ms a few times and see what comes back.
	 *
	 * Worth doing because the failure is silent and asymmetric: too
	 * large a margin only wastes a little CPU in the spin, but too
	 * small a margin means every wait overshoots the deadline and the
	 * cap is quietly missed - and it would be missed by exactly the
	 * amount that makes 60 fps land somewhere near 64, which looks
	 * like a rounding bug rather than a pacing one.
	 */
	if (s_hires) {
		s_margin = TM_SPIN_MARGIN_HIRES;
	} else {
		double worst = 0.0;
		int    i;
		for (i = 0; i < 4; i++) {
			double t0 = tm_real_now();
			double slept;
			if (s_timer != NULL) {
				LARGE_INTEGER due;
				due.QuadPart = -10000;          /* 1 ms, relative */
				if (SetWaitableTimer(s_timer, &due, 0, NULL, NULL, FALSE))
					WaitForSingleObject(s_timer, INFINITE);
				else
					Sleep(1);
			} else {
				Sleep(1);
			}
			slept = tm_real_now() - t0;
			if (slept > worst)
				worst = slept;
		}
		/* Half again, for headroom against a less lucky wait later. */
		s_margin = worst * 1.5;
		if (s_margin < TM_SPIN_MARGIN_FLOOR) s_margin = TM_SPIN_MARGIN_FLOOR;
		if (s_margin > TM_SPIN_MARGIN_CEIL)  s_margin = TM_SPIN_MARGIN_CEIL;
	}
}

/*
 * The measured spin margin, in seconds. A frame budget has to be
 * comfortably larger than this for the cap to be met by sleeping
 * rather than by spinning - at 60 fps the budget is 16.7 ms, so even a
 * 3 ms margin leaves 13.7 ms of real sleep.
 */
double tm_wait_margin(void)
{
	return s_margin;
}

void tm_shutdown(void)
{
	if (!s_ready)
		return;
	if (s_timer) {
		CloseHandle(s_timer);
		s_timer = NULL;
	}
	if (s_periodRaised) {
		timeEndPeriod(1);
		s_periodRaised = 0;
	}
	s_hires = 0;
	s_ready = 0;
}

int tm_have_highres_timer(void)
{
	return s_hires;
}

double tm_real_now(void)
{
	LARGE_INTEGER now;

	if (!s_ready)
		return 0.0;
	QueryPerformanceCounter(&now);
	return (double)(now.QuadPart - s_epoch.QuadPart) / (double)s_freq.QuadPart;
}

/*
 * The virtual clock. See the long note in timing.h: the reason this
 * exists rather than a g_paused flag is that game code reads the
 * clock itself, so a flag would have to be checked by every reader,
 * and the one reader that forgot would jump forward by the whole
 * pause the moment it resumed.
 */
double tm_now(void)
{
	return s_vnow;
}

int tm_paused(void)
{
	return s_paused;
}

void tm_set_paused(int paused)
{
	s_paused = paused ? 1 : 0;
	if (!s_paused)
		s_stepFrames = 0;       /* resuming discards a queued step */
}

void tm_pause_toggle(void)
{
	tm_set_paused(!s_paused);
}

void tm_step_frames(int frames)
{
	if (frames <= 0)
		return;
	s_paused = 1;               /* stepping implies paused */
	s_stepFrames += frames;
}

void tm_set_speed(double speed)
{
	if (speed <= 0.0)
		return;
	if (speed > 64.0)
		speed = 64.0;
	s_speed = speed;
}

double tm_speed(void)
{
	return s_speed;
}

int tm_steps_pending(void)
{
	return s_stepFrames;
}

/*
 * Advance the virtual clock by one frame's worth. Called by
 * frame_begin and nowhere else; the amount it returns is what the
 * frame is entitled to treat as elapsed.
 *
 * A pending step wins over the pause, and is consumed one per frame -
 * that is what makes holding the step key produce slow motion at the
 * keyboard repeat rate rather than one enormous jump.
 */
static double clock_advance(double realDt, double stepSeconds)
{
	double advance;

	if (!s_paused) {
		/* The only place speed applies. A step is deliberately a fixed
		 * amount whatever the speed - see tm_set_speed. */
		advance = realDt * s_speed;
	} else if (s_stepFrames > 0) {
		s_stepFrames--;
		advance = stepSeconds;
	} else {
		advance = 0.0;
	}

	s_vnow += advance;
	return advance;
}

void tm_wait_until(double deadline)
{
	double margin;
	double remaining;

	if (!s_ready)
		return;

	margin = s_margin;

	/*
	 * Stage 1: give the remainder back to the OS, less the margin.
	 *
	 * A negative SetWaitableTimer due time is RELATIVE, in 100 ns
	 * units. Relative rather than absolute deliberately: an absolute
	 * deadline would be in FILETIME terms and would have to be
	 * re-derived from a different clock than the one measuring the
	 * frame, which is a needless source of skew.
	 */
	remaining = deadline - tm_real_now() - margin;
	if (remaining > 0.0) {
		if (s_timer != NULL) {
			LARGE_INTEGER due;
			due.QuadPart = -(LONGLONG)(remaining * 10000000.0);
			if (SetWaitableTimer(s_timer, &due, 0, NULL, NULL, FALSE))
				WaitForSingleObject(s_timer, INFINITE);
			else
				Sleep((DWORD)(remaining * 1000.0));
		} else {
			Sleep((DWORD)(remaining * 1000.0));
		}
	}

	/*
	 * Stage 2: finish by hand.
	 *
	 * YieldProcessor is the PAUSE instruction - it hints to the core
	 * that this is a spin-wait, which lets a hyperthreaded sibling
	 * have the pipeline and drops the power draw considerably. It is
	 * still a burn, but it is a burn of well under a millisecond,
	 * versus the original's burn of the entire remaining frame.
	 */
	while (tm_real_now() < deadline)
		YieldProcessor();
}


/* ------------------------------------------------------------------ */

void frame_init(MR_FramePacer *p, double targetFps, double fixedStepHz)
{
	if (!p)
		return;
	memset(p, 0, sizeof(*p));
	p->targetFps = (targetFps > 0.0) ? targetFps : 0.0;
	p->fixedStep = 1.0 / ((fixedStepHz > 0.0) ? fixedStepHz : 30.0);
}

void frame_set_target(MR_FramePacer *p, double targetFps)
{
	if (p)
		p->targetFps = (targetFps > 0.0) ? targetFps : 0.0;
}

double frame_begin(MR_FramePacer *p)
{
	double now;
	double realDt;
	double step;

	if (!p)
		return 0.0;

	/*
	 * REAL time for the frame stamp. frame_end waits against it, so it
	 * has to keep moving while the virtual clock is frozen - otherwise
	 * a paused frame would have a deadline in the past and the loop
	 * would spin at whatever rate the machine can redraw.
	 */
	now = tm_real_now();

	if (!p->started) {
		/* The first frame has no predecessor; give it one step's worth
		 * so nothing sees a zero or a garbage dt. */
		p->started        = 1;
		p->prevFrameStart = now - p->fixedStep;
		realDt            = p->fixedStep;
	} else {
		p->prevFrameStart = p->frameStart;
		realDt            = now - p->prevFrameStart;
		if (realDt > FRAME_MAX_DT)
			realDt = FRAME_MAX_DT;
		if (realDt < 0.0)
			realDt = 0.0;
	}
	p->frameStart = now;

	/* The rolling average, over the same 24 frames the original keeps.
	 * Stored as a ring with a head rather than shifted down a slot per
	 * frame the way Frame_LimitAndMeasure does it - same result, and
	 * 24 memory moves a frame was never the point of that code.
	 *
	 * REAL dt, so the counter keeps reading the true present rate
	 * while paused instead of dividing by a column of zeroes. */
	p->ring[p->ringHead] = realDt;
	p->ringHead = (p->ringHead + 1) % FRAME_RING;
	if (p->ringCount < FRAME_RING)
		p->ringCount++;

	/*
	 * And now the virtual clock, which is what the game gets. One
	 * frame of a step is the cap period, or the simulation step when
	 * uncapped - a fixed amount either way, because a single-step that
	 * is a different size every time is not reproducible enough to
	 * debug with.
	 */
	step   = (p->targetFps > 0.0) ? (1.0 / p->targetFps) : p->fixedStep;
	p->dt  = clock_advance(realDt, step);

	p->accumulator   += p->dt;
	p->stepsThisFrame = 0;
	return p->dt;
}

int frame_next_step(MR_FramePacer *p)
{
	if (!p)
		return 0;

	if (p->accumulator < p->fixedStep)
		return 0;

	if (p->stepsThisFrame >= FRAME_MAX_STEPS) {
		/*
		 * Too far behind to catch up. Drop the backlog rather than
		 * carry it: keeping it would demand the same overrun next
		 * frame and every frame after, which is the spiral this bound
		 * exists to prevent. The game runs in slow motion for a
		 * moment, which is visible but recoverable.
		 */
		p->accumulator = 0.0;
		return 0;
	}

	p->accumulator -= p->fixedStep;
	p->stepsThisFrame++;
	return 1;
}

double frame_alpha(const MR_FramePacer *p)
{
	if (!p || p->fixedStep <= 0.0)
		return 0.0;
	return p->accumulator / p->fixedStep;
}

void frame_end(MR_FramePacer *p)
{
	if (!p || p->targetFps <= 0.0)
		return;
	tm_wait_until(p->frameStart + 1.0 / p->targetFps);
}

double frame_fps(const MR_FramePacer *p)
{
	double sum = 0.0;
	int    i;

	if (!p || p->ringCount == 0)
		return 0.0;
	for (i = 0; i < p->ringCount; i++)
		sum += p->ring[i];
	if (sum <= 0.0)
		return 0.0;
	return (double)p->ringCount / sum;
}
