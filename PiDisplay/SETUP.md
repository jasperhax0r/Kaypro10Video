# Kaypro Display — Pi Zero Setup (Circle bare metal)

## What you need
- Raspberry Pi Zero or Zero W (not Zero 2W — that needs RASPPI=2)
- Micro SD card (any size, even 512MB works)
- HDMI cable + monitor
- 2 jumper wires

## Wiring
```
Capture Pico GP12 (UART TX) ──→ Pi Zero GPIO15 / Pin 10 (UART0 RX)
Capture Pico GND             ──→ Pi Zero GND / Pin 6
```
Note: 3.3V logic on both sides — direct connection, no level shifter needed.

## Build setup (on Linux)

### 1. Install ARM toolchain
```bash
sudo apt install gcc-arm-none-eabi
```

### 2. Clone Circle
```bash
cd ~
git clone https://github.com/rsta2/circle
export CIRCLE_HOME=~/circle
```

### 3. Build Circle libraries (for Pi Zero 2W)
```bash
cd ~/circle
./makeall --nosample RASPPI=2
```
This takes a few minutes. Only needs to be done once.

### 4. Get Pi Zero firmware files
```bash
cd ~/circle/boot
make
```
This downloads bootcode.bin, start.elf, fixup.dat from the Pi firmware repo.

### 5. Build the kernel
```bash
cd ~/Kaypro   # wherever you put kernel.cpp and Makefile
make
```

## SD card contents
Copy these 4 files to a FAT32 formatted SD card:
```
kernel7.img         ← built by make (Pi Zero 2W uses kernel7.img)
bootcode.bin        ← from ~/circle/boot/
start.elf           ← from ~/circle/boot/
fixup.dat           ← from ~/circle/boot/
```
No config.txt needed for Pi Zero 2W.

## Boot time
~3-4 seconds from power-on to first HDMI frame displayed.

## LED behaviour
- Solid ON = initialised, waiting for frames
- Slow blink = receiving frames normally (blinks every 60 frames)
- Rapid blink = initialisation failed (HDMI or UART problem)

## Updating
To update the display code, just rebuild kernel.img and copy it to the SD card.
The firmware files (bootcode.bin, start.elf, fixup.dat) never need to change.

## Notes
- Pi Zero 2W: uses kernel7.img, RASPPI=2, Cortex-A53 — all handled by Makefile.
- Pi Zero W original: change RASPPI=1 in Makefile, output will be kernel.img.
- The capture Pico sends frames at 4Mbaud on GP12.
- vsync_to_row0 can still be adjusted on the capture Pico with f/F keys.
- Boot time on Zero 2W: ~3 seconds to first frame.
