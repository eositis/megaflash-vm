/*
 * Strong bramble_ext_* symbols for MegaFlash Apple-bus overlay.
 */
#include "bramble_ext.h"
#include "a2bus.h"
#include "a2bus_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Script event types used by overlay (devtools keeps 0..6 for stock). */
#define EXT_SCRIPT_A2PHI   102
#define EXT_SCRIPT_A2READ  103
#define EXT_SCRIPT_A2WRITE 104

static int ext_bridge_port;
static uint32_t ext_regs_addr;

int bramble_ext_parse_arg(int *argi, int argc, char **argv)
{
    int i = *argi;
    if (strcmp(argv[i], "-a2bus-bridge") == 0) {
        ext_bridge_port = 19765;
        if (i + 1 < argc && argv[i + 1][0] != '-') {
            ext_bridge_port = atoi(argv[++i]);
            if (ext_bridge_port <= 0) {
                ext_bridge_port = 19765;
            }
        }
        *argi = i;
        return 1;
    }
    if (strcmp(argv[i], "-a2bus-regs") == 0) {
        if (i + 1 < argc) {
            ext_regs_addr = (uint32_t)strtoul(argv[++i], NULL, 0);
            *argi = i;
            return 1;
        }
    }
    return 0;
}

void bramble_ext_post_init(const char *symbols_path)
{
    if (ext_bridge_port <= 0) {
        return;
    }
    if (ext_regs_addr == 0 && symbols_path) {
        uint32_t resolved = a2bus_bridge_regs_from_elf(symbols_path);
        if (resolved) {
            ext_regs_addr = resolved;
            fprintf(stderr, "[A2Bus] registers @ 0x%08X from %s\n",
                    ext_regs_addr, symbols_path);
        }
    }
    if (ext_regs_addr != 0) {
        a2bus_bridge_set_regs_addr(ext_regs_addr);
    }
    a2bus_bridge_set_port(ext_bridge_port);
    a2bus_bridge_set_pump(NULL);
    if (a2bus_bridge_init() < 0) {
        fprintf(stderr, "[A2Bus] bridge init failed\n");
        exit(1);
    }
}

int bramble_ext_active(void)
{
    return a2bus_bridge_active();
}

void bramble_ext_poll(void)
{
    if (a2bus_bridge_active()) {
        a2bus_bridge_poll();
    }
}

void bramble_ext_cleanup(void)
{
    a2bus_bridge_cleanup();
}

int bramble_ext_script_use_wallclock(void)
{
    return a2bus_bridge_active();
}

int bramble_ext_script_parse(const char *cmd, const char *arg,
                             int *type, int *channel, int *gpio_val)
{
    if (strcmp(cmd, "a2phi") == 0) {
        *type = EXT_SCRIPT_A2PHI;
        return 1;
    }
    if (strcmp(cmd, "a2read") == 0) {
        *type = EXT_SCRIPT_A2READ;
        *channel = (int)strtoul(arg, NULL, 0);
        return 1;
    }
    if (strcmp(cmd, "a2write") == 0) {
        unsigned addr = 0, data = 0;
        if (sscanf(arg, "%u %u", &addr, &data) >= 2) {
            *type = EXT_SCRIPT_A2WRITE;
            *channel = (int)addr;
            *gpio_val = (int)data;
            return 1;
        }
    }
    return 0;
}

int bramble_ext_script_run(int type, int channel, int gpio_val)
{
    if (type == EXT_SCRIPT_A2PHI) {
        a2bus_phi0_pulse_for_detect();
        return 1;
    }
    if (type == EXT_SCRIPT_A2READ) {
        a2bus_inject_read((uint8_t)channel);
        return 1;
    }
    if (type == EXT_SCRIPT_A2WRITE) {
        a2bus_inject_write((uint8_t)channel, (uint8_t)gpio_val);
        return 1;
    }
    return 0;
}

/* Help text fragment for -h */
void bramble_ext_print_help(void)
{
    fprintf(stderr, "  -a2bus-bridge [port]  MegaFlash Apple-bus TCP bridge (default 19765)\n");
    fprintf(stderr, "  -a2bus-regs <addr>    Guest BSS address of MegaFlash registers\n");
}
