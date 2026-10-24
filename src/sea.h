/*
 * sea.h -- the Wave Race 64 water mesh.
 *
 * The grid is a regular hexagon in a triangular lattice, N cells from
 * centre to edge. Its vertex count is the centred hexagonal number
 *
 *     3*N*(N+1) + 1
 *
 * which at N = 16 is 817, and 817 is exactly what the original sizes
 * position_t to. See plan.txt section 4 and
 * _wr64/docs/subsystems/ot_sea_report.txt.
 *
 * M1 builds the layout and the index list. Heights arrive in M3.
 */

#ifndef WATER_SEA_H
#define WATER_SEA_H

#include "d3d9rt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* sea_mode 1, the results screen. plan.txt section 2. */
#define SEA_N        16
#define SEA_ROWS     (2 * SEA_N + 1)                /* 33 */
#define SEA_POINTS   (3 * SEA_N * (SEA_N + 1) + 1)  /* 817 */
#define SEA_TRIS     (6 * SEA_N * SEA_N)            /* 1536 */
#define SEA_INDICES  (SEA_TRIS * 3)                 /* 4608 */

/* World units between columns, and the world half-width of the middle
 * row. sea_disp_size is N << 6 in the original, which is this times N. */
#define SEA_COL_STEP 64

/*
 * One entry per mesh corner, mirroring the original's SeaScreenEntry.
 *
 * sx and sy are patch-local XZ, fixed once at build time. offset is the
 * byte offset of this corner's cell in the simulation buffer; the
 * original refreshes it every frame because its patch scrolls, and this
 * program does not scroll, so it is computed once. plan.txt 4.2.
 */
typedef struct {
    int   offset;
    short sx, sy;
} SeaScreenEntry;

/*
 * Relative hex coordinate per corner, the original's SeaDispOffset.
 *
 * The original declares these unsigned char and reads them back through
 * a signed cast, because the values run -N..N. Declaring them signed
 * here says what they are and reads back the same.
 */
typedef struct {
    signed char dx, dy;
} SeaDispOffset;

/* Build the layout and the triangle index list. Call once. Returns the
 * number of points written, which must be SEA_POINTS. */
int sea_build(void);

/* Advance the simulation one tick and snapshot the heights, so the
 * renderer can interpolate between the last two. plan.txt 6.3. */
void sea_tick(void);

/*
 * init_sea passes push_amplitude, which kn_init.c takes from
 * waveAmpTable[ncourse]: {20,20,20,40,10,30,20,25,25}. So it is a
 * per-course number between 10 and 40, not a constant.
 */
#define SEA_SEED_HIGH 20
#define SEA_SEED_MIN   0
#define SEA_SEED_MAX  80

/* Seed the buffer with the game's smoothed random field. Without it the
 * normals collapse wherever the water is locally flat; see sea.c. */
void sea_seed(int high);

/* Clear the surface to flat and motionless. Wipes the seed too, so
 * sea_seed has to run again to get the chop back. */
void sea_reset(void);

/* One tick of move_sea's walker: the dimple that crosses the water on
 * the game's menu screens. Call before sea_tick. */
void sea_walk(void);
void sea_walk_where(float *px, float *pz);

/* Fill verts with SEA_POINTS vertices. alpha blends the previous
 * tick's heights toward the current ones: 0 is the previous tick,
 * 1 the current. Pass accumulator / SIM_DT. */
void sea_bake(RVertex *verts, float alpha);

/* Stamp a wave into the three cells of the triangle containing
 * world (px, pz). This is push_sea; a mouse click calls it in M5. */
void sea_push(float px, float pz, int depth);

/* Current height extremes across the mesh, for the overlay. */
void sea_height_range(int *lo, int *hi);

/* Is world XZ inside the hexagon? Picking must check: sea_push wraps
 * rather than misses. */
int sea_contains(float px, float pz);

const unsigned short *sea_indices(void);
int  sea_index_count(void);

/* For the console summary and for M5's picking. */
const SeaScreenEntry *sea_entries(void);
void sea_extent(int *minx, int *maxx, int *minz, int *maxz);

#ifdef __cplusplus
}
#endif

#endif /* WATER_SEA_H */
