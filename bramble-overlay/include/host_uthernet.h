#ifndef HOST_UTHERNET_H
#define HOST_UTHERNET_H

#include <stdint.h>

void host_u2_init(void);
int host_u2_read(uint8_t nibble, uint8_t *out);
int host_u2_write(uint8_t nibble, uint8_t wdata);
void host_u2_poll(void);
void host_u2_on_tap_frame(const uint8_t *eth, int len);

#endif
