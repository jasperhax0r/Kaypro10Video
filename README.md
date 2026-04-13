# Kaypro 10 HDMI Video Snooper

A hardware and firmware project that taps the character ROM of a Kaypro 10 CP/M computer and outputs a live HDMI display via a Raspberry Pi Zero 2W running Circle bare metal. No modifications are made to the Kaypro's original circuitry.

![Kaypro 10 displaying CP/M on HDMI monitor](placeholder.jpg)

---

## How It Works

The Kaypro 10 uses a 2732 EPROM as its character ROM (U26) and a Synertek SY6545 CRTC to drive its built-in green phosphor monitor. This project passively taps the character ROM's data outputs and the CRTC's timing signals, reconstructs the pixel data in real time on a Raspberry Pi Pico, and sends it over UART to a Raspberry Pi Zero 2W which renders it to HDMI.

```
Kaypro 10
├── 6545 CRTC ──── HSYNC, CCLK, VSYNC, DE ──┐
│                                             │
└── 2732 Char ROM ── D0-D7 ──────────────────┤
                                             │
                                    [PCB Shim Board]
                                    74LVCH245 level shifters
                                    100Ω series resistors
                                             │
                                    [Raspberry Pi Pico]
                                    PIO captures ROM data
                                    synced to HSYNC/CCLK
                                             │ UART 3Mbaud
                                    [Raspberry Pi Zero 2W]
                                    Circle bare metal OS
                                    HDMI output
                                    Rotary encoder color control
```

---

## Repository Structure

### `Kaypro10Video/`
KiCad PCB files for the main capture board. This board sits between the Kaypro motherboard and the character ROM chip (U26), tapping the data lines D0-D7. It also hosts the Raspberry Pi Pico.

- Dual SN74LVCH245 level shifters (5V Kaypro → 3.3V Pico)
- 100Ω series resistors on all signal lines
- Piggyback ZIF socket for the 2732 character ROM
- JST connector for control signals from the 6545 shim

### `Kaypro10VideoShim/`
KiCad PCB files for the 6545 CRTC shim board. This small board sits between the Kaypro motherboard and the SY6545 CRTC chip, tapping HSYNC, CCLK, VSYNC, and DE without modifying the original board.

- Piggyback socket for SY6545
- JST connector output to main capture board
- Trace lengths kept under 40mm to minimize crosstalk

### `PicoKaypro/`
Raspberry Pi Pico firmware source (C, Pico SDK).

- PIO program samples character ROM data on CCLK edges
- HSYNC-locked line capture — DMA armed on each HSYNC transition
- Double-buffered frame capture (400 lines × 56 bytes)
- Temporal noise filter (AND with previous frame)
- UART output at 3Mbaud to Pi Zero 2W
- USB serial debug output with signal diagnostics

### `PiDisplay/`
Raspberry Pi Zero 2W display firmware source (C++, Circle bare metal).

- Receives pixel frames over UART
- Renders to HDMI via direct framebuffer writes
- 5 color modes: Green, Amber, White, White/Blue, Rainbow
- Rotary encoder control (turn = color mode, press = scanlines)
- Scanline overlay effect toggle
- SD card save/load of settings (`kaypro.cfg`)
- Custom palette support via `kaypro.pal`
- Status bars: connection state top, color mode bottom
- GPIO interrupt-driven encoder for instant response

### `kaypro.uf2`
Pre-compiled Pico firmware ready to flash.

---

## Hardware Requirements

| Component | Details |
|-----------|---------|
| Raspberry Pi Pico | RP2040, any revision |
| Raspberry Pi Zero 2W | With micro HDMI adapter |
| Kaypro 10 | With SY6545 CRTC and 2732 character ROM |
| Kaypro10Video PCB | See `Kaypro10Video/` |
| Kaypro10VideoShim PCB | See `Kaypro10VideoShim/` |
| Rotary encoder | 5-pin (CLK, DT, SW, GND, VCC) |
| MicroSD card | For Pi Zero 2W (FAT32, holds kernel and config) |

---

## Wiring

### Rotary Encoder → Pi Zero 2W
| Encoder | Pi Zero 2W GPIO | Pin |
|---------|----------------|-----|
| CLK | GPIO23 | Pin 16 |
| DT | GPIO24 | Pin 18 |
| SW | GPIO25 | Pin 22 |
| GND | GND | Pin 20 |
| VCC | 3.3V | Pin 17 |

### Pico → Pi Zero 2W
| Pico | Pi Zero 2W GPIO | Function |
|------|----------------|----------|
| GP12 | GPIO15 (Pin 10) | UART TX → RX |
| GND | GND (Pin 6) | Common ground |

---

## Building the Firmware

### Pico Firmware (`PicoKaypro/`)

Requirements: Pico SDK, CMake, arm-none-eabi-gcc

```bash
cd PicoKaypro
mkdir build && cd build
cmake .. \
  -DCMAKE_C_COMPILER=/usr/bin/arm-none-eabi-gcc \
  -DCMAKE_CXX_COMPILER=/usr/bin/arm-none-eabi-g++ \
  -DCMAKE_ASM_COMPILER=/usr/bin/arm-none-eabi-gcc \
  -DPICO_SDK_PATH=/path/to/pico-sdk
make -j4
# Flash kaypro_capture.uf2 to the Pico
```

Or just flash the pre-built `kaypro.uf2` from the repository root.

### Pi Zero 2W Firmware (`PiDisplay/`)

Requirements: [Circle bare metal framework](https://github.com/rsta2/circle), arm-none-eabi-gcc

```bash
# Build Circle FatFs addon first (once)
cd circle/addon/fatfs
make RASPPI=2

# Build kernel
cd circle/app/kaypro   # place PiDisplay source here
make RASPPI=2
# Output: kernel7.img
```

Copy to SD card: `kernel7.img`, `bootcode.bin`, `start.elf`, `fixup.dat`

Optionally add `kaypro.pal` for custom colors (see below).

---

## SD Card Configuration

### `kaypro.cfg` (auto-created)
Saves current color mode and scanlines state across reboots.
Format: `MODE SCANLINES` e.g. `1 0` = Amber, scanlines off.

### `kaypro.pal` (optional)
Custom color palette. Place in the root of the SD card.

```
# mode_name  fg_rrggbb  bg_rrggbb
green        00FF46     000000
amber        FFB000     000000
white        FFFFFF     000000
white_blue   FFFFFF     0000AA
rainbow      000000     000000
```

Colors are 24-bit RGB hex. The `rainbow` mode ignores `fg` and generates per-scanline hues automatically.

---

## Signal Notes

The SY6545 CRTC on the Kaypro 10 produces the following signals (measured):

| Signal | Frequency | Notes |
|--------|-----------|-------|
| CCLK | ~1.286 MHz | Character clock |
| HSYNC | ~21 kHz | HIGH=3µs blanking, LOW=43µs active |
| VSYNC | ~49 Hz | Active low |
| DE | — | Display Enable, active HIGH during active video |

The firmware captures 56 bytes per scan line (the active video window) across 400 total scan lines per frame, giving a frame size of 22,400 bytes transmitted at ~12fps over UART.

---

## Acknowledgements

- [Circle bare metal framework](https://github.com/rsta2/circle) by R. Stange
- [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk)
- Kaypro 10 technical documentation and the vintage computing community

---

## License

MIT License — see `LICENSE` file for details.
