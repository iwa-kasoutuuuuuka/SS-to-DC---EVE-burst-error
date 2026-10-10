#include "input.h"

/* Initialize Input System */
void input_init() {
    /* No special initialization needed for basic Maple polling */
}

uint32_t input_poll() {
    maple_device_t *cont = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    if (!cont) return 0;
    
    cont_state_t *state = (cont_state_t *)maple_dev_status(cont);
    if (!state) return 0;
    
    return state->buttons;
}

void input_wait_release(uint32_t button) {
    while (1) {
        uint32_t btns = input_poll();
        if (!(btns & button)) break;
        thd_sleep(50);
    }
}
