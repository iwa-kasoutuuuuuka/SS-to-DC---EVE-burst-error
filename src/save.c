#include "save.h"
#include <kos.h>
#include <dc/vmufs.h>
#include <dc/vmu_pkg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 32x32 4bpp icon: all pixels use palette entry 0 (opaque dark blue). */
static uint8_t icon_blank[512];

static maple_device_t *find_vmu(void) {
    return maple_enum_type(0, MAPLE_FUNC_MEMCARD);
}

static void slot_name(int slot, char *out, int cap) {
    snprintf(out, cap, "EVEBE_0%c", (char)('1' + slot));   /* EVEBE_01 .. EVEBE_03 */
}

int save_write(int slot, const save_data_t *d) {
    maple_device_t *dev = find_vmu();
    if (!dev) return SAVE_NO_VMU;

    vmu_pkg_t pkg;
    memset(&pkg, 0, sizeof(pkg));
    snprintf(pkg.desc_short, sizeof(pkg.desc_short), "EVE burst error %c", (char)('1' + slot));
    snprintf(pkg.desc_long, sizeof(pkg.desc_long), "EVE burst error  %s", d->cur_script);
    strncpy(pkg.app_id, "EVE-DC", sizeof(pkg.app_id) - 1);
    pkg.icon_cnt = 1;
    pkg.icon_anim_speed = 1;
    pkg.eyecatch_type = VMUPKG_EC_NONE;
    pkg.icon_pal[0] = 0xF048;          /* ARGB4444: opaque dark blue */
    pkg.icon_data = icon_blank;
    pkg.data_len = (int)sizeof(*d);
    pkg.data = (const uint8_t *)d;

    uint8_t *buf = NULL;
    int len = 0;
    if (vmu_pkg_build(&pkg, &buf, &len) < 0 || !buf) return SAVE_IO_ERROR;

    char fn[16];
    slot_name(slot, fn, sizeof(fn));
    int rc = vmufs_write(dev, fn, buf, len, VMUFS_OVERWRITE);
    free(buf);
#ifndef NDEBUG
    printf("SAVE: slot %d -> %s (%d bytes) rc=%d\n", slot + 1, fn, len, rc);
#endif
    return rc < 0 ? SAVE_IO_ERROR : SAVE_OK;
}

int save_read(int slot, save_data_t *d) {
    maple_device_t *dev = find_vmu();
    if (!dev) return SAVE_NO_VMU;

    char fn[16];
    slot_name(slot, fn, sizeof(fn));
    void *buf = NULL;
    int len = 0;
    if (vmufs_read(dev, fn, &buf, &len) < 0 || !buf) return SAVE_NO_DATA;

    vmu_pkg_t pkg;
    int rc = SAVE_NO_DATA;
    if (vmu_pkg_parse((uint8_t *)buf, (size_t)len, &pkg) == 0 &&
        pkg.data_len >= (int)sizeof(*d) &&
        memcmp(pkg.data, SAVE_MAGIC, 4) == 0) {
        memcpy(d, pkg.data, sizeof(*d));
        if (d->version == SAVE_VERSION) rc = SAVE_OK;
    }
    free(buf);
#ifndef NDEBUG
    printf("LOAD: slot %d <- %s rc=%d\n", slot + 1, fn, rc);
#endif
    return rc;
}

int save_peek(int slot, char *name, int cap) {
    static save_data_t tmp;
    name[0] = '\0';
    if (save_read(slot, &tmp) != SAVE_OK) return 0;
    strncpy(name, tmp.cur_script, cap - 1);
    name[cap - 1] = '\0';
    return 1;
}
