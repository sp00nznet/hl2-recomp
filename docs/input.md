# Input, and why there is no controller

The menu draws its background and nothing else; the loader's attract loop lays
out text and never advances. Both are waiting on the same thing, and this is
what is actually in the way.

## The chain, as the title walks it

`XInitDevices` (`sub_00620363`, reached through the thunk at `0x00620FE7` from
`sub_00595440`) walks a driver table and calls each entry's init:

```
mov eax, 0x620034        ; table start
mov esi, 0x620040        ; table end -- three entries
...
mov eax, [edi]           ; driver object
call dword ptr [eax + 4] ; driver->Init(ctx)
```

The table is populated statically in the image, and every target is translated:

```
[0] @00620034 = 006200F8   init = 00620158
[1] @00620038 = 00620110   init = 00620158
[2] @0062003C = 00620140   init = 00620A53   <- the USB one
```

`sub_00620A53` allocates the USB device table with the pool tag `'USBH'`
(`0x48425355`) -- 6 or 14 entries of 64 bytes depending on the config byte at
`0x00620044` -- and starts a `KeInitializeTimerEx` poll. It looked orphaned
(`called_by: []`) only because that indirect call is its sole caller.

The hardware registration is `sub_00620896`, and it gates on the console
revision before doing anything:

```
mov  eax, [0x6262ac]        ; XboxHardwareInfo
cmp  byte ptr [eax + 5], 0xa1   ; McpRevision
je   done                   ; 0xA1 -> skip USB entirely
mov  byte ptr [ebp - 0x18], 3
mov  dword ptr [ebp - 0x10], 0x1000        ; length, 4 KB
mov  dword ptr [ebp - 0x14], 0xfed00000    ; OHCI base
call dword ptr [0x6262ec]   ; HalGetInterruptVector(1, &irql)
```

This runtime reports `McpRevision = 0xB1`, so the check passes and the driver
proceeds. That is worth knowing in both directions: reporting `0xA1` would make
the title skip USB silently and look identical to every other failure here.

## What is actually missing

The registration says the rest: a 4 KB OHCI register block at `0xFED00000`, and
an interrupt vector. The runtime maps the whole MCPX span as plain zeroed RAM:

```
MCPX device aperture: 8 MB at Xbox VA 0xFE000000 (APU/AC97/USB/NIC, zeroed)
```

So the driver initialises, reads `HcRevision` as 0, finds a controller that
claims no root-hub ports, and enumerates nothing. Nothing is missing from the
translation -- this is a device that does not exist.

## The two ways to fix it

**Replace XPP's input API.** The surface is small: eight XPP functions are
called from `.text` at all.

| VA | Size | `ret` | What |
|---|---|---|---|
| `0x00620E64` | 86 | `0x10` | `XInputOpen` -- `push 0x57` and return 0 on failure |
| `0x00620EBA` | 12 | `4` | `XInputClose` |
| `0x00620EC6` | 115 | -- | takes a handle, `[handle+0xa3]` |
| `0x00620F39` | 51 | `8` | writes `[arg2+0x40]`, so a feedback struct rather than `XINPUT_STATE` |
| `0x00620F6C` | 89 | -- | takes a handle |
| `0x00620FE7` | 5 | -- | thunk to `XInitDevices` |
| `0x00620FEC` | 34 | `4` | latches pending device changes |
| `0x0062100E` | 109 | `0xC` | `XGetDeviceChanges` -- three args, two out-pointers |

Tempting, and a trap. Faking a handle out of `XInputOpen` means every other
entry that dereferences it at `handle+0xa3` has to be faked too, so it is all
eight or none, and it is per-title guesswork about XDK struct internals that
are not documented anywhere.

**Model the controller.** More work, and it is the one that generalises: every
Xbox title reaches its pad through this same statically-linked stack, so a
controller that exists is worth it once rather than eight guesses per title.

## What an OHCI model needs here

1. **The register block** at `0xFED00000`, 4 KB, with the read-only fields real:
   `HcRevision = 0x10`, `HcRhDescriptorA` reporting downstream ports, and root
   hub port status with a device connected.
2. **Bus-master list processing.** A real host controller reads its endpoint and
   transfer descriptors out of RAM rather than being fed them through MMIO, so
   the natural shape here is a background thread walking guest memory -- the
   same shape as the existing NV2A busy-bit acknowledger, and the pattern
   `xbox_memory_layout.c` already recommends over emulating a device.
3. **A device.** Standard control transfers (`GET_DESCRIPTOR`, `SET_ADDRESS`,
   `SET_CONFIGURATION`) and the Xbox gamepad's interrupt-IN report.
4. **Interrupt delivery.** The driver took a vector from
   `HalGetInterruptVector`, which means completion is signalled by an IRQ. This
   runtime has no path that calls a title's registered ISR, and that is likely
   the hardest part rather than the descriptors.

Two smaller things stand in the way of even seeing what the driver asks for.
MMIO here is plain memory, so reads are invisible; and `apu_hook_handle_mmio`
exists but nothing calls it -- the VEH routing was never wired up in this title,
so the APU trap under `RECOMP_AC97_READY` protects pages nothing services.
Wiring that once gives the register trace that decides whether the driver polls
`HcInterruptStatus` or genuinely waits on the IRQ, and that answer decides how
much of point 4 is needed.

## Provenance

NoRain211's doaxbv-re has an `ohci_model.c` that solves this problem, and it is
GPL-3.0 while this toolkit is MIT. It is not to be read or adapted -- anything
written here is from the OHCI 1.0a specification and this title's own code.
