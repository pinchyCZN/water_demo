/*
 * textures.h -- the embedded sea textures.
 *
 * Derived from the game's own arrays; see textures.c.
 */

#ifndef WATER_TEXTURES_H
#define WATER_TEXTURES_H

#define SEA_TEX_W     32
#define SEA_TEX_H     32
#define SEA_TEX_BYTES (SEA_TEX_W * SEA_TEX_H * 4)
#define SEA_TEX_COUNT 8

#ifdef __cplusplus
extern "C" {
#endif

extern const unsigned char sot_sea_dirt[SEA_TEX_BYTES];
extern const unsigned char sot_sea_eve[SEA_TEX_BYTES];
extern const unsigned char sot_sea_fine1[SEA_TEX_BYTES];
extern const unsigned char sot_sea_fine2[SEA_TEX_BYTES];
extern const unsigned char sot_sea_morn[SEA_TEX_BYTES];
extern const unsigned char sot_sea_night[SEA_TEX_BYTES];
extern const unsigned char sot_sea_sto[SEA_TEX_BYTES];
extern const unsigned char sot_sea_ice[SEA_TEX_BYTES];

/* Indexed the way crs_sea[] indexes sea_tex_list. Not alphabetical. */
extern const unsigned char *const sea_tex_list[SEA_TEX_COUNT];
extern const char *const         sea_tex_names[SEA_TEX_COUNT];

#ifdef __cplusplus
}
#endif

#endif /* WATER_TEXTURES_H */
