/*
 * d3d9rt.cpp -- minimal Direct3D 9 device. See d3d9rt.h for why each
 * piece is the way it is.
 *
 * No D3DX. The two matrices this needs are built by hand, which keeps
 * the program free of the D3DX9_43.dll redistributable.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <d3d9.h>

#include "d3d9rt.h"

#define R_FVF (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1)

typedef IDirect3D9 * (WINAPI *PFN_Direct3DCreate9)(UINT);

static HMODULE               s_d3d9dll;
static IDirect3D9           *s_d3d;
static IDirect3DDevice9     *s_dev;
static D3DPRESENT_PARAMETERS s_pp;
static HWND                  s_hwnd;
static int                   s_ready;
static int                   s_inScene;
static int                   s_wire;
static IDirect3DTexture9    *s_tex;
static int                   s_texBound;
static IDirect3DTexture9    *s_font;
static int                   s_pointFilter;

/*
 * Render experiments for the triangle-edge artefact. 0 is the N64's own
 * mode. 1 writes depth, so an overlapping triangle cannot blend twice
 * over the same pixel. 2 ignores the texture alpha, which takes the
 * blend out of it entirely. 3 does both.
 */
static int                   s_blendMode;

static void logf_(const char *fmt, ...)
{
    char    buf[512];
    va_list ap;

    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    buf[sizeof(buf) - 1] = 0;
    va_end(ap);
    fputs(buf, stdout);
}

/* Ask the device what depth format it will actually accept, rather than
 * assuming one. */
static D3DFORMAT choose_depth_format(D3DFORMAT adapterFormat)
{
    static const D3DFORMAT order[] = {
        D3DFMT_D24S8, D3DFMT_D24X8, D3DFMT_D16
    };
    int i;

    for (i = 0; i < (int)(sizeof(order) / sizeof(order[0])); i++) {
        if (FAILED(s_d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT,
                                            D3DDEVTYPE_HAL, adapterFormat,
                                            D3DUSAGE_DEPTHSTENCIL,
                                            D3DRTYPE_SURFACE, order[i])))
            continue;
        if (FAILED(s_d3d->CheckDepthStencilMatch(D3DADAPTER_DEFAULT,
                                                 D3DDEVTYPE_HAL,
                                                 adapterFormat, adapterFormat,
                                                 order[i])))
            continue;
        return order[i];
    }
    return D3DFMT_UNKNOWN;
}

/* Everything that has to be re-established after a create or a Reset. */
static void apply_initial_state(void)
{
    if (!s_dev)
        return;

    s_dev->SetFVF(R_FVF);
    s_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    s_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    /*
     * G_RM_AA_ZB_XLU_SURF, which is what make_sea_glist selects for the
     * water: othermode low 0x005049D8 has Z_CMP set and Z_UPD clear,
     * and its blender is CLR_IN*A_IN + CLR_MEM*(1-A_IN).
     *
     * The water is a TRANSLUCENT surface over the background, not an
     * opaque one. Its alpha comes per-texel from the texture and runs
     * as low as 71 of 255, so a good part of what the eye reads as
     * water is really the sky behind it. Lose this and the sheen goes
     * with it, however right the texture coordinates are.
     */
    s_dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    s_dev->SetRenderState(D3DRS_ZWRITEENABLE,
                          (s_blendMode & 1) ? TRUE : FALSE);
    s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE,
                          (s_blendMode & 2) ? FALSE : TRUE);
    s_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    s_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    s_dev->SetRenderState(D3DRS_FILLMODE,
                          s_wire ? D3DFILL_WIREFRAME : D3DFILL_SOLID);

    /* The sea combiner is 0xFCFFFFFF / 0xFFFCF279, which decodes to
     * RGB = TEXEL0 and A = TEXEL0_A in both cycles: the texture alone,
     * with no shade and no environment term. Modulating by DIFFUSE here
     * would be a divergence. */
    s_dev->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
    s_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    s_dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
    s_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    s_dev->SetTextureStageState(1, D3DTSS_COLOROP,   D3DTOP_DISABLE);
    s_dev->SetTextureStageState(1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE);

    s_dev->SetSamplerState(0, D3DSAMP_MINFILTER,
                           s_pointFilter ? D3DTEXF_POINT : D3DTEXF_LINEAR);
    s_dev->SetSamplerState(0, D3DSAMP_MAGFILTER,
                           s_pointFilter ? D3DTEXF_POINT : D3DTEXF_LINEAR);
    /* One level, so this only matters as insurance against a driver
     * default of anything but NONE. */
    s_dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    /* G_SETTILE 0x00080200: cm_s and cm_t are both CLAMP with mask 0.
     * A sphere map must not tile - wrapping puts a seam across every
     * steep wave face. */
    s_dev->SetSamplerState(0, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP);
    s_dev->SetSamplerState(0, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP);
}

/*
 * What the device actually has, rather than what was asked for.
 *
 * Visible triangle facets on the water are the signature of point
 * sampling: with LINEAR the sphere map renders smooth at any amplitude,
 * with POINT each triangle flattens to one texel. So if the readback
 * below does not say 2, that is the whole story.
 */
static void log_filter_state(const char *when)
{
    D3DCAPS9 caps;
    DWORD    mn = 0, mg = 0, mip = 0, au = 0, av = 0;

    if (!s_dev)
        return;
    if (SUCCEEDED(s_dev->GetDeviceCaps(&caps)))
        logf_("d3d9: caps magflinear %s, minflinear %s\n",
              (caps.TextureFilterCaps & D3DPTFILTERCAPS_MAGFLINEAR)
              ? "yes" : "NO",
              (caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFLINEAR)
              ? "yes" : "NO");
    s_dev->GetSamplerState(0, D3DSAMP_MINFILTER, &mn);
    s_dev->GetSamplerState(0, D3DSAMP_MAGFILTER, &mg);
    s_dev->GetSamplerState(0, D3DSAMP_MIPFILTER, &mip);
    s_dev->GetSamplerState(0, D3DSAMP_ADDRESSU, &au);
    s_dev->GetSamplerState(0, D3DSAMP_ADDRESSV, &av);
    logf_("d3d9: %s sampler0 min %lu mag %lu mip %lu addr %lu/%lu"
          "  (1=point 2=linear, addr 3=clamp)\n",
          when, (unsigned long)mn, (unsigned long)mg, (unsigned long)mip,
          (unsigned long)au, (unsigned long)av);
}

void r_set_blend_mode(int mode)
{
    static const char *name[4] = {
        "N64: blend, no depth write",
        "depth write on",
        "opaque, no blend",
        "depth write + opaque"
    };

    s_blendMode = mode & 3;
    apply_initial_state();
    logf_("blend mode %d: %s\n", s_blendMode, name[s_blendMode]);
}

void r_set_point_filter(int on)
{
    s_pointFilter = on ? 1 : 0;
    if (!s_dev)
        return;
    s_dev->SetSamplerState(0, D3DSAMP_MINFILTER,
                           s_pointFilter ? D3DTEXF_POINT : D3DTEXF_LINEAR);
    s_dev->SetSamplerState(0, D3DSAMP_MAGFILTER,
                           s_pointFilter ? D3DTEXF_POINT : D3DTEXF_LINEAR);
    log_filter_state(s_pointFilter ? "point :" : "linear:");
}

static void fill_present_params(int clientW, int clientH,
                                D3DFORMAT depthFormat)
{
    ZeroMemory(&s_pp, sizeof(s_pp));
    s_pp.Windowed               = TRUE;
    s_pp.SwapEffect             = D3DSWAPEFFECT_DISCARD;
    s_pp.BackBufferFormat       = D3DFMT_UNKNOWN;   /* desktop format */
    s_pp.BackBufferWidth        = (UINT)(clientW > 0 ? clientW : 1);
    s_pp.BackBufferHeight       = (UINT)(clientH > 0 ? clientH : 1);
    s_pp.BackBufferCount        = 1;
    s_pp.hDeviceWindow          = s_hwnd;
    s_pp.EnableAutoDepthStencil = (depthFormat != D3DFMT_UNKNOWN);
    s_pp.AutoDepthStencilFormat = depthFormat;
    s_pp.PresentationInterval   = D3DPRESENT_INTERVAL_ONE;
}

int r_init(HWND hwnd, int clientW, int clientH)
{
    PFN_Direct3DCreate9 create;
    D3DDISPLAYMODE      mode;
    D3DFORMAT           depth;
    HRESULT             hr;

    s_hwnd = hwnd;

    s_d3d9dll = LoadLibraryA("d3d9.dll");
    if (!s_d3d9dll) {
        logf_("d3d9: d3d9.dll not present\n");
        return 0;
    }
    create = (PFN_Direct3DCreate9)GetProcAddress(s_d3d9dll,
                                                 "Direct3DCreate9");
    if (!create) {
        logf_("d3d9: Direct3DCreate9 missing from d3d9.dll\n");
        return 0;
    }
    s_d3d = create(D3D_SDK_VERSION);
    if (!s_d3d) {
        logf_("d3d9: Direct3DCreate9 returned null "
              "(SDK version mismatch?)\n");
        return 0;
    }
    if (FAILED(s_d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &mode))) {
        logf_("d3d9: GetAdapterDisplayMode failed\n");
        return 0;
    }

    depth = choose_depth_format(mode.Format);
    if (depth == D3DFMT_UNKNOWN)
        logf_("d3d9: no depth format accepted; running without a z buffer\n");

    fill_present_params(clientW, clientH, depth);

    hr = s_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                             D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                             &s_pp, &s_dev);
    if (FAILED(hr)) {
        logf_("d3d9: CreateDevice failed (0x%08lx)\n", (unsigned long)hr);
        return 0;
    }

    apply_initial_state();
    log_filter_state("init  :");
    s_ready = 1;
    logf_("d3d9: device up, %dx%d, depth %s\n", clientW, clientH,
          depth == D3DFMT_D24S8 ? "D24S8" :
          depth == D3DFMT_D24X8 ? "D24X8" :
          depth == D3DFMT_D16   ? "D16"   : "none");
    return 1;
}

void r_shutdown(void)
{
    if (s_font) {
        s_font->Release();
        s_font = NULL;
    }
    if (s_tex) {
        s_tex->Release();
        s_tex = NULL;
    }
    if (s_dev) {
        s_dev->Release();
        s_dev = NULL;
    }
    if (s_d3d) {
        s_d3d->Release();
        s_d3d = NULL;
    }
    if (s_d3d9dll) {
        FreeLibrary(s_d3d9dll);
        s_d3d9dll = NULL;
    }
    s_ready = 0;
}

int r_ready(void)
{
    return s_ready && s_dev != NULL;
}

void r_resize(int clientW, int clientH)
{
    D3DDISPLAYMODE mode;

    if (!s_dev || clientW <= 0 || clientH <= 0)
        return;
    if ((UINT)clientW == s_pp.BackBufferWidth &&
        (UINT)clientH == s_pp.BackBufferHeight)
        return;

    if (FAILED(s_d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &mode)))
        return;

    fill_present_params(clientW, clientH, choose_depth_format(mode.Format));
    if (FAILED(s_dev->Reset(&s_pp))) {
        logf_("d3d9: Reset to %dx%d failed\n", clientW, clientH);
        s_ready = 0;
        return;
    }
    apply_initial_state();
    s_ready = 1;
}

/* ------------------------------------------------------------------ */
/* matrices, by hand                                                   */
/* ------------------------------------------------------------------ */

static void vsub(float *d, const float *a, const float *b)
{
    d[0] = a[0] - b[0];
    d[1] = a[1] - b[1];
    d[2] = a[2] - b[2];
}

static void vcross(float *d, const float *a, const float *b)
{
    d[0] = a[1] * b[2] - a[2] * b[1];
    d[1] = a[2] * b[0] - a[0] * b[2];
    d[2] = a[0] * b[1] - a[1] * b[0];
}

static float vdot(const float *a, const float *b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void vnorm(float *v)
{
    float l = (float)sqrt(vdot(v, v));

    if (l > 1e-6f) {
        v[0] /= l;
        v[1] /= l;
        v[2] /= l;
    }
}

void r_set_camera(const float *eye3, const float *at3, const float *up3,
                  float fovY, float znear, float zfar)
{
    D3DMATRIX view, proj;
    float     zax[3], xax[3], yax[3], h, w, q;

    if (!s_dev)
        return;

    /* left-handed look-at */
    vsub(zax, at3, eye3);
    vnorm(zax);
    vcross(xax, up3, zax);
    vnorm(xax);
    vcross(yax, zax, xax);

    memset(&view, 0, sizeof(view));
    view._11 = xax[0]; view._12 = yax[0]; view._13 = zax[0];
    view._21 = xax[1]; view._22 = yax[1]; view._23 = zax[1];
    view._31 = xax[2]; view._32 = yax[2]; view._33 = zax[2];
    view._41 = -vdot(xax, eye3);
    view._42 = -vdot(yax, eye3);
    view._43 = -vdot(zax, eye3);
    view._44 = 1.0f;

    h = 1.0f / (float)tan(fovY * 0.5f);
    w = h * (float)s_pp.BackBufferHeight / (float)s_pp.BackBufferWidth;
    q = zfar / (zfar - znear);

    memset(&proj, 0, sizeof(proj));
    proj._11 = w;
    proj._22 = h;
    proj._33 = q;
    proj._34 = 1.0f;
    proj._43 = -znear * q;

    s_dev->SetTransform(D3DTS_VIEW, &view);
    s_dev->SetTransform(D3DTS_PROJECTION, &proj);
}

/* ------------------------------------------------------------------ */
/* frame                                                               */
/* ------------------------------------------------------------------ */

int r_begin(unsigned int clearARGB)
{
    HRESULT hr;

    if (!s_dev)
        return 0;

    /* A lost device is normal: minimise, lock, resolution change. Try to
     * get it back each frame and draw nothing until it comes. */
    hr = s_dev->TestCooperativeLevel();
    if (hr == D3DERR_DEVICENOTRESET) {
        if (SUCCEEDED(s_dev->Reset(&s_pp))) {
            apply_initial_state();
            s_ready = 1;
        }
        return 0;
    }
    if (FAILED(hr)) {
        s_ready = 0;
        return 0;
    }

    s_dev->Clear(0, NULL,
                 D3DCLEAR_TARGET |
                 (s_pp.EnableAutoDepthStencil ? D3DCLEAR_ZBUFFER : 0),
                 (D3DCOLOR)clearARGB, 1.0f, 0);
    if (FAILED(s_dev->BeginScene()))
        return 0;
    s_inScene = 1;
    return 1;
}

void r_end(void)
{
    if (!s_dev || !s_inScene)
        return;
    s_dev->EndScene();
    s_inScene = 0;
    s_dev->Present(NULL, NULL, NULL, NULL);
}

void r_draw(const RVertex *verts, int nverts,
            const unsigned short *indices, int nindices)
{
    if (!s_dev || !s_inScene || nverts <= 0 || nindices < 3)
        return;
    s_dev->SetFVF(R_FVF);
    s_dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, (UINT)nverts,
                                  (UINT)(nindices / 3), indices,
                                  D3DFMT_INDEX16, verts, sizeof(RVertex));
}

void r_set_wireframe(int on)
{
    s_wire = on ? 1 : 0;
    if (s_dev)
        s_dev->SetRenderState(D3DRS_FILLMODE,
                              s_wire ? D3DFILL_WIREFRAME : D3DFILL_SOLID);
}

/*
 * Upload w*h texels of N64-order RGBA bytes as an A8R8G8B8 texture.
 *
 * The source byte order is the ROM's, R G B A per texel; D3DFMT_A8R8G8B8
 * is a little-endian word, so a texel is stored B G R A. The swizzle is
 * per channel below rather than a word rotate, so it does not depend on
 * the host's byte order.
 */
int r_texture_upload(const unsigned char *rgba, int w, int h)
{
    IDirect3DTexture9 *tex = NULL;
    D3DLOCKED_RECT     lr;
    int                x, y;

    if (!s_dev || !rgba || w <= 0 || h <= 0)
        return 0;

    if (FAILED(s_dev->CreateTexture((UINT)w, (UINT)h, 1, 0,
                                    D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                    &tex, NULL))) {
        logf_("texture: CreateTexture %dx%d failed\n", w, h);
        return 0;
    }
    if (FAILED(tex->LockRect(0, &lr, NULL, 0))) {
        logf_("texture: LockRect failed\n");
        tex->Release();
        return 0;
    }
    /* Pitch is the driver's stride and need not equal w*4. */
    for (y = 0; y < h; y++) {
        unsigned int        *dst = (unsigned int *)((char *)lr.pBits
                                                    + (size_t)y * lr.Pitch);
        const unsigned char *src = rgba + (size_t)y * w * 4;

        for (x = 0; x < w; x++) {
            unsigned int r = src[x * 4 + 0];
            unsigned int g = src[x * 4 + 1];
            unsigned int b = src[x * 4 + 2];
            unsigned int a = src[x * 4 + 3];

            dst[x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    tex->UnlockRect(0);

    if (s_tex)
        s_tex->Release();
    s_tex = tex;
    return 1;
}

void r_texture_bind(int on)
{
    if (!s_dev)
        return;
    s_texBound = (on && s_tex) ? 1 : 0;
    s_dev->SetTexture(0, s_texBound ? s_tex : NULL);
}

/* ------------------------------------------------------------------ */
/* screen-space text                                                   */
/* ------------------------------------------------------------------ */

/* Pre-transformed: position is already in screen space, so D3D9 does no
 * transform and no frustum clip. See d3d9rt.h for why that is acceptable
 * for the overlay and not for the water. */
#define R_FVF2 (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

struct R_Vertex2 {
    float        x, y, z, rhw;
    unsigned int color;
    float        tu, tv;
};

#define FONT_FIRST 32
#define FONT_LAST  126
#define FONT_COUNT (FONT_LAST - FONT_FIRST + 1)
#define FONT_COLS  16

static int                s_glyphW, s_glyphH;
static int                s_atlasW, s_atlasH;

/*
 * Build the glyph atlas with GDI.
 *
 * A top-down 32-bit DIB is drawn white-on-black with a fixed-pitch face,
 * then the red channel becomes the alpha of a white texel. That gives
 * antialiased text with no art to ship and no asset loader.
 */
int r_font_init(int pixelHeight)
{
    HDC          dc;
    HBITMAP      bm;
    BITMAPINFO   bi;
    HFONT        font, oldFont;
    TEXTMETRICA  tm;
    void        *bits = NULL;
    D3DLOCKED_RECT lr;
    int          i, x, y;

    if (!s_dev)
        return 0;
    if (pixelHeight < 8)
        pixelHeight = 8;

    dc = CreateCompatibleDC(NULL);
    if (!dc)
        return 0;

    /* FIXED_PITCH so the advance is one number, not a per-glyph table.
     * Consolas if present, else whatever the mapper gives for the
     * request, which on every supported Windows is a fixed face. */
    font = CreateFontA(-pixelHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                       FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                       FIXED_PITCH | FF_MODERN, "Consolas");
    if (!font) {
        DeleteDC(dc);
        return 0;
    }
    oldFont = (HFONT)SelectObject(dc, font);
    GetTextMetricsA(dc, &tm);
    s_glyphW = tm.tmMaxCharWidth;
    s_glyphH = tm.tmHeight;
    if (s_glyphW < 1) s_glyphW = pixelHeight / 2 + 1;
    if (s_glyphH < 1) s_glyphH = pixelHeight;

    s_atlasW = s_glyphW * FONT_COLS;
    s_atlasH = s_glyphH * ((FONT_COUNT + FONT_COLS - 1) / FONT_COLS);

    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = s_atlasW;
    bi.bmiHeader.biHeight      = -s_atlasH;        /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bm || !bits) {
        SelectObject(dc, oldFont);
        DeleteObject(font);
        DeleteDC(dc);
        return 0;
    }
    SelectObject(dc, bm);
    SelectObject(dc, font);

    {
        RECT all;
        all.left = 0; all.top = 0;
        all.right = s_atlasW; all.bottom = s_atlasH;
        FillRect(dc, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));

    for (i = 0; i < FONT_COUNT; i++) {
        char ch = (char)(FONT_FIRST + i);

        TextOutA(dc, (i % FONT_COLS) * s_glyphW,
                 (i / FONT_COLS) * s_glyphH, &ch, 1);
    }
    GdiFlush();

    if (FAILED(s_dev->CreateTexture((UINT)s_atlasW, (UINT)s_atlasH, 1, 0,
                                    D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                    &s_font, NULL))) {
        DeleteObject(bm);
        SelectObject(dc, oldFont);
        DeleteObject(font);
        DeleteDC(dc);
        return 0;
    }
    if (FAILED(s_font->LockRect(0, &lr, NULL, 0))) {
        s_font->Release();
        s_font = NULL;
        DeleteObject(bm);
        SelectObject(dc, oldFont);
        DeleteObject(font);
        DeleteDC(dc);
        return 0;
    }
    for (y = 0; y < s_atlasH; y++) {
        unsigned int *src = (unsigned int *)bits + (size_t)y * s_atlasW;
        unsigned int *dst = (unsigned int *)((char *)lr.pBits
                                             + (size_t)y * lr.Pitch);
        for (x = 0; x < s_atlasW; x++) {
            /* White text on black, so any channel is the coverage. */
            unsigned int a = (src[x] >> 16) & 0xFF;
            dst[x] = (a << 24) | 0x00FFFFFFu;
        }
    }
    s_font->UnlockRect(0);

    DeleteObject(bm);
    SelectObject(dc, oldFont);
    DeleteObject(font);
    DeleteDC(dc);

    logf_("font: %dx%d glyphs, atlas %dx%d\n", s_glyphW, s_glyphH,
          s_atlasW, s_atlasH);
    return 1;
}

/* The advance. The face is FIXED_PITCH, so one number does for every
 * glyph and a caller can centre a block of text by counting characters. */
int r_font_width(void)
{
    return s_glyphW;
}

int r_font_height(void)
{
    return s_glyphH;
}

void r_text(int x, int y, unsigned int argb, const char *s)
{
    struct R_Vertex2 v[4];
    int   i;
    float du, dv;

    if (!s_dev || !s_inScene || !s_font || !s)
        return;

    du = (float)s_glyphW / (float)s_atlasW;
    dv = (float)s_glyphH / (float)s_atlasH;

    s_dev->SetFVF(R_FVF2);
    s_dev->SetTexture(0, s_font);
    /* The water pass leaves stage 0 on SELECTARG1 TEXTURE, which would
     * ignore argb and draw every string in the atlas's white. The atlas
     * is white with alpha for coverage, so MODULATE by the vertex
     * colour gives argb's rgb at argb's alpha times that coverage. */
    s_dev->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_MODULATE);
    s_dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    s_dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_MODULATE);
    s_dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    s_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    s_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    s_dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    /* The glyphs are filled quads whatever the water is doing. Left on
     * wireframe they draw as four edges each, which reads as a scatter
     * of dots at this size. */
    s_dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);

    for (i = 0; s[i]; i++) {
        int c = (unsigned char)s[i];
        int g, gx, gy;
        float x0, y0, u0, v0;

        if (c < FONT_FIRST || c > FONT_LAST)
            c = '?';
        g  = c - FONT_FIRST;
        gx = g % FONT_COLS;
        gy = g / FONT_COLS;

        /* The half-pixel shift is D3D9's texel-to-pixel rule; without it
         * the glyphs blur by half a pixel. */
        x0 = (float)(x + i * s_glyphW) - 0.5f;
        y0 = (float)y - 0.5f;
        u0 = (float)gx * du;
        v0 = (float)gy * dv;

        v[0].x = x0;                     v[0].y = y0;
        v[0].tu = u0;                    v[0].tv = v0;
        v[1].x = x0 + (float)s_glyphW;   v[1].y = y0;
        v[1].tu = u0 + du;               v[1].tv = v0;
        v[2].x = x0;                     v[2].y = y0 + (float)s_glyphH;
        v[2].tu = u0;                    v[2].tv = v0 + dv;
        v[3].x = x0 + (float)s_glyphW;   v[3].y = y0 + (float)s_glyphH;
        v[3].tu = u0 + du;               v[3].tv = v0 + dv;

        v[0].z = v[1].z = v[2].z = v[3].z = 0.0f;
        v[0].rhw = v[1].rhw = v[2].rhw = v[3].rhw = 1.0f;
        v[0].color = v[1].color = v[2].color = v[3].color = argb;

        s_dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v,
                               sizeof(struct R_Vertex2));
    }

    /* Put back what the water pass expects: the texture-only combiner,
     * the depth test and the fill mode. It blends too, so the blend
     * states stay as they are. */
    s_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    s_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    s_dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    s_dev->SetRenderState(D3DRS_FILLMODE,
                          s_wire ? D3DFILL_WIREFRAME : D3DFILL_SOLID);
    s_dev->SetTexture(0, s_texBound ? s_tex : NULL);
    s_dev->SetFVF(R_FVF);
}
