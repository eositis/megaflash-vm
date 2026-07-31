#ifndef A2BUS_H
#define A2BUS_H

#include <stdint.h>

/* MegaFlash Apple II slot-4 bus GPIO map (RP2350 a2bus_rp2350.pio). */
#define A2BUS_GPIO_A0       6
#define A2BUS_GPIO_A3       9
#define A2BUS_GPIO_RNW      10
#define A2BUS_GPIO_D0       11
#define A2BUS_GPIO_PHI0     19
#define A2BUS_GPIO_NDEVSEL  20
#define A2BUS_GPIO_BUS_BASE 6
#define A2BUS_GPIO_BUS_WIDTH 13

/* Bit 4 of listener FIFO word = read (R/nW high). */
#define A2BUS_READ_FLAG     (1u << 4)

void a2bus_gpio_idle(void);
void a2bus_phi0_pulse_for_detect(void);
void a2bus_inject_read(uint8_t addr_nibble);
void a2bus_inject_write(uint8_t addr_nibble, uint8_t data);
void a2bus_pio_burst(unsigned steps);

#endif /* A2BUS_H */
