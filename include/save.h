#ifndef SAVE_H
#define SAVE_H

#include <stdint.h>
#include "vm.h"

/* Game state saved to the VMU. One file per slot, EVEBE_0<n>,
 * written as a standard VMU package so the BIOS file manager lists it. */
#define SAVE_SLOTS   3
#define SAVE_MAGIC   "EVSV"
#define SAVE_VERSION 3   /* 2: + bgm track, 3: + disc (same layout, reserved -> disc) */
#define SAVE_VERSION_MIN 2

typedef struct {
    char     magic[4];
    uint16_t version;
    uint16_t route;                    /* vm_context_t.route of the running route */
    char     cur_script[VM_NAME_LEN];  /* upper-case name, e.g. "A001_2" */
    uint32_t cur_ip;                   /* byte ip to resume at (the command menu) */
    uint16_t other_started;
    uint16_t other_route;
    char     other_script[VM_NAME_LEN];
    uint32_t other_ip;
    char     bg_name[VM_NAME_LEN];
    char     other_bg[VM_NAME_LEN];
    uint16_t bgm;                      /* BGM track playing when saved (0 = none) */
    uint16_t disc;                     /* disc in the drive when saved (0 = single-disc build / v2) */
    uint16_t vars[VM_NUM_VARS];        /* shared game flags */
} save_data_t;

/* Return codes */
#define SAVE_OK        0
#define SAVE_NO_VMU   -1
#define SAVE_IO_ERROR -2
#define SAVE_NO_DATA  -3

int save_write(int slot, const save_data_t *d);
int save_read(int slot, save_data_t *d);
/* Fills name with the script saved in the slot ("" if empty). Returns 1 if present. */
int save_peek(int slot, char *name, int cap);

#endif /* SAVE_H */
