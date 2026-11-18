/* psprecomp -- mods.
 *
 * A mod is a folder under mods/ (next to settings.ini):
 *
 *   mods/<folder>/mod.ini        [mod] name, author, version, description
 *   mods/<folder>/disc/...       files that replace the game's disc files
 *                                (same path as under disc/, e.g.
 *                                disc/PSP_GAME/USRDIR/data/...)
 *   mods/<folder>/textures/<hash>.png
 *                                replaces the game texture with that hash,
 *                                at any size (UVs are normalised)
 *
 * Mods are switched on and off in the F1 menu (Mods), kept in settings.ini
 * [mods]. When two mods carry the same file or texture, the one later in
 * folder-name order wins. Texture changes apply at once; file changes when
 * the game next opens the file (a level load, or a restart).
 *
 * To find a texture's hash, turn on Mods > Dump textures: every texture the
 * game decodes is written once to dump/textures/<hash>.png. Edit a copy and
 * put it in a mod's textures folder under the same name. */
#ifndef PSPRECOMP_MODS_H
#define PSPRECOMP_MODS_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char dir[64];                 /* folder name; the settings key */
    char name[64], author[64], version[32], desc[192];
    int on;
    int files, textures;          /* what it carries: disc files, texture count */
} psp_mod_t;

void psp_mods_init(void);         /* scan mods/ (idempotent; called lazily) */
int  psp_mods_count(void);
const psp_mod_t *psp_mods_get(int i);
void psp_mods_set(int i, int on);
int  psp_mods_dump(void);         /* texture dump on */
void psp_mods_set_dump(int on);
void psp_mods_open_folder(void);
const char *psp_mods_root(void);  /* absolute path of mods/ */

/* The enabled mods' replacement for disc file rel ("PSP_GAME/..."): 1 and
 * its host path in out, or 0. */
int psp_mods_file(const char *rel, char *out, size_t cap);

/* Renderer side. Bumps whenever the enabled set changes: replaced textures
 * must be decoded again. */
unsigned psp_mods_generation(void);
/* Replacement for the texture with content hash h, or NULL (RGBA, w x h). */
const uint32_t *psp_mods_texture(uint64_t hash, int *w, int *h);
/* With dumping on, write the decoded texture once as dump/textures/<hash>.png. */
void psp_mods_dump_texture(uint64_t hash, const uint32_t *px, int w, int h);

/* Settings file path (settings.ini in the working directory). */
const char *psp_ini_path(void);
#endif
