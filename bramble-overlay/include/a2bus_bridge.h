#ifndef A2BUS_BRIDGE_H
#define A2BUS_BRIDGE_H

#include <stdint.h>

/* MegaFlash register shadow default (pico2_debug BSS). Override with -a2bus-regs. */
#define A2BUS_REGS_DEFAULT 0x20057038u

/* Binary protocol (host byte order, little-endian hosts):
 *   C->S: op [nibble] [data]
 *     0x00 PING
 *     0x01 PHI   (Apple connect pulse)
 *     0x02 READ  + nibble (0..15)
 *     0x03 WRITE + nibble + data
 *     0x04 PEEK  + nibble
 *     0x05 U2BURST + count (0=256): W5100 DATA auto-inc bytes
 *   S->C: status (0=ok) + data
 *   U2BURST: status + count + count bytes
 */

#define A2BUS_BRIDGE_OP_PING  0x00
#define A2BUS_BRIDGE_OP_PHI   0x01
#define A2BUS_BRIDGE_OP_READ  0x02
#define A2BUS_BRIDGE_OP_WRITE 0x03
#define A2BUS_BRIDGE_OP_PEEK  0x04
#define A2BUS_BRIDGE_OP_U2BURST 0x05

typedef void (*a2bus_bridge_pump_fn)(unsigned steps);

void a2bus_bridge_set_port(int port);
void a2bus_bridge_set_regs_addr(uint32_t addr);
void a2bus_bridge_set_pump(a2bus_bridge_pump_fn fn);
int  a2bus_bridge_active(void);
int  a2bus_bridge_init(void);
void a2bus_bridge_cleanup(void);
void a2bus_bridge_poll(void);

/* Resolve "registers" from an ELF (OBJECT/BSS); 0 on failure. */
uint32_t a2bus_bridge_regs_from_elf(const char *elf_path);

/*
 * DoTestWifi / DoTFTP hold CMD BUSY for seconds while Apple polls STATUS.
 * Nesting count: while >0, a2bus_bridge must NOT clear BUSY on timeout —
 * that aborts FormatIPAddr and leaves the control panel with empty/junk IPs.
 */
void a2bus_long_cmd_begin(const char *why);
void a2bus_long_cmd_end(void);
int  a2bus_long_cmd_active(void);

#endif /* A2BUS_BRIDGE_H */
