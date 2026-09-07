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

## What is built so far

The register half, behind `RECOMP_USB`, with `RECOMP_USB_TRACE` for the access
log. Off by default: a port with no descriptor walker behind it is not yet a
controller, and a title that was working without one must keep behaving exactly
as it did.

`src/usb/ohci.c` models two controllers at `0xFED00000` and `0xFED08000`, four
downstream ports between them, and a device on HC0 port 1. The registers with
behaviour rather than storage are the ones a driver actually gets stuck on:

- `HcCommandStatus.HCR` is self-clearing. It is the first thing a driver
  writes, and a reset bit that stays set is a hang before anything else runs.
- `HcInterruptStatus` is write-1-to-clear, not a value to store.
- `HcRhPortStatus` writes are set/clear *requests by bit position*. Treating
  them as a value to store looks exactly like a port that will not enable.
- `HcFmNumber` and `HcFmRemaining` advance on their own, because a stopped
  frame counter is how a driver recognises a controller that is not running.

Reaching them needs the registers to fault rather than read out of RAM, so the
two blocks are `PAGE_NOACCESS` and the title's vectored handler routes the
fault back. That routing is new in this title: `apu_hook_handle_mmio` has
existed with no caller, which is worth knowing before trusting
`RECOMP_AC97_READY` -- it protects a page nothing services.

The decoder behind it is `src/platform/mmio_decode.h`. It existed twice already,
once in `nv2a_mmio_hook.c` and once in `apu_mmio_hook.c`, as the same opcode
table against two different pairs of accessors; this is that logic with the
accessors passed in, so this device did not add a third copy. The two originals
still carry their own and can move onto it when next touched.

`tests/mmio_decode` covers it, and the cases are the ones where being wrong is
silent: an instruction length that leaves the instruction pointer mid-opcode,
a 32-bit read that does not clear the high half of its destination, flags that
send a poll loop the wrong way, and -- the one that matters most -- an
unrecognised opcode reported as handled, which steps over an instruction nobody
decoded and corrupts the guest with no message at all.

### What the driver did with it

It found the controller and the device. The whole bring-up, from the trace:

```
read  +0x00 = 00000010   HcRevision -- OHCI 1.0
write +0x08 = 00000001   HcCommandStatus.HCR -- reset, self-cleared
write +0x18 = 80000000   HcHCCA
write +0x04 = 000000BE   HcControl -- HCFS operational, all lists enabled
write +0x20 = 80000520   HcControlHeadED -- a real endpoint descriptor
read  +0x54 = 00010101   port 1: CCS | PPS | CSC
write +0x54 = 00010000   clears CSC -- the root hub handshake
read  +0x58 = 00000100   port 2: powered, empty
write +0x10 = 00000040   HcInterruptEnable |= RHSC
```

Every register with behaviour was exercised and behaved: the reset bit had to
self-clear for the sequence to continue past `+0x08`, the port write had to be
a set/clear request rather than a stored value for `CSC` to go away, and the
port read had to report `CCS` for the driver to acknowledge a device at all.

Then it stops. After enabling the root hub status change interrupt it never
touches a register again, and the title carries on into the same frame loop as
before. That answers the question this was built to ask: **the driver is
interrupt-driven, and does not poll.** No amount of register modelling moves it
further on its own.

It also reads four port status registers although the root hub reports two,
which is why the register file covers `0x54` through `0x64`.

### So the next step is interrupt delivery

XPP takes its vector from `HalGetInterruptVector` and registers the handler
with `KeInitializeInterrupt` and `KeConnectInterrupt`, both of which this
runtime already bridges. `KeInitializeInterrupt` even writes the service
routine, its context and the vector into the guest `KINTERRUPT` at +0, +4 and
+8. `KeConnectInterrupt` returns TRUE and keeps none of it.

So the missing link is small and specific: record the connected interrupt
object, and give the OHCI model a way to raise one -- set the status bits,
publish a done head in the HCCA, and call the guest service routine. Calling a
recompiled function from the host side already has a pattern in this tree, in
the loader's `recomp_manual.c`. The open questions are which thread runs it and
what IRQL means here, not whether it can be reached.

### The port comes up

Interrupt delivery is in, and with it the driver takes the port all the way to
enabled. Four things had to be built, and each was found by the driver stopping
rather than by reading code.

`KeConnectInterrupt` recorded nothing. It returned TRUE and dropped the
interrupt object, so the routine `KeInitializeInterrupt` had already written
into guest memory was unreachable from this side. It keeps it by vector now,
and Half-Life 2's USB ISR is `0x00624D26` on vector 1.

`KeInsertQueueDpc` had no bridge at all and returned 0. An interrupt service
routine is supposed to do almost nothing except mask its source and queue a
DPC, so every driver following that pattern acknowledged its interrupt and then
did none of the work. The trace showed it exactly: read the enable mask, read
the status, mask MIE, queue -- and stop.

`KeSetTimer` did nothing, on a note saying timers were not needed for basic
execution. A driver polls its hardware from a timer DPC; without one the
enumeration state machine has no clock. `KeSetTimerEx` was aliased to
`KeSetTimer`, which is worse than missing: its `Period` argument sits before
the DPC pointer, so the alias read the period as the routine to call.

And the interrupt has to be delivered by the controller thread rather than from
the register write that caused it. The guest register file is thread-local, so
pointing `g_esp` at a worker stack from inside the MMIO fault handler
overwrites the stack pointer of the thread being interrupted.

What that produces, end to end:

```
raised 00000040 -> ISR claimed it        root hub status change
write +0x54 = 80000010                   SetPortReset
read  +0x10 = 80000073                   ISR: what is enabled
read  +0x0C = 00000040                   ISR: status is RHSC
write +0x14 = 80000000                   ISR: mask MIE, then queue the DPC
read  +0x54 = 00100103                   PRSC | PPS | PES | CCS
write +0x54 = 00100000                   DPC: clears the reset change
write +0x0C = 00000040                   DPC: clears RHSC
write +0x10 = 80000000                   DPC: unmask MIE
```

`00100103` is the line that matters: connected, **enabled**, reset complete. The
root hub port is up, which is as far as the hub can take it.

Two deliveries in a run, so level-triggered delivery is self-limiting -- the
handler clears the status bit and the line drops. There is a cap that reports a
handler which never clears rather than spinning the ISR forever, because an
interrupt storm is miserable to recognise from the outside.

### The transfer lists, and where it stops now

A host controller is a bus master: the driver builds endpoint and transfer
descriptors in RAM, points `HcControlHeadED` at them, and the controller walks
that list itself. None of it goes through MMIO, which is why the driver looked
idle after the port came up -- it was not idle, it was talking to memory.

`ohci.c` walks the control list now, and `usb_gamepad.c` is the device on the
other end: standard descriptors over endpoint 0 and the 20-byte report over the
interrupt endpoint, fed from the host's own pad through `xbox_input`. The pad
is not a HID device -- interface class 0x58 subclass 0x42, Microsoft's own, with
a fixed report layout -- so there is no report descriptor and nothing asks for
one.

Two bugs in that walker are worth recording, because both are the same mistake:
calling into guest territory without the checks guest code assumes.

The descriptor pointers are **untrusted input**. The driver writes
`HcControlHeadED` twice during bring-up and the second write is `0xCCCCCCCC` --
MSVC's uninitialised fill, from a local it never assigned. Following it landed
outside the mapping and took the runtime down with an access violation the
title itself would never have had. Every guest address the walker follows is
bounds-checked against `xbox_GetMappedSize()` now.

And a thread that runs recompiled code needs **its own TIB**, not just its own
stack. `fs:[0]` is the SEH chain head and `fs:[4]` reaches the CRT's per-thread
data, so a guest function with an SEH prologue on a thread whose `g_fs_base` is
zero dereferences null before it runs a line of its own body. The controller
thread and the kernel timer thread both allocate one, and both refuse to run
guest code at all rather than proceed without it.

Neither of those was visible as a fault, because `veh_handler` only reported
access violations. Everything else went past silently and the process simply
stopped, which in a log is indistinguishable from a clean exit. It names any
fatal exception now, with the code and the thread, and that is what produced
the diagnosis below in one run instead of three guesses.

**Where it stops.** The driver reaches transfer submission -- `sub_006260CC`
dispatching on endpoint type -- and divides by zero in `sub_00625BCA`:

```
and edx, 0x7ff        ; MaxPacketSize, 11 bits
cmp [ecx + 0x14], eax ; length zero? then skip
div esi               ; length / MaxPacketSize
```

It is dividing a transfer length by MaxPacketSize, and MaxPacketSize is zero.
That is the chicken and egg of enumeration: the value comes from the device
descriptor, and reading the device descriptor is the transfer being set up. A
driver resolves it with a default of 8 for endpoint 0 until the real descriptor
arrives, so the question is where this one expects that default to come from --
its own device structure, or something the root hub should have told it about
the port. Port speed is the first thing to check: `LSDA` in `HcRhPortStatus` is
clear here, so the device presents as full speed.

Reaching a divide by zero is progress, not a wall: it means the driver accepted
the port, built a device, and got as far as queueing a control transfer for it.

It is not the DPC ordering. DPCs ran inline at first -- on the queueing thread,
before the ISR returned -- which inverts what a DPC is and had the driver's
enumeration re-entering its own interrupt. That was a fair suspect for a
half-initialised device structure, so they are queued properly now and drained
by the timer thread. The crash is identical, on a different thread id, which
rules the shortcut out and leaves the queue improved anyway.

So `MaxPacketSize` is zero in XAPI's own endpoint structure, and the next
session starts by finding where that field is filled. The endpoint is reached
as `URB[+0x10]`, and the field is the word at `endpoint+2`, masked to 11 bits.
Nothing in this runtime is left unbridged on the path -- the only unbridged
ordinals in a run are `DbgPrint` and `RtlFreeAnsiString`.

Worth weighing against that: the XPP input surface is eight functions and they
are all identified above. Overriding them is the pivot this document argues
against, and the argument still holds -- it is per-title guesswork about
undocumented XDK structures -- but it is bounded work with a known end, and the
USB path is now four layers deep with a fifth in front of it. If the
`MaxPacketSize` question does not answer quickly, the eight-function override
is the cheaper way to a controller, and the OHCI model keeps its value for
every other title regardless.

Not done, and next: that MaxPacketSize, then the endpoint and transfer descriptor lists in anger, a device
answering the standard control transfers, the Xbox gamepad's descriptors and
its interrupt-IN report, and whatever interrupt delivery turns out to be
needed. The loader has the same problem and the same fix; its `loader_veh` has
not been wired, deliberately, until this is known to work in one place.

## Provenance

NoRain211's doaxbv-re has an `ohci_model.c` that solves this problem, and it is
GPL-3.0 while this toolkit is MIT. It is not to be read or adapted -- anything
written here is from the OHCI 1.0a specification and this title's own code.
