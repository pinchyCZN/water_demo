/*
 * d3d9rt.h -- minimal Direct3D 9 device for the water study.
 *
 * Adapted from MOTORACER's src/d3d9rt.cpp, reduced to what this program
 * needs. Three things were worth taking from it:
 *
 *   o  d3d9.dll is loaded with LoadLibrary and Direct3DCreate9 is looked
 *      up by name, so the program starts and reports a reason on a
 *      machine without it rather than failing to load at all.
 *
 *   o  the depth format is chosen by asking the device, not assumed:
 *      D24S8, then D24X8, then D16.
 *
 *   o  geometry is submitted UNTRANSFORMED, in world space, and a
 *      projection matrix does the divide. The alternative, writing
 *      pre-transformed screen-space vertices, skips D3D9's frustum clip
 *      and drops whole triangles at the screen edge against a finite
 *      guard band. MOTORACER hit that and moved off it.
 *
 * The vertex format is deliberately the same as theirs so the water mesh
 * and their scene geometry are interchangeable if that ever matters.
 */

#ifndef WATER_D3D9RT_H
#define WATER_D3D9RT_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One mesh corner. Position is world space; the projection and view
 * matrices are set by r_set_camera. color and specular are in the FVF
 * but the sea stage does not read them: its combiner is TEXEL0 alone.
 * plan.txt section 9.1. */
typedef struct {
    float        x, y, z;
    unsigned int color;
    unsigned int specular;
    float        tu, tv;
} RVertex;

/* 0 on failure, having printed why. Safe to call once. */
int  r_init(HWND hwnd, int clientW, int clientH);
void r_shutdown(void);
int  r_ready(void);

/* Call on WM_SIZE. A lost or resized device is reset here; vertices are
 * submitted from ordinary memory so there is nothing to recreate. */
void r_resize(int clientW, int clientH);

/* fovY in radians. Builds the projection from the current back buffer
 * aspect, so call it after r_resize. */
void r_set_camera(const float *eye3, const float *at3, const float *up3,
                  float fovY, float znear, float zfar);

int  r_begin(unsigned int clearARGB);
void r_end(void);

/* Indexed triangle list from ordinary memory. No vertex buffer is
 * created, so a device reset costs nothing. */
void r_draw(const RVertex *verts, int nverts,
            const unsigned short *indices, int nindices);

void r_set_wireframe(int on);

/*
 * Upload w*h texels of N64-order RGBA bytes, as the arrays in
 * textures.h hold them, as the single texture.
 *
 * D3DPOOL_MANAGED, so the texture survives a device Reset and
 * r_resize still has nothing to recreate.
 */
int  r_texture_upload(const unsigned char *rgba, int w, int h);
void r_texture_bind(int on);

/* Flip the sampler between point and linear, and report what the
 * device actually took. Visible triangle facets mean point. */
void r_set_point_filter(int on);

/* 0 the N64's mode, 1 depth write, 2 opaque, 3 both. For chasing
 * the triangle-edge artefact. */
void r_set_blend_mode(int mode);

/*
 * Screen-space text, for the fps readout.
 *
 * The atlas is built from a fixed-pitch GDI font at startup rather
 * than shipped as data. MOTORACER's font system was the obvious
 * thing to reuse and is the wrong shape for this: it writes pixels
 * into a software framebuffer and loads its glyph art from that
 * game's own font assets.
 *
 * r_text uses a pre-transformed vertex format, which skips the
 * frustum clip. That is fine here and only here: the quads are
 * screen aligned and always on screen.
 */
int  r_font_init(int pixelHeight);
void r_text(int x, int y, unsigned int argb, const char *s);
int  r_font_height(void);
int  r_font_width(void);

#ifdef __cplusplus
}
#endif

#endif /* WATER_D3D9RT_H */
