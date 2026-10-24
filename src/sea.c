/*
 * sea.c -- the water mesh and its simulation, ported from
 * _wr64/src_n64/ot_sea.c.
 *
 * TWO DIFFERENT KINDS OF PORT LIVE IN THIS FILE, deliberately.
 *
 * sea_build is the ALGORITHM of make_data_table. That function is 4x
 * loop-unrolled by the decompiler, so transcribing it would carry the
 * unrolling with no benefit; the layout it produces is simple and is
 * verified by its own arithmetic (see below).
 *
 * wave_step and sea_push are LITERAL TRANSCRIPTIONS of wave_sea and
 * push_sea, variable names and all. Those two are integer relaxation
 * with 16-bit truncation at specific points, and the places where a
 * temporary is 16 bits rather than 32 change the result. Tidying them
 * into something readable would be guessing at behaviour. plan.txt says
 * accurate spaghetti beats plausible prose, and this is where that bites.
 *
 * THE LAYOUT, derived by tracing make_data_table's two readable phases:
 * rows -N..-1 with widths N+1..2N give 392 entries at N = 16, rows 0..N
 * with widths 2N+1..N+1 give 425, and 392 + 425 = 817 = 3*N*(N+1)+1,
 * which is exactly what the original sizes position_t to. So row r has
 * width 2N+1-|r| starting at dx = -N + max(0, r), with
 *
 *     sx = (col - ((2N - |r|) >> 1)) * 64, less 32 when |r| is odd
 *     sy = r * 64 * sqrt(3)/2, rounded away from zero
 *
 * At N = 16 that spans +/-1024 by +/-887, and 1024 * sqrt(3) is 1774, so
 * the hexagon is regular.
 *
 * NOT PORTED: init_wave_high, which seeds the surface with noise. Its
 * decompile is visibly uncertain - the stack array it uses defeated the
 * decompiler, which left a commented-out second array behind - and
 * plan.txt section 9 does not yet know whether the results screen needs
 * the seed at all or is driven entirely by injection. Starting flat and
 * injecting is honest; inventing a noise function would not be.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sea.h"

/* The simulation buffer, wave[384][128] of 4-byte cells. plan.txt 4.1. */
#define WAVE_ROWS  384
#define WAVE_COLS  128
#define WAVE_CELL  4

/* sea_mode 1, from ot_sea.c:1927. These are paired with the 30 Hz tick
 * of the results screen; the race uses 9, 5, 6 at 20 Hz. plan.txt 2. */
int SUII_POWER=10;
int WAVE_POWER=4;
int WATER_DAMP=5;

typedef struct {
    short height;
    short flags;
} BigWaveCell;

static BigWaveCell    s_wave[WAVE_ROWS][WAVE_COLS];
static SeaDispOffset  s_index[SEA_POINTS];
static SeaScreenEntry s_pos[SEA_POINTS];
static unsigned short s_idx[SEA_INDICES];
static int            s_nidx;
static int            s_built;

/* plan.txt 6.3: the renderer interpolates between the last two ticks, so
 * a 30 Hz simulation does not judder on a faster display. */
static short s_prevH[SEA_POINTS];
static short s_currH[SEA_POINTS];

static int s_rowStart[SEA_ROWS];
static int s_rowWidth[SEA_ROWS];

/* Per-row x span, for sea_contains. The hexagon is NOT symmetric in x:
 * odd rows are shifted 32 left, so they reach further left than right
 * and an idealised outline rejects 24 real corners. */
static short s_rowMinX[SEA_ROWS];
static short s_rowMaxX[SEA_ROWS];

/*
 * The three corners vertex_sea samples for a surface normal, per point,
 * or -1 at the rim.
 *
 * vertex_sea uses a different index stride in each of its three loop
 * phases, but all three pick the same two points geometrically: the
 * ones in the adjacent rows 32 units left of the centre, level with
 * each other. Built from that rule, having checked it reproduces all
 * three strides. plan.txt section 9.2 has them.
 */
static int s_nb[SEA_POINTS][3];

/* The interpolated heights one sea_bake works from, so that the surface
 * and the normals derived from it agree. */
static short s_bakeH[SEA_POINTS];

/*
 * sin_t, the game's sine table: sin_t[i] = sin(i/4096 * 2*PI), per
 * ot_system.h and camera.c. Built here rather than shipped - the values
 * match to float precision, and what the table is actually for is
 * quantising a heading to 4096 steps, which the & 0xFFF does.
 */
static float s_sinT[4096];

/* move_sea's walker state, with the original's starting values. px and
 * pz both 1024 puts it outside the patch on the first step, so it
 * always begins by respawning somewhere random. */
static int   s_wr  = 0;
static int   s_wra = 1;
static float s_wpx = 1024.0f;
static float s_wpz = 1024.0f;

/* The byte offset a SeaScreenEntry carries is masked into the buffer when
 * it is built, so this needs no range check. plan.txt 4.1 and
 * _wr64/docs/subsystems/ot_sea_report.txt section 8. */
#define SEA_BWAVE(off) ((BigWaveCell *)((char *)&s_wave[0][0] + (off)))

static int row_width(int r)
{
    return 2 * SEA_N + 1 - (r < 0 ? -r : r);
}

/* The original adds -0.5 on negative rows and +0.5 on positive ones and
 * then truncates, which rounds away from zero. */
static short row_sy(int r)
{
    double k = sqrt(3.0) * (double)SEA_COL_STEP * 0.5 * (double)r;

    return (short)(int)(k + (r < 0 ? -0.5 : 0.5));
}

/*
 * Triangulate the strip between two rows of the triangular lattice.
 *
 * Widths differ by one and the rows are offset half a column, so each
 * vertex of the narrower row sits between two of the wider. A strip
 * between rows of n and m vertices has n + m - 2 triangles.
 */
static void strip(int aStart, int aWide, int bStart, int bWide)
{
    int i;

    if (bWide == aWide + 1) {
        for (i = 0; i < aWide; i++) {
            s_idx[s_nidx++] = (unsigned short)(aStart + i);
            s_idx[s_nidx++] = (unsigned short)(bStart + i);
            s_idx[s_nidx++] = (unsigned short)(bStart + i + 1);
        }
        for (i = 0; i + 1 < aWide; i++) {
            s_idx[s_nidx++] = (unsigned short)(aStart + i);
            s_idx[s_nidx++] = (unsigned short)(bStart + i + 1);
            s_idx[s_nidx++] = (unsigned short)(aStart + i + 1);
        }
    } else {
        for (i = 0; i < bWide; i++) {
            s_idx[s_nidx++] = (unsigned short)(aStart + i);
            s_idx[s_nidx++] = (unsigned short)(bStart + i);
            s_idx[s_nidx++] = (unsigned short)(aStart + i + 1);
        }
        for (i = 0; i + 1 < bWide; i++) {
            s_idx[s_nidx++] = (unsigned short)(aStart + i + 1);
            s_idx[s_nidx++] = (unsigned short)(bStart + i);
            s_idx[s_nidx++] = (unsigned short)(bStart + i + 1);
        }
    }
}

/* The point in row r with this sx, or -1. Rows are short, and this runs
 * once. */
static int row_find(int r, int sx)
{
    int i;

    if (r < 0 || r >= SEA_ROWS)
        return -1;
    for (i = s_rowStart[r]; i < s_rowStart[r] + s_rowWidth[r]; i++)
        if (s_pos[i].sx == (short)sx)
            return i;
    return -1;
}

int sea_build(void)
{
    int n = 0;
    int r, c;

    memset(s_wave, 0, sizeof(s_wave));
    memset(s_index, 0, sizeof(s_index));
    memset(s_pos, 0, sizeof(s_pos));
    memset(s_prevH, 0, sizeof(s_prevH));
    memset(s_currH, 0, sizeof(s_currH));
    s_nidx = 0;

    for (r = -SEA_N; r <= SEA_N; r++) {
        int   w       = row_width(r);
        int   dxStart = -SEA_N + (r > 0 ? r : 0);
        int   half    = (2 * SEA_N - (r < 0 ? -r : r)) >> 1;
        int   odd     = ((r < 0 ? -r : r) & 1) != 0;
        short sy      = row_sy(r);

        s_rowStart[r + SEA_N] = n;
        s_rowWidth[r + SEA_N] = w;

        for (c = 0; c < w; c++) {
            int dx = dxStart + c;
            int wr, wc;

            if (n >= SEA_POINTS)
                return n;

            s_index[n].dx = (signed char)dx;
            s_index[n].dy = (signed char)r;

            s_pos[n].sx = (short)(((c - half) << 6) - (odd ? 32 : 0));
            s_pos[n].sy = sy;

            /* ot_sea.c:2481, with the patch at the origin so that
             * its col_base and row_mod are both zero. The carry of the
             * column's high bits into the row is the original's, and
             * sea_push carries the same one; see plan.txt 10.2. It is
             * zero for a column of 0..16 and -128 below that. */
            wr = ((dx & ~0x7F) + r + 1536) % WAVE_ROWS;
            wc = dx & (WAVE_COLS - 1);
            s_pos[n].offset = ((wr << 7) + wc) * WAVE_CELL;

            n++;
        }
    }

    for (r = 0; r + 1 < SEA_ROWS; r++)
        strip(s_rowStart[r], s_rowWidth[r],
              s_rowStart[r + 1], s_rowWidth[r + 1]);

    for (r = 0; r < 4096; r++)
        s_sinT[r] = (float)sin(6.283185307179586 * (double)r / 4096.0);

    for (r = 0; r < SEA_ROWS; r++) {
        s_rowMinX[r] = s_pos[s_rowStart[r]].sx;
        s_rowMaxX[r] = s_pos[s_rowStart[r] + s_rowWidth[r] - 1].sx;

        for (c = 0; c < s_rowWidth[r]; c++) {
            int i = s_rowStart[r] + c;

            s_nb[i][0] = row_find(r - 1, s_pos[i].sx - 32);
            s_nb[i][1] = row_find(r + 1, s_pos[i].sx - 32);
            s_nb[i][2] = (c + 1 < s_rowWidth[r]) ? i + 1 : -1;
        }
    }

    s_built = 1;
    sea_seed(SEA_SEED_HIGH);
    return n;
}

/* ------------------------------------------------------------------ */
/* wave_sea, transcribed                                               */
/* ------------------------------------------------------------------ */

/*
 * Three phases over the hexagon: the upper rows, the lower rows, then a
 * two-stride tip strip and the centre cell.
 *
 * Each cell couples to its neighbour in the next row and to its left
 * neighbour in its own row, exchanging an average term (>> SUII_POWER)
 * and a difference term (>> WAVE_POWER) through flags, which acts as a
 * velocity. flags is folded into height and then decayed by
 * >> WATER_DAMP.
 *
 * The variable names are the decompiler's. Several temporaries are 16
 * bits and the truncation is load bearing: v28, for instance, is stored
 * into both height and flags after truncating, and widening it changes
 * the surface.
 *
 * The cursor advances exactly 3*N*(N+1) entries, 816 at N = 16, one
 * short of the 817 the layout produces, which is why no phase can run
 * off the end. Verified in _wr64 by simulating the arithmetic against an
 * unguarded m2c decompile; see ot_sea_report.txt section 8.
 *
 * THE SURFACE SETTLES ABOVE ZERO, not back to it. Integer truncation in
 * the averaging term leaves a residue, so a large injection decays to a
 * uniform level rather than to flat: about 509 in fixed point, two world
 * units, reached by tick 300 and unchanged at tick 6000. It is stable,
 * not a drift, and the original computes the same thing. Do not go
 * looking for a leak.
 */
static void wave_step(void)
{
    int v0, v1, v3, height, v8, v9, v10, offset;
    int v14, v15, v16, v17, v18, v20, v21, v22;
    int v25, v26, v27;
    int v29, v35, v38, v39, v40, v41, v42, v44, v45, v46;
    int v53, v54, v56, v57, v58, v60, v61, v64;
    int flags;
    int i;
    short v23, v24, v28, v33, v47, v48, v52, v59, v62, v63;
    SeaScreenEntry *v2, *v4, *v11, *v34, *v36, *v50;
    BigWaveCell *v5, *v7, *v13, *v19, *v32, *v37, *v43, *v51, *v55;

    v0 = SEA_N;
    v1 = -SEA_N;
    v2 = s_pos;

    /* -- phase 1: upper rows ---------------------------------------- */
    v3 = -1 * SEA_N;
    do {
        v4 = &v2[2 * SEA_N + 2 + v1];
        v5 = SEA_BWAVE(v2->offset);
        height = v5->height;
        v7 = SEA_BWAVE(v4[-1].offset);
        v8 = v7->height;
        v9 = (height + v8) >> SUII_POWER;
        v10 = (v8 - height) >> WAVE_POWER;
        v5->flags = (short)(v5->flags + v10 - v9);
        v7->flags = (short)(v7->flags - v10 - v9);
        v11 = &v2[2 * SEA_N + v3];
        if (v2 < v11) {
            do {
                ++v2;
                v13 = SEA_BWAVE(v4->offset);
                v14 = v13->height;
                v15 = (height + v14) >> SUII_POWER;
                v16 = (v14 - height) >> WAVE_POWER;
                v5->flags = (short)(v5->flags + v16 - v15);
                ++v4;
                v13->flags = (short)(v13->flags - v16 - v15);
                v5 = SEA_BWAVE(v2->offset);
                height = v5->height;
                v17 = (height + v14) >> SUII_POWER;
                v18 = (v14 - height) >> WAVE_POWER;
                v5->flags = (short)(v5->flags + v18 - v17);
                v13->flags = (short)(v13->flags - v18 - v17);
                v19 = SEA_BWAVE(v2[-1].offset);
                v20 = v19->height;
                v21 = (height + v20) >> SUII_POWER;
                v22 = (v20 - height) >> WAVE_POWER;
                v5->flags = (short)(v5->flags + v22 - v21);
                v23 = v19->height;
                v24 = (short)(v19->flags - v22 - v21);
                v19->flags = v24;
                v19->height = (short)(v23 + v24);
                v19->flags = (short)(v19->flags - (v19->flags >> WATER_DAMP));
            } while (v2 < v11);
        }
        offset = v4->offset;
        ++v1;
        v25 = SEA_BWAVE(offset)->height;
        ++v3;
        v26 = (height + v25) >> SUII_POWER;
        v27 = (v25 - height) >> WAVE_POWER;
        v28 = (short)(v5->flags + v27 - v26);
        v5->height = (short)(v5->height + v28);
        v5->flags = v28;
        ++v2;
        SEA_BWAVE(offset)->flags =
            (short)(SEA_BWAVE(offset)->flags - v27 - v26);
        v5->flags = (short)(v5->flags - (v5->flags >> WATER_DAMP));
    } while (v1 < 0);
    v0 = SEA_N;

    /* -- phase 2: lower rows ---------------------------------------- */
    v29 = 0;
    for (i = v2->offset; ; i = v2->offset) {
        v32 = SEA_BWAVE(i);
        v33 = SEA_BWAVE(i)->height;
        v34 = &v2[2 * v0 - v29];
        v35 = v33;
        if (v2 < v34) {
            v36 = &v2[2 * v0 - v29 + 1];
            do {
                ++v2;
                v37 = SEA_BWAVE(v36->offset);
                v38 = v37->height;
                v39 = (v35 + v38) >> SUII_POWER;
                v40 = (v38 - v35) >> WAVE_POWER;
                v32->flags = (short)(v32->flags + v40 - v39);
                ++v36;
                v37->flags = (short)(v37->flags - v40 - v39);
                v32 = SEA_BWAVE(v2->offset);
                v35 = v32->height;
                v41 = (v35 + v38) >> SUII_POWER;
                v42 = (v38 - v35) >> WAVE_POWER;
                v32->flags = (short)(v32->flags + v42 - v41);
                v37->flags = (short)(v37->flags - v42 - v41);
                v43 = SEA_BWAVE(v2[-1].offset);
                v44 = v43->height;
                v45 = (v35 + v44) >> SUII_POWER;
                v46 = (v44 - v35) >> WAVE_POWER;
                v32->flags = (short)(v32->flags + v46 - v45);
                v47 = v43->height;
                v48 = (short)(v43->flags - v46 - v45);
                v43->flags = v48;
                v43->height = (short)(v47 + v48);
                v43->flags = (short)(v43->flags - (v43->flags >> WATER_DAMP));
            } while (v2 < v34);
            v33 = v32->height;
        }
        flags = v32->flags;
        ++v29;
        v32->height = (short)(v33 + flags);
        ++v2;
        v32->flags = (short)(flags - (flags >> WATER_DAMP));
        v0 = SEA_N;
        if (v29 >= SEA_N)
            break;
    }

    /* -- phase 3: tip strip, stride two, then the centre ------------ */
    v50 = &v2[v0];
    v51 = SEA_BWAVE(v2->offset);
    v52 = v51->height;
    v53 = v51->height;
    if (v2 < v50) {
        do {
            v54 = v2[1].offset;
            v2 += 2;
            v55 = SEA_BWAVE(v54);
            v56 = SEA_BWAVE(v54)->height;
            v57 = (v53 + v56) >> SUII_POWER;
            v58 = (v56 - v53) >> WAVE_POWER;
            v59 = (short)(v51->flags + v58 - v57);
            v51->height = (short)(v51->height + v59);
            v51->flags = v59;
            v55->flags = (short)(SEA_BWAVE(v54)->flags - v58 - v57);
            v51->flags = (short)(v51->flags - (v51->flags >> WATER_DAMP));
            v51 = SEA_BWAVE(v2->offset);
            v53 = v51->height;
            v60 = (v53 + v56) >> SUII_POWER;
            v61 = (v56 - v53) >> WAVE_POWER;
            v51->flags = (short)(v51->flags + v61 - v60);
            v62 = SEA_BWAVE(v54)->height;
            v63 = (short)(SEA_BWAVE(v54)->flags - v61 - v60);
            v55->flags = v63;
            v55->height = (short)(v62 + v63);
            /* The original reloads flags here rather than using v63, so
             * the damp is applied to the value just stored. */
            v55->flags = (short)(SEA_BWAVE(v54)->flags
                                 - (SEA_BWAVE(v54)->flags >> WATER_DAMP));
        } while (v2 < v50);
        v52 = v51->height;
    }
    v64 = v51->flags;
    v51->height = (short)(v52 + v64);
    v51->flags = (short)(v64 - (v64 >> WATER_DAMP));
}

/* ------------------------------------------------------------------ */
/* push_sea, transcribed                                              */
/* ------------------------------------------------------------------ */

/*
 * Stamp a height into the three cells of the containing triangle and
 * zero their velocity. The skew constants are 1/sqrt(3) and 2/sqrt(3),
 * turning world XZ into hex coordinates; 24576 is 384 * 64, the buffer's
 * row count times the column pitch, so the modulo wraps into it.
 *
 * v4 is the hex column and v8 the hex row. The (v4 & 0xFFFFFF80) added
 * to the row is the original's own: a carry out of the 7-bit column
 * field moves 128 rows, not one, and sea_build carries the same term so
 * the two agree. plan.txt section 10.2.
 */
void sea_push(float fpx, float fpz, int fdp)
{
    int           v3, v4, v5, v6, v7, v8, v12;
    unsigned int  v9, v11;
    BigWaveCell  *v10, *v13, *v14;
    short         flags;

    if (!s_built)
        return;

    v3 = (int)(float)((float)(0.57735026f * fpz) + fpx) % 24576;
    v4 = v3 >> 6;
    v5 = v3 - (v3 >> 6 << 6);
    v6 = (v3 >> 6) & 0x7F;
    v7 = (int)(float)(1.1547005f * fpz) % 24576;
    v8 = v7 >> 6;
    if (v5 >= v7 - (v7 >> 6 << 6)) {
        v11 = v8 + ((v4 + 1) & 0xFFFFFF80);
        v12 = (v4 + 1) & 0x7F;
        v10 = &s_wave[(int)(v11 + 1536) % WAVE_ROWS][v12];
        v9 = v8 + (v4 & 0xFFFFFF80);
    } else {
        v9 = v8 + (v4 & 0xFFFFFF80);
        v6 = v4 & 0x7F;
        v10 = &s_wave[(int)(v9 + 1537) % WAVE_ROWS][v6];
        v11 = v8 + ((v4 + 1) & 0xFFFFFF80);
        v12 = (v4 + 1) & 0x7F;
    }
    v13 = &s_wave[(int)(v11 + 1537) % WAVE_ROWS][v12];
    v13->height = (short)((unsigned short)fdp << 8);
    v14 = &s_wave[(int)(v9 + 1536) % WAVE_ROWS][v6];
    v14->height = (short)((unsigned short)fdp << 8);
    v10->height = (short)((unsigned short)fdp << 8);
    v13->flags = 0;
    flags = v13->flags;
    v14->flags = flags;
    v10->flags = flags;
}

/* ------------------------------------------------------------------ */
/* init_wave_high, transcribed                                         */
/* ------------------------------------------------------------------ */

/*
 * Seed the whole buffer with a smoothed random field of amplitude high.
 * kn_init.c passes push_amplitude, which is 20 on the screens that
 * matter. Two passes: the first sets, walking columns with rows fastest,
 * the second adds, walking rows. plan.txt 10.4.
 *
 * THIS IS WHAT KEEPS THE SHEEN ALIVE, and not for the reason it looks.
 * vertex_sea builds its normal from heights already shifted >> 7, so
 * anywhere the water is locally flat the gradient is exactly zero across
 * whole patches, every vertex there samples the one centre texel, and
 * the triangles show through. Seeded, 15 of 817 corners have a flat
 * normal; unseeded it is 490.
 *
 * Only v24[5] is ever refreshed: (v6 & 1) is true on the first turn of
 * the inner loop and never again, since v6 only doubles. So the other
 * five are a fixed offset rather than a moving window, which is why the
 * field comes out biased positive and smaller than the nominal +/- high.
 * Transcribed as written.
 */
void sea_seed(int high)
{
    unsigned long v24[6];
    float         v4 = (float)high;
    int           i, j, k, v3, v6, v7, p;

    if (!s_built)
        return;

    for (p = 0; p < 6; p++)
        v24[p] = (unsigned long)(rand() & 0x7FFF);

    for (v3 = 0; v3 < 512; v3 += 4) {
        for (i = 0; i < WAVE_ROWS; i++) {
            v6 = 1;
            v7 = 0;
            for (p = 5; p >= 0; p--) {
                if (v6 & 1)
                    v24[p] = (unsigned long)(rand() & 0x7FFF);
                v7 += (int)v24[p];
                v6 *= 2;
            }
            s_wave[i][v3 >> 2].height =
                (short)((int)(v4 * (((float)v7 / 6.0f) / 16383.5f - 1.0f))
                        << 8);
            s_wave[i][v3 >> 2].flags = 0;
        }
    }

    for (p = 0; p < 6; p++)
        v24[p] = (unsigned long)(rand() & 0x7FFF);

    for (j = 0; j < WAVE_ROWS; j++) {
        for (k = 0; k < WAVE_COLS; k++) {
            v6 = 1;
            v7 = 0;
            for (p = 5; p >= 0; p--) {
                if (v6 & 1)
                    v24[p] = (unsigned long)(rand() & 0x7FFF);
                v7 += (int)v24[p];
                v6 *= 2;
            }
            s_wave[j][k].height = (short)
                (s_wave[j][k].height
                 + ((int)(v4 * (((float)v7 / 6.0f) / 16383.5f - 1.0f)) << 8));
        }
    }

    memcpy(s_prevH, s_currH, sizeof(s_prevH));
    for (i = 0; i < SEA_POINTS; i++)
        s_currH[i] = SEA_BWAVE(s_pos[i].offset)->height;
}

/* ------------------------------------------------------------------ */
/* move_sea's walker, transcribed                                      */
/* ------------------------------------------------------------------ */

/*
 * One invisible point skimming the patch, dimpling it as it goes. This
 * is the whole of what disturbs the water on the game's menu and
 * results screens - no boat, no rider, just this. plan.txt 10.3.
 *
 * The depth is NEGATIVE, so it digs a trough rather than raising a
 * crest, which is why it reads as a dent being dragged along.
 */
void sea_walk(void)
{
    float v0, v1, v2, v4, v5;
    short v3;
    int   size = SEA_N * SEA_COL_STEP;

    if (!s_built)
        return;

    v0 = 51.2f * s_sinT[((unsigned short)s_wr
                         + (unsigned short)s_wra + 4096) & 0xFFF];
    v1 = s_wpx + 51.2f * s_sinT[(((((unsigned short)s_wr
                                    + (unsigned short)s_wra + 4096) & 0xFFF))
                                 + 1024) & 0xFFF];
    s_wr = ((unsigned short)s_wr + (unsigned short)s_wra + 4096) & 0xFFF;
    s_wpx = v1;
    v2 = s_wpz + v0;
    s_wpz = v2;
    if ((float)(size * size) < (float)((float)(v1 * v1) + (float)(v2 * v2))) {
        v3 = (short)rand();
        v4 = s_sinT[((v3 & 0xFFF) + 1024) & 0xFFF] * (float)size;
        v5 = s_sinT[v3 & 0xFFF];
        s_wr = v3 & 0xFFF;
        s_wpx = v4;
        s_wpz = v5 * (float)size;
        s_wr = rand() & 0xFFF;
        s_wra = (int)((float)((float)(rand() & 0x3FFF) * 14.0) / 16383.0) - 7;
    }
    sea_push(s_wpx, s_wpz, -50);
}

/* Where the walker is, for the overlay. */
void sea_walk_where(float *px, float *pz)
{
    if (px) *px = s_wpx;
    if (pz) *pz = s_wpz;
}

/* ------------------------------------------------------------------ */
/* driving it                                                          */
/* ------------------------------------------------------------------ */

void sea_tick(void)
{
    int i;

    if (!s_built)
        return;

    /* plan.txt 6.3: keep the previous tick's heights so the renderer can
     * interpolate. Two arrays of 817 shorts is 3.2 KB. */
    memcpy(s_prevH, s_currH, sizeof(s_prevH));
    wave_step();
    for (i = 0; i < SEA_POINTS; i++)
        s_currH[i] = SEA_BWAVE(s_pos[i].offset)->height;
}

/*
 * Back to flat and still.
 *
 * flags has to go as well as height. It is the velocity term, so a
 * buffer with zero height and non-zero flags is a surface that is
 * momentarily flat and about to move; only clearing both gives a
 * surface that stays put. The two height snapshots go too, or the
 * renderer interpolates out of the old state for one tick.
 */
void sea_reset(void)
{
    memset(s_wave, 0, sizeof(s_wave));
    memset(s_prevH, 0, sizeof(s_prevH));
    memset(s_currH, 0, sizeof(s_currH));
    memset(s_bakeH, 0, sizeof(s_bakeH));
}

void sea_bake(RVertex *verts, float alpha)
{
    int i;

    if (!s_built || !verts)
        return;
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;

    /* Interpolate first and in full. The normals below are differences
     * of neighbouring heights, so taking them from the current tick
     * while the surface moves smoothly would strobe the sheen at the
     * tick rate and undo plan.txt 6.3. */
    for (i = 0; i < SEA_POINTS; i++)
        s_bakeH[i] = (short)((float)s_prevH[i]
                     + ((float)s_currH[i] - (float)s_prevH[i]) * alpha);

    for (i = 0; i < SEA_POINTS; i++) {
        verts[i].x = (float)s_pos[i].sx;
        /* The original bakes (height >> 8) + sea_offset as world Y.
         * sea_offset on this screen is still unknown, plan.txt 10, so
         * it is left at zero and the water sits on y = 0. */
        verts[i].y = (float)s_bakeH[i] * (1.0f / 256.0f);
        verts[i].z = (float)s_pos[i].sy;
        verts[i].color    = 0xFFFFFFFFu;
        verts[i].specular = 0xFF000000u;

        /*
         * vertex_sea's sphere map, transcribed, then scaled from the
         * N64's S10.5 coordinate into D3D's 0..1: tc 1024 is the
         * centre, 992 the swing, G_TEXTURE halves it and the texture
         * is 32 texels, so u = tc/2048. The slope terms are a lattice
         * cross product whose up component is sqrt(12582912).
         * plan.txt section 9.2.
         */
        {
            int   a = s_nb[i][0], b = s_nb[i][1], cc = s_nb[i][2];
            int   hs = (int)s_bakeH[i] >> 7;
            int   ha = (a  >= 0) ? (int)s_bakeH[a]  >> 7 : hs;
            int   hb = (b  >= 0) ? (int)s_bakeH[b]  >> 7 : hs;
            int   hc = (cc >= 0) ? (int)s_bakeH[cc] >> 7 : hs;
            int   sx = (((ha + hb) >> 1) - hc) << 6;
            int   sy = 55 * (ha - hb);
            float k  = 992.0f / (float)sqrt((double)(sx * sx + sy * sy)
                                            + 12582912.0);

            /* The + 1/64 is half a texel. tc/2048 lands on the texel
             * BOUNDARY, where a bilinear sample is a 50/50 blend of two
             * texels, so every lookup was softened and shifted. The
             * port does the same correction, as u += 0.5f in texel
             * units before dividing by the width; gfx_pc.c around the
             * buf_vbo writes. */
            verts[i].tu = ((float)sx * k + 1024.0f) * (1.0f / 2048.0f)
                          + (1.0f / 64.0f);
            verts[i].tv = ((float)sy * k + 1024.0f) * (1.0f / 2048.0f)
                          + (1.0f / 64.0f);
        }
    }
}

void sea_height_range(int *lo, int *hi)
{
    int i, a = 0, b = 0;

    for (i = 0; i < SEA_POINTS; i++) {
        if (s_currH[i] < a) a = s_currH[i];
        if (s_currH[i] > b) b = s_currH[i];
    }
    if (lo) *lo = a;
    if (hi) *hi = b;
}

const unsigned short *sea_indices(void)
{
    return s_idx;
}

int sea_index_count(void)
{
    return s_nidx;
}

const SeaScreenEntry *sea_entries(void)
{
    return s_pos;
}

void sea_extent(int *minx, int *maxx, int *minz, int *maxz)
{
    int i;
    int x0 = 0, x1 = 0, z0 = 0, z1 = 0;

    for (i = 0; i < SEA_POINTS; i++) {
        if (s_pos[i].sx < x0) x0 = s_pos[i].sx;
        if (s_pos[i].sx > x1) x1 = s_pos[i].sx;
        if (s_pos[i].sy < z0) z0 = s_pos[i].sy;
        if (s_pos[i].sy > z1) z1 = s_pos[i].sy;
    }
    if (minx) *minx = x0;
    if (maxx) *maxx = x1;
    if (minz) *minz = z0;
    if (maxz) *maxz = z1;
}

/*
 * Is world XZ inside the hexagon?
 *
 * Picking needs this because sea_push maps its argument with a % 24576,
 * so a point well outside the patch does not miss - it wraps, and
 * stamps a cell somewhere unrelated that may well be on screen.
 *
 * Tested against the real row extents rather than an outline, because
 * the stagger makes the hexagon asymmetric; see s_rowMinX. The z bound
 * is half a row pitch, so the test is exact to within one cell.
 */
int sea_contains(float px, float pz)
{
    int   r, best = 0;
    float bd = 0.0f;

    if (!s_built)
        return 0;
    if (pz < (float)s_pos[s_rowStart[0]].sy - 28.0f
        || pz > (float)s_pos[s_rowStart[SEA_ROWS - 1]].sy + 28.0f)
        return 0;

    for (r = 0; r < SEA_ROWS; r++) {
        float d = pz - (float)s_pos[s_rowStart[r]].sy;

        if (d < 0.0f)
            d = -d;
        if (r == 0 || d < bd) {
            bd = d;
            best = r;
        }
    }
    return px >= (float)s_rowMinX[best] && px <= (float)s_rowMaxX[best];
}
