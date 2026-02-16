#!/usr/bin/env python3
"""Convert an iTerm2 .itermcolors theme to SevenTTY .sttheme format.

Usage: python3 itermcolors2sttheme.py Theme.itermcolors > Theme.sttheme
"""
import plistlib
import sys


def rgb(d):
    """Extract 8-bit RGB hex string from an iTerm2 color dict."""
    return '%02X%02X%02X' % (
        int(d['Red Component'] * 255 + 0.5),
        int(d['Green Component'] * 255 + 0.5),
        int(d['Blue Component'] * 255 + 0.5),
    )


def main():
    if len(sys.argv) != 2:
        print('Usage: %s <file.itermcolors>' % sys.argv[0], file=sys.stderr)
        sys.exit(1)

    with open(sys.argv[1], 'rb') as f:
        p = plistlib.load(f)

    print('STTY1')
    print(rgb(p['Background Color']))
    print(rgb(p['Foreground Color']))
    print(rgb(p['Cursor Color']))
    for i in range(16):
        print(rgb(p['Ansi %d Color' % i]))


if __name__ == '__main__':
    main()
