#include "a2bus.h"
#include "gpio.h"
#include "pio.h"
#include "usb.h"

static void a2bus_set_bus_pins(uint32_t busdata)
{
    for (int i = 0; i < A2BUS_GPIO_BUS_WIDTH; i++) {
        uint8_t pin = (uint8_t)(A2BUS_GPIO_BUS_BASE + i);
        gpio_set_input_pin(pin, (busdata >> i) & 1u);
    }
}

void a2bus_gpio_idle(void)
{
    gpio_set_input_pin(A2BUS_GPIO_NDEVSEL, 1);
    gpio_set_input_pin(A2BUS_GPIO_PHI0, 0);
    a2bus_set_bus_pins(0);
}

/* Deliver one 13-bit listener word: A3-A0 + R/nW + D7-D0.
 * Must use the full 13 bits — masking to 5 bits dropped write data so every
 * CMD write looked like CMD_RESETBOTHPTRS (0) and CMD_GETDEVINFO never ran.
 *
 * Do not also run a nDEVSEL sample cycle: SM0 push noblock would enqueue a
 * second word and leftover FIFO entries later decode as MFERR_UNKNOWNCMD. */
static void a2bus_notify_listener(uint32_t busdata)
{
    pio_inject_rx(0, 0, busdata & 0x1FFFu);
}

void a2bus_phi0_pulse_for_detect(void)
{
    /* IsAppleConnected() samples PHI0 for an edge in a tight loop. */
    if (usb_console_tcp_active()) {
        /* USB UserTerminal needs Apple offline (Release: stdio_usb && !IsAppleConnected). */
        gpio_set_input_pin(A2BUS_GPIO_PHI0, 0);
        return;
    }
    gpio_set_input_pin(A2BUS_GPIO_PHI0, 0);
    a2bus_pio_burst(4);
    gpio_set_input_pin(A2BUS_GPIO_PHI0, 1);
    a2bus_pio_burst(4);
    gpio_set_input_pin(A2BUS_GPIO_PHI0, 0);
    a2bus_pio_burst(4);
}

static void a2bus_run_slot_cycle(uint32_t busdata)
{
    /* Keep nDEVSEL high so a2buslistener never GPIO-samples; inject exactly
     * one FIFO word for BusLoop's GetAppleBusBlocking(). Drain first so
     * boot/PHI noise leftovers cannot decode as bogus CMD writes. */
    gpio_set_input_pin(A2BUS_GPIO_NDEVSEL, 1);
    gpio_set_input_pin(A2BUS_GPIO_PHI0, 0);
    a2bus_set_bus_pins(busdata);
    pio_drain_rx(0, 0);
    a2bus_notify_listener(busdata);
}

void a2bus_inject_read(uint8_t addr_nibble)
{
    uint32_t busdata = (addr_nibble & 0xFu) | A2BUS_READ_FLAG;
    a2bus_run_slot_cycle(busdata);
}

void a2bus_inject_write(uint8_t addr_nibble, uint8_t data)
{
    uint32_t busdata = (addr_nibble & 0xFu) | ((uint32_t)data << 5);
    a2bus_run_slot_cycle(busdata);
}

void a2bus_pio_burst(unsigned steps)
{
    for (unsigned i = 0; i < steps; i++) {
        pio_step();
    }
}
