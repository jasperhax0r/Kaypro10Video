#!/usr/bin/env python3
"""
kaypro_windows.py — Kaypro Video RAM Snooper Viewer
Live adjustment panel for frame sync tuning.

Requirements: pip install pyserial pygame
Usage: python kaypro_windows.py [--com COM14]

Keys:
  W / S   = vsync_offset +1 / -1
  E / D   = vsync_offset +10 / -10
  R / F   = line_skip +1 / -1
  V       = recalibrate VSYNC
  C       = toggle console
  X       = clear console
  B       = BOOTSEL reset
  Snap    = screenshot
  Q/Esc   = quit
"""

import sys, os, json, argparse, threading, queue, time
import serial
import pygame

# ── Log file ──────────────────────────────────────────────────────────────────
_LOG_PATH = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    f"kaypro_{time.strftime('%Y%m%d_%H%M%S')}.log"
)
_log_q = queue.Queue()

def _log_writer():
    try:
        with open(_LOG_PATH, 'a', encoding='utf-8', buffering=1) as f:
            f.write(f"=== Session started {time.strftime('%Y-%m-%d %H:%M:%S')} ===\n")
            while True:
                line = _log_q.get()
                if line is None: break
                f.write(line + '\n')
    except Exception as e:
        print(f"[WARN] Log writer: {e}")

threading.Thread(target=_log_writer, daemon=True).start()

# ── Constants ─────────────────────────────────────────────────────────────────
FRAME_MAGIC  = bytes([0xAA, 0x55, 0xAA, 0x55])
STATUS_MAGIC = bytes([0xBB, 0x66, 0xBB, 0x66])
BAUD         = 115200
DISPLAY_COLS = 80
ROWS         = 24

FG        = (0, 255, 70)
BG        = (0, 18,  0)
DIM       = (0, 160, 50)
DARK      = (0, 8,   0)
PANEL_BG  = (0, 12,  0)
BORDER    = (0, 55,  20)
CON_FG    = (180, 220, 180)
CON_BG    = (0, 5, 0)
SLIDER_BG = (0, 25,  8)
SLIDER_FG = (0, 200, 70)
SLIDER_HL = (0, 255, 120)
VAL_COL   = (0, 255, 100)

# ── Shared state ──────────────────────────────────────────────────────────────
frame_q   = queue.Queue(maxsize=4)
status_q  = queue.Queue(maxsize=10)
console_q = queue.Queue(maxsize=500)
cmd_q     = queue.Queue(maxsize=64)
connected = threading.Event()

status = {
    'frame': 0, 'de': 0, 'cclk': 0, 'hsync': 0, 'vsync': 0,
    'fps': '0.0', 'cols': 80,
    'vsync_to_row0': 0, 'line_skip': 10,
    'diag': {}
}

# Local tracking of values we've sent (for display before status echo)
local_vsync_offset = 0
local_line_skip    = 10


# ── Console / log ─────────────────────────────────────────────────────────────

def _add_console(line):
    ts  = time.strftime("%H:%M:%S")
    msg = f"[{ts}] {line}"
    try: _log_q.put_nowait(msg)
    except queue.Full: pass
    try:
        console_q.put_nowait(msg)
    except queue.Full:
        try: console_q.get_nowait()
        except: pass
        try: console_q.put_nowait(msg)
        except: pass


# ── Serial thread ─────────────────────────────────────────────────────────────

def serial_reader(com_port):
    while True:
        try:
            s = serial.Serial(com_port, BAUD, timeout=1.0)
            connected.set()
            _add_console(f"[INFO] Connected to {com_port}")
            line_buf  = bytearray()
            pkt_buf   = bytearray(4)
            in_pkt    = False
            frames_rx = 0
            last_frame_request = 0

            while True:
                # Drain command queue
                # Request a frame periodically (~6fps)
                now = time.time()
                if now - last_frame_request > 0.166:
                    last_frame_request = now
                    try: cmd_q.put_nowait('s')
                    except: pass

                while not cmd_q.empty():
                    try:
                        c = cmd_q.get_nowait()
                        s.write(c.encode('ascii'))
                    except: pass

                b = s.read(1)
                if not b: continue
                byte = b[0]
                pkt_buf = pkt_buf[1:] + bytearray([byte])

                if pkt_buf == FRAME_MAGIC:
                    line_buf.clear(); in_pkt = True
                    frames_rx += 1
                    if frames_rx == 1:
                        _add_console("[INFO] First frame received")
                    _read_frame(s)
                    continue
                elif pkt_buf == STATUS_MAGIC:
                    line_buf.clear(); in_pkt = True
                    _read_status(s)
                    continue

                if in_pkt: continue

                if byte == 0x0A:
                    line = line_buf.decode('utf-8', errors='replace').strip()
                    if line: _add_console(line)
                    line_buf.clear()
                elif byte == 0x0D:
                    pass
                elif 0x20 <= byte <= 0x7E:
                    line_buf.append(byte)
                else:
                    if line_buf:
                        line = line_buf.decode('utf-8', errors='replace').strip()
                        if line: _add_console(line)
                        line_buf.clear()

        except Exception as e:
            connected.clear()
            _add_console(f"[WARN] {e} — reconnecting...")
            time.sleep(2)


def _read_frame(s):
    hdr = s.read(4)
    if len(hdr) < 4: return
    nbytes = hdr[2] | (hdr[3] << 8)
    # Accept both old char-mode and new pixel-mode frames
    if nbytes not in (40*24, 46*24, 75*24, 80*24, 45*240, 60*240, 80*240): return
    data = s.read(nbytes)
    if len(data) < nbytes: return
    if frame_q.full():
        try: frame_q.get_nowait()
        except: pass
    try: frame_q.put_nowait(bytes(data))
    except: pass


def _read_status(s):
    ln = s.read(1)
    if not ln: return
    data = s.read(ln[0])
    if len(data) < ln[0]: return
    try: status_q.put_nowait(json.loads(data.decode()))
    except: pass


def send_cmd(c, count=1):
    for _ in range(count):
        try: cmd_q.put_nowait(c)
        except queue.Full: pass


def reset_bootsel(port):
    try:
        s = serial.Serial(port, 1200)
        time.sleep(0.5)
        s.close()
        _add_console("[INFO] BOOTSEL reset sent")
    except Exception as e:
        _add_console(f"[WARN] BOOTSEL: {e}")


# ── Slider widget ─────────────────────────────────────────────────────────────

def draw_slider(surf, font, x, y, w, label, value, vmin, vmax, active=False):
    """Draw a labelled slider bar."""
    h = 28
    pygame.draw.rect(surf, SLIDER_BG, (x, y, w, h), border_radius=4)
    if vmax > vmin:
        fill = int((value - vmin) / (vmax - vmin) * (w - 4))
        fill = max(0, min(fill, w - 4))
        col = SLIDER_HL if active else SLIDER_FG
        pygame.draw.rect(surf, col, (x+2, y+2, fill, h-4), border_radius=3)
    pygame.draw.rect(surf, BORDER, (x, y, w, h), 1, border_radius=4)
    lbl = font.render(f"{label}: {value}", True, VAL_COL if active else DIM)
    surf.blit(lbl, (x + 6, y + 7))


# ── Renderer ──────────────────────────────────────────────────────────────────

def run(com_port):
    global local_vsync_offset, local_line_skip
    pygame.init()

    CHAR_W   = 10
    CHAR_H   = 18
    VID_W    = DISPLAY_COLS * CHAR_W   # 800
    VID_H    = ROWS * CHAR_H           # 432
    ADJ_H    = 80                       # adjustment panel
    CON_H    = 120
    STATUS_H = 55
    WIN_W    = VID_W
    WIN_H    = VID_H + ADJ_H + CON_H + STATUS_H

    screen = pygame.display.set_mode((WIN_W, WIN_H), pygame.RESIZABLE)
    pygame.display.set_caption("Kaypro 10 — Video RAM Snooper")
    clk = pygame.time.Clock()

    font_main   = pygame.font.SysFont("Courier New", 16, bold=True)
    font_con    = pygame.font.SysFont("Courier New", 12)
    font_status = pygame.font.SysFont("Courier New", 11)
    font_label  = pygame.font.SysFont("Courier New", 11, bold=True)

    glyph_cache = {}
    for c in range(0x20, 0x7F):
        glyph_cache[c] = font_main.render(chr(c), True, FG, BG)
    glyph_space = glyph_cache[0x20]

    glyph_wide = {}
    for c in range(0x20, 0x7F):
        glyph_wide[c] = pygame.transform.scale(glyph_cache[c], (CHAR_W*2, CHAR_H))
    glyph_space_wide = glyph_wide[0x20]

    video_surf  = pygame.Surface((VID_W, VID_H))
    video_surf.fill(BG)
    prev_frame  = bytearray(0)
    prev_cols   = 0
    frame_dirty = True

    console_lines  = []
    console_scroll = 0
    snap_n         = 0
    show_console   = True
    last_frame     = None
    active_slider  = 0   # 0=vsync_offset, 1=line_skip

    while True:
        # ── Drain queues ──────────────────────────────────────────────────────
        while not status_q.empty():
            try:
                st = status_q.get_nowait()
                status.update(st)
                # Sync local values from Pico's echo
                local_vsync_offset = status.get('vsync_to_row0', local_vsync_offset)
                local_line_skip    = status.get('line_skip', local_line_skip)
            except: break

        while not console_q.empty():
            try:
                line = console_q.get_nowait()
                console_lines.append(line)
                if len(console_lines) > 500:
                    console_lines = console_lines[-500:]
            except: break

        new_frame = None
        while not frame_q.empty():
            try: new_frame = frame_q.get_nowait()
            except: break
        if new_frame is not None:
            last_frame = new_frame
            frame_dirty = True

        # ── Events ────────────────────────────────────────────────────────────
        for ev in pygame.event.get():
            if ev.type == pygame.QUIT:
                pygame.quit(); return
            if ev.type == pygame.KEYDOWN:
                # vsync_offset adjustment
                if ev.key == pygame.K_w:
                    local_vsync_offset += 1
                    send_cmd('f')
                    frame_dirty = True
                    _add_console(f"vsync_offset → {local_vsync_offset}")
                elif ev.key == pygame.K_s:
                    local_vsync_offset = max(0, local_vsync_offset - 1)
                    send_cmd('F')
                    frame_dirty = True
                    _add_console(f"vsync_offset → {local_vsync_offset}")
                elif ev.key == pygame.K_e:
                    for _ in range(10):
                        local_vsync_offset += 1
                        send_cmd('f')
                    frame_dirty = True
                    _add_console(f"vsync_offset → {local_vsync_offset}")
                elif ev.key == pygame.K_d:
                    for _ in range(10):
                        local_vsync_offset = max(0, local_vsync_offset - 1)
                        send_cmd('F')
                    frame_dirty = True
                    _add_console(f"vsync_offset → {local_vsync_offset}")
                # line_skip adjustment
                elif ev.key == pygame.K_r:
                    local_line_skip += 1
                    send_cmd('l')
                    frame_dirty = True
                    _add_console(f"line_skip → {local_line_skip}")
                elif ev.key == pygame.K_f:
                    local_line_skip = max(1, local_line_skip - 1)
                    send_cmd('L')
                    frame_dirty = True
                    _add_console(f"line_skip → {local_line_skip}")
                # Other commands
                elif ev.key == pygame.K_v:
                    send_cmd('v')
                    _add_console("[INFO] Recalibrating VSYNC...")
                elif ev.key == pygame.K_TAB:
                    active_slider = 1 - active_slider
                elif ev.key in (pygame.K_q, pygame.K_ESCAPE):
                    pygame.quit(); return
                elif ev.key == pygame.K_SPACE:
                    os.makedirs("frames", exist_ok=True)
                    fname = f"frames/snap_{snap_n:05d}.png"
                    pygame.image.save(screen, fname)
                    snap_n += 1
                    _add_console(f"Snapshot: {fname}")
                elif ev.key == pygame.K_c:
                    show_console = not show_console
                    _add_console("Console " + ("shown" if show_console else "hidden"))
                elif ev.key == pygame.K_x:
                    console_lines.clear(); console_scroll = 0
                elif ev.key == pygame.K_UP:
                    console_scroll = min(console_scroll+1, max(0, len(console_lines)-3))
                elif ev.key == pygame.K_DOWN:
                    console_scroll = max(0, console_scroll-1)
                elif ev.key == pygame.K_PAGEUP:
                    console_scroll = min(console_scroll+10, max(0, len(console_lines)-3))
                elif ev.key == pygame.K_PAGEDOWN:
                    console_scroll = max(0, console_scroll-10)
                elif ev.key == pygame.K_b:
                    threading.Thread(target=reset_bootsel,
                                     args=(com_port,), daemon=True).start()

        # ── Draw video ────────────────────────────────────────────────────────
        screen.fill(DARK)

        if last_frame:
            nbytes = len(last_frame)
            is_pixel_mode = (nbytes == 80 * 240 or nbytes == 45 * 240)

            if is_pixel_mode:
                # Pixel mode: N bytes/line × 240 lines, each byte = 8 pixels
                bytes_per_line = nbytes // 240
                PIXEL_W = bytes_per_line * 8
                PIXEL_H = 240
                scale_x = VID_W / PIXEL_W
                scale_y = VID_H / PIXEL_H
                px_w = max(1, int(scale_x))
                px_h = max(1, int(scale_y))

                if frame_dirty:
                    video_surf.fill(BG)
                    for line in range(PIXEL_H):
                        for byte_idx in range(bytes_per_line):
                            byte = last_frame[line * bytes_per_line + byte_idx]
                            for bit in range(8):
                                pixel_on = (byte >> (7 - bit)) & 1
                                if pixel_on:
                                    px = (byte_idx * 8 + bit)
                                    x = int(px * scale_x)
                                    y = int(line * scale_y)
                                    pygame.draw.rect(video_surf, FG,
                                                     (x, y, px_w, px_h))
                    frame_dirty = False
                screen.blit(video_surf, (0, 0))
            else:
                # Character mode (legacy)
                frame_cols = len(last_frame) // ROWS
                wide       = (frame_cols <= 40)
                cw         = CHAR_W*2 if wide else CHAR_W
                cache      = glyph_wide if wide else glyph_cache
                space_g    = glyph_space_wide if wide else glyph_space
                fsz        = frame_cols * ROWS
                if frame_cols != prev_cols or len(prev_frame) != fsz:
                    prev_frame = bytearray(fsz)
                    frame_dirty = True
                    prev_cols = frame_cols
                    video_surf.fill(BG)

                for idx in range(fsz):
                    row   = idx // frame_cols
                    col   = idx % frame_cols
                    src_col = (col + frame_cols - 1) % frame_cols
                    src_idx = row * frame_cols + src_col
                    raw = last_frame[src_idx] & 0x7F
                    if idx < len(prev_frame) and raw == prev_frame[idx] and not frame_dirty:
                        continue
                    ch    = raw if 0x20 <= raw <= 0x7E else 0x20
                    glyph = cache.get(ch, space_g)
                    video_surf.blit(glyph, (col * cw, row * CHAR_H))

                rotated = bytearray(fsz)
                for i in range(fsz):
                    r = i // frame_cols
                    c = i % frame_cols
                    src = r * frame_cols + (c + 79) % frame_cols
                    rotated[i] = last_frame[src] & 0x7F
                prev_frame  = rotated
                frame_dirty = False
                screen.blit(video_surf, (0, 0))
        else:
            msg = ("Waiting for frame..." if connected.is_set()
                   else "NOT CONNECTED — check COM port")
            t = font_main.render(msg, True, DIM)
            screen.blit(t, (VID_W//2 - t.get_width()//2,
                             VID_H//2 - t.get_height()//2))

        pygame.draw.line(screen, BORDER, (0, VID_H), (WIN_W, VID_H), 1)

        # ── Adjustment panel ──────────────────────────────────────────────────
        ay = VID_H + 1
        pygame.draw.rect(screen, PANEL_BG, (0, ay, WIN_W, ADJ_H))

        title = font_label.render(
            "── Frame Sync Adjustment ──  TAB=switch  W/S=vsync±1  E/D=vsync±10  R/F=skip±1",
            True, BORDER)
        screen.blit(title, (6, ay + 4))

        sw = (WIN_W - 30) // 2
        draw_slider(screen, font_label, 8,      ay+22, sw,
                    "vsync_offset", local_vsync_offset, 0, 200,
                    active=(active_slider==0))
        draw_slider(screen, font_label, sw+16,  ay+22, sw,
                    "line_skip",    local_line_skip,    1, 20,
                    active=(active_slider==1))

        # Show what Pico confirmed
        pico_v2r  = status.get('vsync_to_row0', '?')
        pico_skip = status.get('line_skip', '?')
        diag      = status.get('diag', {})
        col0r0    = diag.get('col0_r0', '?')
        de_frame  = diag.get('de_frame', '?')
        confirmed = font_status.render(
            f"Pico confirmed: vsync_offset={pico_v2r}  line_skip={pico_skip}  "
            f"col0_r0={col0r0}  de_frame={de_frame}  (col0=0x4B='K' when aligned)",
            True, (0, 140, 60))
        screen.blit(confirmed, (6, ay + 56))

        # ── Console ───────────────────────────────────────────────────────────
        if show_console:
            cy = VID_H + ADJ_H
            pygame.draw.rect(screen, CON_BG, (0, cy, WIN_W, CON_H))
            hdr = font_status.render("── Console (C=toggle X=clear) ──", True, BORDER)
            screen.blit(hdr, (4, cy + 3))
            scroll_hint = f" [{console_scroll} from bottom]" if console_scroll > 0 else " [latest]"
            hdr2 = font_status.render(f"↑↓/PgUp/PgDn{scroll_hint}", True, (0,60,20))
            screen.blit(hdr2, (WIN_W - hdr2.get_width() - 4, cy + 3))

            line_h    = font_con.get_height() + 1
            max_lines = (CON_H - 18) // line_h
            total     = len(console_lines)
            end_idx   = max(0, total - console_scroll)
            start_idx = max(0, end_idx - max_lines)
            for i, line in enumerate(console_lines[start_idx:end_idx]):
                if   "[OK]"   in line: col = (0, 200, 80)
                elif "[WARN]" in line or "[FAIL]" in line: col = (220, 160, 0)
                elif "[INFO]" in line: col = (100, 180, 220)
                else:                  col = CON_FG
                t = font_con.render(line[:120], True, col)
                screen.blit(t, (4, cy + 16 + i * line_h))

        # ── Status bar ────────────────────────────────────────────────────────
        sy = VID_H + ADJ_H + CON_H
        pygame.draw.rect(screen, PANEL_BG, (0, sy, WIN_W, STATUS_H))
        pygame.draw.line(screen, BORDER, (0, sy), (WIN_W, sy), 1)

        vfps   = clk.get_fps()
        fw_fps = status.get('fps', '0.0')
        conn_c = (0, 200, 60) if connected.is_set() else (200, 60, 0)
        conn_t = "● CONNECTED" if connected.is_set() else "● DISCONNECTED"

        def sr(dy, text, col=DIM):
            screen.blit(font_status.render(text, True, col), (6, sy+dy))

        sr(4,  f"{conn_t}   frame #{status['frame']}   "
               f"{vfps:.1f}fps   fw:{fw_fps}fps", conn_c)
        sr(18, f"DE={status['de']}/s  CCLK={status.get('cclk',0):,}/s  "
               f"HSYNC={status.get('hsync',0)}/s  VSYNC={status.get('vsync',0)}/s")
        sr(32, f"W/S=vsync±1  E/D=vsync±10  R/F=skip±1  "
               f"V=recal  Space=snap  C=console  B=bootsel  Q=quit",
           (0, 100, 35))
        sr(46, f"log:{os.path.basename(_LOG_PATH)}", (0, 70, 25))

        pygame.display.flip()
        clk.tick(65)


# ── Entry point ───────────────────────────────────────────────────────────────

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--com', default="COM14")
    a = p.parse_args()
    com = a.com.upper()
    if com.isdigit(): com = 'COM' + com

    print(f"[INFO] Connecting to {com}")
    print(f"[INFO] Logging to {_LOG_PATH}")
    _add_console(f"[INFO] Log: {_LOG_PATH}")

    threading.Thread(target=serial_reader, args=(com,), daemon=True).start()
    try:
        run(com)
    except KeyboardInterrupt:
        print("\n[INFO] Stopped.")
    finally:
        _log_q.put(f"=== Session ended {time.strftime('%Y-%m-%d %H:%M:%S')} ===")


if __name__ == '__main__':
    main()
