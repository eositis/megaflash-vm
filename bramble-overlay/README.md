# Bramble Apple-bus / MegaFlash overlay

Sources linked into the **megaflash-vm** `bramble` binary (not stock Bramble):

| File | Role |
|------|------|
| `a2bus.c` / `a2bus.h` | Virtual Apple-bus GPIO + PIO inject |
| `a2bus_bridge.c` / `a2bus_bridge.h` | MAME TCP soft-switch protocol |
| `bramble_ext_a2bus.c` | Strong `bramble_ext_*` CLI / poll / script wiring |
| `megaflash_a2bus_hooks.c` | Temporary wifi/RTC/veneer/SmartPort PC stubs |

Build:

```bash
cmake -B build && make -C build bramble
```

Produces `./bramble` and `build/bramble`. The MAME launcher prefers this binary.

**Temporary:** wifi/RTC stubs go away once CYW43 is mapped to the macOS network stack.
