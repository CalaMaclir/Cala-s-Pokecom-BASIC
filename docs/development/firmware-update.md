# Firmware update workflow

> Version 0.85 final documentation. Hardware verification: Enter BOOTSEL,
> Reboot, and `flash-cpb.cmd` all PASS.

Cala's Pokecom BASIC can update Pico 2 W firmware without repeatedly using the
physical RESET/BOOTSEL USB insertion sequence.

## Control Center

Open:

```text
Control Center
  Firmware
    Enter BOOTSEL
    Reboot
```

### Enter BOOTSEL

`Enter BOOTSEL` stops CPB and transfers control to the RP2350 ROM USB
bootloader. The computer can then program a UF2 by the normal BOOTSEL method or
with `picotool`.

### Reboot

`Reboot` performs a normal hardware restart and boots CPB again from flash.

Both actions require confirmation. If CPB has handed the SD card to the computer
through USB Storage, return/eject USB Storage before using the Firmware menu.

## One-click Windows update with picotool

The CI artifact contains:

```text
flash-cpb.cmd
build/
  CPokecombasic.uf2
```

Requirements:

- Raspberry Pi `picotool.exe` is installed and available on `PATH`.
- PicoCalc is connected to the Pico 2 W USB port.
- USB Storage mode is not active.

Double-click:

```text
flash-cpb.cmd
```

The script uses the CPB application USB identity and runs the equivalent of:

```text
picotool load build\CPokecombasic.uf2 -v -f --vid 0xcafe --pid 0x4003
```

With a compatible CPB firmware already installed, `-f` asks the running
firmware to enter BOOTSEL, writes and verifies the UF2, then returns the board to
application mode. No physical RESET operation is required.

A UF2 can also be dragged onto `flash-cpb.cmd`, or supplied as its first
argument.

## First installation of this feature

An older CPB firmware does not contain the Firmware menu or the picotool reset
USB interface. Therefore the first upgrade to a build containing this feature
must still be installed once with the existing physical BOOTSEL/RESET procedure.

After that bootstrap installation, future firmware updates can use either:

1. `Control Center > Firmware > Enter BOOTSEL`, or
2. `flash-cpb.cmd` for the normal one-click path.

If the script cannot reach a running CPB through its application-mode reset
interface, it automatically makes a second attempt against a device that is
already in BOOTSEL mode.

## USB composite details

The Pico 2 W native USB descriptor is fixed:

- interfaces 0/1: CDC
- interface 2: MSC
- interface 3: Raspberry Pi reset vendor interface
- application VID/PID: `0xcafe` / `0x4003`
- `bcdUSB 2.10` with Microsoft OS 2.0 descriptor support

The public firmware filename is `CPokecombasic.uf2`; the public Actions
artifact is `CPokecombasic-pico2w`. The internal CMake target name is not a
user-facing download name.
