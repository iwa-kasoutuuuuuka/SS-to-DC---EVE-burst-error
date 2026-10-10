#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>
#include <kos.h>

/* Initialize Input System */
void input_init();

/* Poll all connected controllers */
uint32_t input_poll();

/* Wait for a specific button release or time */
void input_wait_release(uint32_t button);

#endif /* INPUT_H */
