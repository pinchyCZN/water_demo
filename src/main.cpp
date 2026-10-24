/*
 * main.cpp -- console, window and frame loop for the water study.
 *
 * M0: a cleared D3D9 window, a console reporting frame and tick rates,
 * and the fixed-step pump already in place with an empty simulation
 * step, so M3 has somewhere to land and the pacing can be judged before
 * there is any water to judge it by.
 *
 * Modelled on MOTORACER's src/main.cpp. Not copied: that file includes
 * 25 project headers, so the structure is reused and the body is not.
 * timing.c is copied verbatim; it depends only on windows.h.
 *
 * See plan.txt section 6 for why the simulation steps at a fixed rate
 * instead of taking the frame delta.
 */

#include <windows.h>
#include <windowsx.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "d3d9rt.h"
#include "sea.h"
#include "textures.h"
#include "timing.h"

extern "C" int SUII_POWER, WAVE_POWER, WATER_DAMP;

/* plan.txt section 2: the results screen asks for GRAPH_TIMING_RESULT,
 * which is 2, and the graph thread runs once every that many retraces
 * of a 60 Hz field. 60/2 = 30. The race is 20 Hz; this screen is not. */
static const double kSimHz[] = { 5.0, 10.0, 15.0, 20.0, 30.0, 60.0 };

/*
 * C steps through those. 30 is the results screen ; 20
 * is the in-race rate; 60 is past anything the original ran.
 *
 * CHANGING THIS CHANGES THE WATER, it does not just resample it. The
 * relaxation in wave_step is integer with fixed shifts, so a tick is a
 * fixed amount of propagation and decay rather than a fixed amount of
 * time: at 60 the same waves cross the grid and die away twice as fast
 * in wall-clock terms. plan.txt 6.1 and 6.4 are about exactly that, and
 * M7 is where it gets fixed rather than selected. Being able to flip
 * between the three is how the difference becomes visible.
 */
static int    s_simRate = 3;
static double s_simDt   = 1.0 / 20.0;

/* C was pressed; the backlog is measured in the old tick and has to go
 * rather than be re-divided. Consumed by the frame loop. */
static int    s_rateDirty;

/*
 * L. Off, the renderer runs as fast as the display allows and
 * interpolates between the last two ticks, so 20 Hz water is smooth at
 * 60. On, a frame goes out only when a tick happened and shows that
 * tick exactly, which is what the N64 does: Graph_timing gates its
 * graph thread to every second or third retrace, so the port both
 * simulates and displays at 20 or 30 Hz with nothing in between.
 */
static int    s_lockRender;

static int    s_walk;

static int    s_seedHigh = SEA_SEED_HIGH;

/* plan.txt section 6.2: a stall must not leave a backlog that costs more
 * than a frame to clear, or the backlog grows and never drains. */
static const int MAX_STEPS = 8;

static const char  kClassName[] = "WaterStudyWindow";
static const int   kClientWidth  = 960;
static const int   kClientHeight = 720;

static HWND s_hwnd;
static int  s_running = 1;
static int  s_clientW = kClientWidth;
static int  s_clientH = kClientHeight;

/* M3 replaces this. It exists now so the pump is exercised and the step
 * count can be seen on the console before there is a simulation. */
static unsigned int s_simTicks;

static RVertex s_verts[SEA_POINTS];

/* The projection. Named because pick_water has to build the inverse of
 * exactly this, and a second copy of 0.7f would drift from it. */
static const float kFovY  = 0.7f;
static const float kNearZ = 16.0f;
static const float kFarZ  = 40000.0f;

/*
 * Orbit camera.
 *
 * The default is the results-screen framing: the patch centred at the
 * origin, pulled back far enough to see all 2048 units of the middle row
 * and tilted down enough that the hexagon reads as a surface rather than
 * a line. R returns to it.
 *
 * CAD convention, as asked: left drag orbits, right drag pans, wheel
 * zooms. Pitch is clamped just short of straight down and straight up,
 * because at exactly +/-90 degrees the view direction and the world up
 * are parallel and the look-at cross product collapses.
 */
static const float kHomeDist  = 2470.0f;    /* hypot(900, 2300) */
static const float kHomePitch = 0.3737f;    /* asin(900/2470), 21.4 deg */
static const float kHomeYaw   = 0.0f;

static float s_dist  = kHomeDist;
static float s_yaw   = kHomeYaw;
static float s_pitch = kHomePitch;
static float s_target[3];

static int   s_dragL, s_dragR;

/* F1. The key column is padded so the descriptions line up in a
 * fixed-pitch face; the whole block is centred as a block, which reads
 * better than centring each line on its own. */
static int s_help;

static const char *s_helpText[] =
{
    "Z      make wave",
    "X      draw with mouse to make wave",
    "Space  reset the water, flat and still",
    "M      seed the chop, M+wheel its amplitude",
    "",
    "D+wheel  lift depth, 1 to 120",
    "  +shift to step by ten",
    "Q/W/E  SUII_POWER/WAVE_POWER/WATER_DAMP",
    "T/Y    next/prev course",
    "TAB    wireframe",
    "C      sim rate 20 / 30 / 60 Hz",
    "L      lock render to the sim rate",
    "F5     walker, the menu-screen dimple",
    "N      point / linear texture filter",
    "B      blend: n64 / zwrite / opaque / both",
    "F      maximize",
    "R      home view, default lift values",
    "P      pause",
    ".      step one tick",
    "",
    "left drag   orbit",
    "right drag  pan",
    "wheel       zoom",
    "",
    "F1     hide this",
    "Esc    quit"
};

/* X held lifts the surface under the cursor. s_liftHave says whether
 * s_liftX/s_liftZ hold the previous sample, which the stamp walks from
 * so that a fast drag leaves a ridge and not a dotted line. */
static int   s_lifting;
static int   s_liftHave;
static float s_liftX, s_liftZ;

/* The console prints once a second; the overlay wants something that
 * settles quickly but does not flicker, so keep an exponential
 * average of the real frame interval. */
static double s_fpsAvg;
static int   s_lastX, s_lastY;

static void view_home(void)
{
    s_dist = kHomeDist;
    s_yaw = kHomeYaw;
    s_pitch = kHomePitch;
    s_target[0] = s_target[1] = s_target[2] = 0.0f;
}

/* eye = target + dist * (cos p sin y, sin p, -cos p cos y), so yaw 0 and
 * pitch p puts the eye on -Z looking at the target, which is the head-on
 * view. Also hands back the basis: panning needs right and up, picking
 * needs all three. */
static void view_eye(float *eye, float *right, float *up, float *outFwd)
{
    float cp = (float)cos(s_pitch), sp = (float)sin(s_pitch);
    float cy = (float)cos(s_yaw),   sy = (float)sin(s_yaw);
    float fwd[3];

    fwd[0] = -cp * sy;
    fwd[1] = -sp;
    fwd[2] =  cp * cy;          /* target - eye, normalised */

    eye[0] = s_target[0] - fwd[0] * s_dist;
    eye[1] = s_target[1] - fwd[1] * s_dist;
    eye[2] = s_target[2] - fwd[2] * s_dist;

    if (outFwd) {
        outFwd[0] = fwd[0];
        outFwd[1] = fwd[1];
        outFwd[2] = fwd[2];
    }
    if (right) {
        right[0] =  cy;
        right[1] =  0.0f;
        right[2] =  sy;
    }
    if (up) {
        /* up = fwd x right. The order matters: right x fwd points
         * down, and anything not perpendicular to fwd leaves the
         * look-at to re-derive the basis, which shows up as the
         * horizon rolling as the pitch changes. */
        up[0] = -sp * sy;
        up[1] =  cp;
        up[2] =  sp * cy;
    }
}

/* Pause and single-step are owned here. timing.h has its own pair, but
 * tm_step_frames' requests are consumed inside frame_begin, which this
 * program does not call, so borrowing them would leave the requests
 * uncleared and the clock stepping for ever. Nothing reads tm_now. */
static int s_paused;
static int s_stepRequests;

static void sim_step(void)
{
    if (s_walk)
        sea_walk();
    sea_tick();
    s_simTicks++;
}

static void open_console(void)
{
    if (!AllocConsole())
        return;

    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
    SetConsoleTitleA("water -- console");
    printf("water study -- M0\n");
    printf("sim %.0f Hz (dt %.3f ms), max %d steps/frame\n",
           kSimHz[s_simRate], s_simDt * 1000.0, MAX_STEPS);
}

/*
 * One course index drives both, exactly as the game does.
 *
 * tex is crs_sea[] from live_textures.c, the index into sea_tex_list.
 * r/g/b is mSeaBottomColorTable[] from kn_val.h, which
 * makeKnSeaBottomGlist loads as the prim colour and then FILLS THE
 * WHOLE DISPLAY RECT with. That flat fill is the backdrop the
 * translucent water is blended over - there is no environment light
 * anywhere in the sea pass, the combiner is TEXEL0 alone. plan.txt 9.3.
 */
typedef struct {
    const char   *name;
    int           tex;
    unsigned char r, g, b;
} Course;

static const Course s_course[] = {
    { "DOLPHIN",  2,   0, 242, 228 },
    { "SUNSET",   1, 227, 112, 129 },
    { "MARIN",    6,  43, 135, 146 },
    { "MILKY",    4, 137, 180, 189 },
    { "PORT",     0,  80, 117,  85 },
    { "CASTLE",   5,  12,  58, 117 },
    { "SOUTHERN", 3,  28,  90, 221 },
    { "COOL",     7, 137, 180, 189 }
};
#define COURSE_COUNT (int)(sizeof(s_course) / sizeof(s_course[0]))

static int    s_crs = 0;                 /* SANDY, the cyan one */
static unsigned int s_sky = 0xFF00F2E4u;
static unsigned int s_hud = 0xFF590409u;

/*
 * A readable text colour for whatever bed colour is behind it.
 *
 * The complement alone is not enough: against SUNSET it differs by 41
 * in luminance and against MARIN by 37, which is not readable. So take
 * the complement for the hue, then drive its luminance to the far side
 * of the backdrop - darkened to 35% over a light bed, lifted 60% toward
 * white over a dark one. That holds every course between 103 and 184
 * apart where the plain complement managed as little as 37.
 */
static unsigned int contrast_for(unsigned int argb)
{
    int r = (int)((argb >> 16) & 0xFF);
    int g = (int)((argb >> 8) & 0xFF);
    int b = (int)(argb & 0xFF);
    int lum = (r * 299 + g * 587 + b * 114) / 1000;
    int cr = 255 - r, cg = 255 - g, cb = 255 - b;

    if (lum > 128) {
        cr = cr * 35 / 100;
        cg = cg * 35 / 100;
        cb = cb * 35 / 100;
    } else {
        cr += (255 - cr) * 60 / 100;
        cg += (255 - cg) * 60 / 100;
        cb += (255 - cb) * 60 / 100;
    }
    return 0xFF000000u | ((unsigned int)cr << 16)
         | ((unsigned int)cg << 8) | (unsigned int)cb;
}

/* Select a course: its sea texture, and its sea-bed colour as the
 * backdrop the water blends over. */
static void load_course(int which)
{
    const Course *c = &s_course[which];

    if (!r_texture_upload(sea_tex_list[c->tex], SEA_TEX_W, SEA_TEX_H)) {
        printf("texture: %s failed to upload\n", sea_tex_names[c->tex]);
        return;
    }
    r_texture_bind(1);
    s_sky = 0xFF000000u | ((unsigned int)c->r << 16)
          | ((unsigned int)c->g << 8) | (unsigned int)c->b;
    s_hud = contrast_for(s_sky);
    printf("course %d %-8s  tex %-14s  bed %3d,%3d,%3d\n",
           which, c->name, sea_tex_names[c->tex], c->r, c->g, c->b);
}

/* The stamp depth. D with the wheel, shift for ten. THE CEILING IS NOT
 * TASTE: sea_push truncates to 8 bits and the relaxation wraps the
 * 16-bit height above 120. plan.txt 10.1. */
#define LIFT_DEPTH_DEF    64
#define LIFT_DEPTH_MIN     1
#define LIFT_DEPTH_MAX   120

/* Fixed, not knobs: at an ordinary drag speed the stamps already
 * overlap and neither does anything. plan.txt 10. */
#define LIFT_SPACING      32
#define LIFT_STEPS        64

static int s_liftDepth = LIFT_DEPTH_DEF;

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void lift_defaults(void)
{
    s_liftDepth = LIFT_DEPTH_DEF;
}

/* A wheel notch while D or M is held. 0 if neither was, so the caller
 * knows the wheel still belongs to the zoom. */
static int lift_tweak(int notches, int big)
{
    int step = notches * (big ? 10 : 1);

    if (GetKeyState('D') & 0x8000)
        s_liftDepth = clampi(s_liftDepth + step,
                             LIFT_DEPTH_MIN, LIFT_DEPTH_MAX);
    else if (GetKeyState('M') & 0x8000) {
        s_seedHigh = clampi(s_seedHigh + step, SEA_SEED_MIN, SEA_SEED_MAX);
        sea_seed(s_seedHigh);
        printf("seed %d\n", s_seedHigh);
    } else
        return 0;
    return 1;
}

static void power_defaults(int val)
{
	if(val){
		SUII_POWER=9;
		WAVE_POWER=5;
		WATER_DAMP=6;
	}
	else{
		SUII_POWER=10;
		WAVE_POWER=4;
		WATER_DAMP=5;
	}
}
static int power_tweak(int notches, int big)
{
	int step = notches * (big ? 10 : 1);

	if(GetKeyState('Q') & 0x8000)
		SUII_POWER = clampi(SUII_POWER + step, 0, 100);
	else if(GetKeyState('W') & 0x8000)
		WAVE_POWER = clampi(WAVE_POWER + step, 0, 100);
	else if(GetKeyState('E') & 0x8000)
		WATER_DAMP = clampi(WATER_DAMP + step, 0, 100);
	else
		return 0;
	return 1;

}

/*
 * Where the cursor meets the water plane, or 0 if it does not.
 *
 * The inverse of r_set_camera's projection: that one builds
 * h = 1/tan(fovY/2) and scales x by h*height/width, so undoing it gives
 * a camera-space direction of (ndc_x*aspect*t, ndc_y*t, 1) with
 * t = tan(fovY/2). Rotate that by the view basis and intersect y = 0.
 */
static int pick_water(int mx, int my, float *outX, float *outZ)
{
    float eye[3], right[3], up[3], fwd[3];
    float t, aspect, ndcx, ndcy, cx, cy, dir[3], hit;

    if (s_clientW <= 0 || s_clientH <= 0)
        return 0;

    view_eye(eye, right, up, fwd);

    aspect = (float)s_clientW / (float)s_clientH;
    t      = (float)tan(kFovY * 0.5);
    ndcx   = 2.0f * ((float)mx + 0.5f) / (float)s_clientW - 1.0f;
    ndcy   = 1.0f - 2.0f * ((float)my + 0.5f) / (float)s_clientH;
    cx     = ndcx * aspect * t;
    cy     = ndcy * t;

    dir[0] = right[0] * cx + up[0] * cy + fwd[0];
    dir[1] = right[1] * cx + up[1] * cy + fwd[1];
    dir[2] = right[2] * cx + up[2] * cy + fwd[2];

    /* Looking along the water rather than at it: the intersection is at
     * infinity, or behind the eye. */
    if (dir[1] > -1.0e-4f && dir[1] < 1.0e-4f)
        return 0;
    hit = -eye[1] / dir[1];
    if (hit <= 0.0f)
        return 0;

    *outX = eye[0] + dir[0] * hit;
    *outZ = eye[2] + dir[2] * hit;
    return sea_contains(*outX, *outZ);
}

/* Stamp from the previous sample to this one, so a drag draws a line. */
static void lift_at(int mx, int my)
{
    float px, pz;

    if (!pick_water(mx, my, &px, &pz)) {
        /* Off the mesh: break the line rather than bridge the gap. */
        s_liftHave = 0;
        return;
    }

    /* The steps below are not range checked, and do not need to be: the
     * hexagon is convex, so a segment between two points inside it is
     * inside it. */
    if (s_liftHave) {
        float dx = px - s_liftX;
        float dz = pz - s_liftZ;
        float len = (float)sqrt((double)(dx * dx + dz * dz));
        int   steps = (int)(len / (float)LIFT_SPACING);
        int   k;

        if (steps > LIFT_STEPS)
            steps = LIFT_STEPS;
        for (k = 1; k < steps; k++) {
            float f = (float)k / (float)steps;

            sea_push(s_liftX + dx * f, s_liftZ + dz * f, s_liftDepth);
        }
    }
    sea_push(px, pz, s_liftDepth);
    s_liftX    = px;
    s_liftZ    = pz;
    s_liftHave = 1;
}

/* Centred as a block: the widest line fixes the left edge, the line
 * count fixes the top. */
static void draw_help(int lh)
{
    int n = (int)(sizeof(s_helpText) / sizeof(s_helpText[0]));
    int gw = r_font_width();
    int widest = 0;
    int x, y, i;

    for (i = 0; i < n; i++) {
        int len = (int)strlen(s_helpText[i]);

        if (len > widest)
            widest = len;
    }
    x = (s_clientW - widest * gw) / 2;
    y = (s_clientH - n * lh) / 2;
    if (x < 8) x = 8;
    if (y < 8) y = 8;

    /* Twice, offset: sot_sea_fine1 averages (194, 218, 219), so plain
     * white over it is unreadable. */
    for (i = 0; i < n; i++) {
        if (!s_helpText[i][0])
            continue;
        r_text(x + 1, y + i * lh + 1, 0xC0101010u, s_helpText[i]);
        r_text(x, y + i * lh, 0xFFFFFFFFu, s_helpText[i]);
    }
}

/* The cursor in client pixels, for a key press rather than a mouse
 * message. */
static int cursor_client(HWND h, int *mx, int *my)
{
    POINT pt;

    if (!GetCursorPos(&pt) || !ScreenToClient(h, &pt))
        return 0;
    *mx = pt.x;
    *my = pt.y;
    return 1;
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_DESTROY:
        s_running = 0;
        PostQuitMessage(0);
        return 0;

    case WM_SIZE:
        s_clientW = LOWORD(lp);
        s_clientH = HIWORD(lp);
        if (s_clientW > 0 && s_clientH > 0)
            r_resize(s_clientW, s_clientH);
        return 0;

    case WM_KEYDOWN:
        switch (wp) {
        case VK_ESCAPE:
            s_running = 0;
            PostQuitMessage(0);
            return 0;
		case VK_OEM_2:
        case 'P':
            s_paused = !s_paused;
            printf("sim %s\n", s_paused ? "paused" : "running");
            return 0;
        case VK_OEM_PERIOD:          /* step one tick; implies paused */
            s_paused = 1;
            s_stepRequests++;
            return 0;
        case 'Z':
            {
                /* A single cell is almost invisible at this scale,
                 * so drop a short ring of stamps. push_sea takes
                 * world XZ and a depth in whole units; the original
                 * shifts it left 8 into the fixed-point height. */
                int k;

                for (k = 0; k < 6; k++) {
                    float a = 6.2831853f * (float)k / 6.0f;
                    sea_push(200.0f * (float)cos(a),
                             200.0f * (float)sin(a), 48);
                }
                sea_push(0.0f, 0.0f, 64);
                printf("push: ring at r=200, centre depth 64\n");
            }
            return 0;
        case 'X':
            {
                int mx, my;

                /* Auto-repeat lands here too and is harmless: sea_push
                 * sets the cell rather than adding to it. */
                s_lifting = 1;
                if (cursor_client(h, &mx, &my))
                    lift_at(mx, my);
            }
            return 0;
        case VK_F1:
            s_help = !s_help;
            return 0;
        case 'M':
            sea_seed(s_seedHigh);
            printf("water: seeded, init_wave_high(%d)\n", s_seedHigh);
            return 0;
        case 'B':
            {
                static int bm;

                bm = (bm + 1) & 3;
                r_set_blend_mode(bm);
            }
            return 0;
        case 'N':
            {
                static int pt;

                pt = !pt;
                r_set_point_filter(pt);
            }
            return 0;
        case VK_F5:
            s_walk = !s_walk;
            printf("walker %s\n", s_walk ? "on" : "off");
            return 0;
        case 'V':
            s_simRate = (s_simRate + 1)
                        % (int)(sizeof(kSimHz) / sizeof(kSimHz[0]));
            s_simDt     = 1.0 / kSimHz[s_simRate];
            s_rateDirty = 1;
            printf("sim %.0f Hz\n", kSimHz[s_simRate]);
            return 0;
		case 'C':
		{
			int max=(int)(sizeof(kSimHz) / sizeof(kSimHz[0]));
			s_simRate = (s_simRate + max - 1) % max;
			s_simDt     = 1.0 / kSimHz[s_simRate];
			s_rateDirty = 1;
			printf("sim %.0f Hz\n", kSimHz[s_simRate]);
		}
			return 0;
		case 'L':
            s_lockRender = !s_lockRender;
            printf("render: %s\n", s_lockRender ? "locked to the sim rate"
                                                 : "free, interpolated");
            return 0;
        case 'F':
            /* What the title bar's maximise button does. WM_SIZE picks
             * up the new client area and resizes the device. */
            ShowWindow(h, IsZoomed(h) ? SW_RESTORE : SW_MAXIMIZE);
            return 0;
        case VK_SPACE:
            sea_reset();
            s_liftHave = 0;
            printf("water: reset\n");
            return 0;
		case '1':
			power_defaults(0);
			break;
		case '2':
			power_defaults(1);
			break;
		case 'R':
            view_home();
            lift_defaults();
            printf("view and lift values: default\n");
            return 0;
        case 'T':
            s_crs = (s_crs + 1) % COURSE_COUNT;
            load_course(s_crs);
            return 0;
		case 'Y':
			s_crs = (s_crs + COURSE_COUNT - 1) % COURSE_COUNT;
			load_course(s_crs);
			return 0;
		case VK_TAB:
            {
                static int wire;
                wire = !wire;
                r_set_wireframe(wire);
                printf("fill %s\n", wire ? "wireframe" : "solid");
            }
            return 0;
        }
        return 0;

    case WM_KEYUP:
        if (wp == 'X') {
            s_lifting  = 0;
            s_liftHave = 0;
        }
        return 0;

    case WM_KILLFOCUS:
        /* The key-up and button-up for anything held when focus goes
         * elsewhere are delivered to the other window, so drop every
         * held state here or it stays stuck on. */
        s_lifting  = 0;
        s_liftHave = 0;
        s_dragL    = 0;
        s_dragR    = 0;
        ReleaseCapture();
        return 0;

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        /* Capture so a drag that leaves the window keeps tracking, and
         * so the matching button-up still arrives. */
        if (msg == WM_LBUTTONDOWN) s_dragL = 1; else s_dragR = 1;
        s_lastX = GET_X_LPARAM(lp);
        s_lastY = GET_Y_LPARAM(lp);
        SetCapture(h);
        return 0;

    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        if (msg == WM_LBUTTONUP) s_dragL = 0; else s_dragR = 0;
        if (!s_dragL && !s_dragR)
            ReleaseCapture();
        return 0;

    case WM_MOUSEMOVE:
        {
            int mx = GET_X_LPARAM(lp);
            int my = GET_Y_LPARAM(lp);
            int dx = mx - s_lastX;
            int dy = my - s_lastY;

            s_lastX = mx;
            s_lastY = my;

            if (s_lifting)
                lift_at(mx, my);
            if (s_dragL) {
                s_yaw   += (float)dx * 0.008f;
                s_pitch += (float)dy * 0.008f;
                /* Short of vertical; see the note on view_eye. */
                if (s_pitch >  1.5533f) s_pitch =  1.5533f;
                if (s_pitch < -1.5533f) s_pitch = -1.5533f;
            }
            if (s_dragR) {
                float eye[3], right[3], up[3];
                /* Scale the pan with distance so the grab point tracks
                 * the cursor at any zoom. */
                float k = s_dist * 0.0012f;

                view_eye(eye, right, up, NULL);
                s_target[0] -= (right[0] * dx - up[0] * dy) * k;
                s_target[1] -= (right[1] * dx - up[1] * dy) * k;
                s_target[2] -= (right[2] * dx - up[2] * dy) * k;
            }
        }
        return 0;

    case WM_MOUSEWHEEL:
        {
            int   notches = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
            float f;

            if (lift_tweak(notches, GET_KEYSTATE_WPARAM(wp) & MK_SHIFT))
                return 0;
			if(power_tweak(notches, GET_KEYSTATE_WPARAM(wp) & MK_SHIFT))
				return 0;
            f = (float)pow(0.88, (double)notches);

            /* Multiplicative, so each notch is the same proportion of
             * the current distance rather than a fixed number of units. */
            s_dist *= f;
            if (s_dist < 120.0f)   s_dist = 120.0f;
            if (s_dist > 20000.0f) s_dist = 20000.0f;
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;                    /* D3D9 owns the client area */
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static HWND make_window(HINSTANCE hinst)
{
    WNDCLASSEXA wc;
    RECT        r;
    DWORD       style = WS_OVERLAPPEDWINDOW;

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc   = wndproc;
    wc.hInstance     = hinst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExA(&wc)) {
        printf("window: RegisterClassEx failed (%lu)\n", GetLastError());
        return NULL;
    }

    /* Size the frame so the CLIENT area is the size asked for; the back
     * buffer is created from the client rect. */
    r.left   = 0;
    r.top    = 0;
    r.right  = kClientWidth;
    r.bottom = kClientHeight;
    AdjustWindowRect(&r, style, FALSE);

    return CreateWindowExA(0, kClassName, "water -- wave grid", style,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top,
                           NULL, NULL, hinst, NULL);
}

int APIENTRY WinMain(HINSTANCE hinst, HINSTANCE hprev,
                     LPSTR cmdline, int showcmd)
{
    MSG    msg;
    double accumulator = 0.0;
    double prev;

    (void)hprev;
    (void)cmdline;

    open_console();
    tm_init();

    s_hwnd = make_window(hinst);
    if (!s_hwnd) {
        printf("window: CreateWindowEx failed (%lu)\n", GetLastError());
        return 1;
    }
    ShowWindow(s_hwnd, showcmd);
    UpdateWindow(s_hwnd);

    if (!r_init(s_hwnd, kClientWidth, kClientHeight)) {
        printf("d3d9: init failed; nothing to draw\n");
        return 1;
    }
    printf("high-resolution timer: %s\n",
           tm_have_highres_timer() ? "yes" : "NO -- pacing will be coarse");
    {
        int pts = sea_build();
        int x0, x1, z0, z1;

        sea_extent(&x0, &x1, &z0, &z1);
        printf("mesh: %d points (expect %d), %d tris, %d indices\n",
               pts, SEA_POINTS, sea_index_count() / 3, sea_index_count());
        printf("      extent x %d..%d  z %d..%d\n", x0, x1, z0, z1);
        if (pts != SEA_POINTS || sea_index_count() != SEA_INDICES)
            printf("mesh: UNEXPECTED counts -- layout is wrong\n");
    }
    load_course(s_crs);
    if (!r_font_init(16))
        printf("font: atlas failed; no on-screen readout\n");
    printf("F1 for the key list on screen\n");
    printf("mouse: left drag orbit, right drag pan, wheel zoom\n");

    prev = tm_real_now();

    while (s_running) {
        double now, frameDt;
        int    steps;

        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                s_running = 0;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!s_running)
            break;

        now     = tm_real_now();
        frameDt = now - prev;
        prev    = now;

        if (frameDt > 0.0) {
            double inst = 1.0 / frameDt;

            s_fpsAvg = (s_fpsAvg <= 0.0) ? inst
                                         : s_fpsAvg * 0.9 + inst * 0.1;
        }

        if (s_rateDirty) {
            s_rateDirty = 0;
            accumulator = 0.0;
        }

        /* plan.txt 6.2. Pausing stops feeding the accumulator, so the
         * water freezes while the renderer and the fps counter keep
         * running. A step request feeds it exactly one tick. */
        if (!s_paused)
            accumulator += frameDt;
        else if (s_stepRequests > 0) {
            accumulator += s_simDt;
            s_stepRequests--;
        }

        steps = 0;
        while (accumulator >= s_simDt && steps < MAX_STEPS) {
            sim_step();
            accumulator -= s_simDt;
            steps++;
        }
        if (steps == MAX_STEPS && accumulator >= s_simDt) {
            /* Dropped the rest on purpose: catching up would cost more
             * than a frame and the backlog would grow. */
            accumulator = 0.0;
        }

        /* Locked, a frame is only worth presenting when the surface
         * moved. Paused is the exception: the camera still has to turn.
         * Present is what paces this loop, so skipping it needs a yield
         * or the idle time becomes a busy wait. */
        if (s_lockRender && steps == 0 && !s_paused) {
            Sleep(1);
            continue;
        }

        if (r_ready() && r_begin(s_sky)) {
            {
                float eye[3], up[3];

                view_eye(eye, NULL, up, NULL);
                r_set_camera(eye, s_target, up, kFovY, kNearZ, kFarZ);
            }
            /* plan.txt 6.3: where we are between the last two ticks. */
            sea_bake(s_verts, s_lockRender
                               ? 1.0f : (float)(accumulator / s_simDt));
            r_draw(s_verts, SEA_POINTS, sea_indices(), sea_index_count());
            {
                char line[96];
                int  lh = r_font_height();

                /* Fixed field widths, so a changing digit count does
                 * not shuffle everything after it sideways. */
                _snprintf(line, sizeof(line) - 1,
                          "FPS %5.1f  SIM %5.1f HZ  DIST %5.0f%s%s%s",
                          s_fpsAvg, 1.0 / s_simDt, s_dist,
                          s_lockRender ? "  LOCKED" : "",
                          s_paused ? "  PAUSED" : "",
                          s_walk ? "  WALKER" : "");
                line[sizeof(line) - 1] = 0;
                r_text(8, 8, s_hud, line);

                {
                    int lo, hi;

                    sea_height_range(&lo, &hi);
                    _snprintf(line, sizeof(line) - 1, "%s  %s  H %d..%d",
                              s_course[s_crs].name,
                              sea_tex_names[s_course[s_crs].tex], lo, hi);
                }
                line[sizeof(line) - 1] = 0;
                r_text(8, 8 + lh, s_hud, line);

                _snprintf(line, sizeof(line) - 1,
                          "DEPTH %d  SEED %d", s_liftDepth, s_seedHigh);
                line[sizeof(line) - 1] = 0;
                r_text(8, 8 + lh * 2, s_hud, line);

				char *msg="";
				if((10 == SUII_POWER) && (4 == WAVE_POWER) && (5 == WATER_DAMP))
					msg="(20)";
				if((9 == SUII_POWER) && (5 == WAVE_POWER) && (6 == WATER_DAMP))
					msg="(30)";
				_snprintf(line, sizeof(line) - 1,
					"SUII_POWER %d  WAVE_POWER %d  WATER_DAMP %d  %s",
					SUII_POWER, WAVE_POWER, WATER_DAMP, msg);
				line[sizeof(line) - 1] = 0;
				r_text(8, 8 + lh * 3, s_hud, line);

                if (s_walk) {
                    float wx, wz;

                    sea_walk_where(&wx, &wz);
                    _snprintf(line, sizeof(line) - 1, "WALK %.0f, %.0f",
                              wx, wz);
                    line[sizeof(line) - 1] = 0;
                    r_text(8, 8 + lh * 4, s_hud, line);
                }

                if (s_help)
                    draw_help(lh);
            }
            r_end();
        }
    }

    r_shutdown();
    tm_shutdown();
    printf("ticks %u\n", s_simTicks);
    return 0;
}
