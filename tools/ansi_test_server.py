#!/usr/bin/env python3
"""Interactive ANSI art gallery over TCP.
Usage: python3 ansi_test_server.py [port]
Serves .ans files from tools/testart/, converting CP437 to UTF-8
(like a real BBS does). Arrow keys to navigate, r to reload, q to quit.
"""
import socket, sys, time, os, glob, select

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 1337

# CP437 -> Unicode mapping for bytes 0x00-0xFF
# 0x00-0x1F: CP437 control chars have special glyphs (not ASCII control)
# 0x20-0x7E: same as ASCII
# 0x7F: house
# 0x80-0xFF: extended characters
CP437_TO_UNICODE = [
    # 0x00-0x1F: CP437 special glyphs (but we skip most control chars)
    0x0000, 0x263A, 0x263B, 0x2665, 0x2666, 0x2663, 0x2660, 0x2022,
    0x25D8, 0x25CB, 0x25D9, 0x2642, 0x2640, 0x266A, 0x266B, 0x263C,
    0x25BA, 0x25C4, 0x2195, 0x203C, 0x00B6, 0x00A7, 0x25AC, 0x21A8,
    0x2191, 0x2193, 0x2192, 0x2190, 0x221F, 0x2194, 0x25B2, 0x25BC,
    # 0x20-0x7E: ASCII (identity mapped)
] + list(range(0x20, 0x7F)) + [
    # 0x7F
    0x2302,
    # 0x80-0xFF: extended CP437
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
    0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
    0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
]

def strip_sauce(data):
    """Strip SAUCE record from end of .ans file."""
    # SAUCE record is 128 bytes at end, starts with "SAUCE"
    if len(data) >= 128 and data[-128:-123] == b'SAUCE':
        data = data[:-128]
    # Also strip any COMNT block before SAUCE
    while len(data) >= 5 and data[-5:] == b'COMNT':
        data = data[:-5]
    # Strip trailing EOF marker (Ctrl-Z)
    while data and data[-1] == 0x1A:
        data = data[:-1]
    return data

def ansi_sys_to_vt(data):
    """Translate ANSI.SYS sequences to VT equivalents.
    BBS art uses ESC[s/ESC[u for save/restore cursor (ANSI.SYS),
    but vterm treats ESC[s as DECSLRM (set left/right margins).
    Convert to ESC 7 / ESC 8 (DECSC/DECRC) which vterm supports."""
    import re
    data = re.sub(rb'\x1b\[s', b'\x1b7', data)
    data = re.sub(rb'\x1b\[u', b'\x1b8', data)
    return data

def cp437_to_utf8(data):
    """Convert CP437 bytes to UTF-8, preserving ANSI escape sequences."""
    data = strip_sauce(data)
    out = bytearray()
    i = 0
    while i < len(data):
        b = data[i]
        # Stop at EOF marker
        if b == 0x1A:
            break
        # Pass through ESC sequences verbatim (they're ASCII)
        if b == 0x1B:
            out.append(b)
            i += 1
            continue
        # Pass through normal ASCII printable + standard controls
        if b == 0x0A or b == 0x0D or b == 0x09:
            out.append(b)
            i += 1
            continue
        # Skip other control chars that aren't CP437 glyphs
        if b < 0x20 and b not in (0x1B, 0x0A, 0x0D, 0x09):
            # Most ANSI art doesn't use CP437 control char glyphs
            i += 1
            continue
        elif b <= 0x7E:
            out.append(b)
            i += 1
            continue
        else:
            cp = CP437_TO_UNICODE[b]

        # Encode Unicode codepoint as UTF-8
        if cp < 0x80:
            out.append(cp)
        elif cp < 0x800:
            out.append(0xC0 | (cp >> 6))
            out.append(0x80 | (cp & 0x3F))
        else:
            out.append(0xE0 | (cp >> 12))
            out.append(0x80 | ((cp >> 6) & 0x3F))
            out.append(0x80 | (cp & 0x3F))
        i += 1
    return bytes(out)

script_dir = os.path.dirname(os.path.abspath(__file__))
art_dir = os.path.join(script_dir, "testart")
files = sorted(glob.glob(os.path.join(art_dir, "*.ans")) +
               glob.glob(os.path.join(art_dir, "*.ANS")))

if not files:
    print(f"No .ans files in {art_dir}")
    sys.exit(1)

# Pre-convert all files
artdata = {}
for f in files:
    with open(f, "rb") as fh:
        raw = fh.read()
    artdata[f] = cp437_to_utf8(raw)
    nlines = artdata[f].count(b'\n')
    print(f"  {os.path.basename(f):30s} {len(raw):6d} -> {len(artdata[f]):6d} bytes ({nlines} lines)")

print(f"\nLoaded {len(files)} art files")

CLR = b'\x1b[2J\x1b[H'

def send_art(conn, idx):
    f = files[idx]
    fname = os.path.basename(f)
    conn.sendall(CLR)
    time.sleep(0.3)
    data = artdata[f]
    chunk_size = 256
    for i in range(0, len(data), chunk_size):
        conn.sendall(data[i:i+chunk_size])
        time.sleep(0.01)
    # Position status bar at bottom of screen (row 25) regardless of art height
    status = f"\x1b[25;1H\x1b[0m\x1b[7m [{idx+1}/{len(files)}] {fname} | \u2190\u2192: navigate | r: reload | q: quit \x1b[0m"
    conn.sendall(status.encode('utf-8'))

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("0.0.0.0", PORT))
srv.listen(1)
print(f"Listening on port {PORT}")

while True:
    conn, addr = srv.accept()
    print(f"Connection from {addr}")
    conn.setblocking(False)
    idx = 0
    try:
        conn.sendall(b'\xff\xfb\x01\xff\xfb\x03')
        time.sleep(0.1)
        try:
            conn.recv(256)
        except BlockingIOError:
            pass
        send_art(conn, idx)
        while True:
            ready, _, _ = select.select([conn], [], [], 0.1)
            if ready:
                try:
                    data = conn.recv(64)
                except (BlockingIOError, ConnectionResetError):
                    break
                if not data:
                    break
                i = 0
                while i < len(data):
                    if data[i] == 0xFF and i + 2 < len(data):
                        i += 3
                        continue
                    if data[i] == 0x1B and i + 2 < len(data) and data[i+1] == 0x5B:
                        arrow = data[i+2]
                        if arrow in (0x43, 0x42):  # right/down
                            idx = (idx + 1) % len(files)
                            send_art(conn, idx)
                        elif arrow in (0x44, 0x41):  # left/up
                            idx = (idx - 1) % len(files)
                            send_art(conn, idx)
                        i += 3
                        continue
                    ch = data[i]
                    if ch in (ord('q'), ord('Q'), 0x03):
                        raise ConnectionResetError
                    elif ch in (ord('r'), ord('R')):
                        send_art(conn, idx)
                    elif ch in (ord('n'), ord('N'), ord(' ')):
                        idx = (idx + 1) % len(files)
                        send_art(conn, idx)
                    elif ch in (ord('p'), ord('P')):
                        idx = (idx - 1) % len(files)
                        send_art(conn, idx)
                    i += 1
    except (BrokenPipeError, ConnectionResetError, OSError):
        pass
    conn.close()
    print(f"Disconnected")
