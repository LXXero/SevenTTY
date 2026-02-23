#!/usr/bin/env python3
"""
Generate unicode.h for SevenTTY.

Builds UNICODE_BMP_NORMALIZER table: maps Unicode codepoints 0-65535 to
Mac Roman byte values. Characters with exact Mac Roman equivalents get their
proper encoding. Characters without equivalents get ASCII approximations
where sensible, or the lozenge fallback (0xD7).
"""

# Complete Mac Roman encoding table: Mac Roman byte -> Unicode codepoint
# 0x00-0x7F are standard ASCII (identity mapping)
# 0x80-0xFF are the Mac Roman extended characters
MAC_ROMAN_TO_UNICODE = {
    0x80: 0x00C4,  # Ä
    0x81: 0x00C5,  # Å
    0x82: 0x00C7,  # Ç
    0x83: 0x00C9,  # É
    0x84: 0x00D1,  # Ñ
    0x85: 0x00D6,  # Ö
    0x86: 0x00DC,  # Ü
    0x87: 0x00E1,  # á
    0x88: 0x00E0,  # à
    0x89: 0x00E2,  # â
    0x8A: 0x00E4,  # ä
    0x8B: 0x00E3,  # ã
    0x8C: 0x00E5,  # å
    0x8D: 0x00E7,  # ç
    0x8E: 0x00E9,  # é
    0x8F: 0x00E8,  # è
    0x90: 0x00EA,  # ê
    0x91: 0x00EB,  # ë
    0x92: 0x00ED,  # í
    0x93: 0x00EC,  # ì
    0x94: 0x00EE,  # î
    0x95: 0x00EF,  # ï
    0x96: 0x00F1,  # ñ
    0x97: 0x00F3,  # ó
    0x98: 0x00F2,  # ò
    0x99: 0x00F4,  # ô
    0x9A: 0x00F6,  # ö
    0x9B: 0x00F5,  # õ
    0x9C: 0x00FA,  # ú
    0x9D: 0x00F9,  # ù
    0x9E: 0x00FB,  # û
    0x9F: 0x00FC,  # ü
    0xA0: 0x2020,  # †
    0xA1: 0x00B0,  # °
    0xA2: 0x00A2,  # ¢
    0xA3: 0x00A3,  # £
    0xA4: 0x00A7,  # §
    0xA5: 0x2022,  # •
    0xA6: 0x00B6,  # ¶
    0xA7: 0x00DF,  # ß
    0xA8: 0x00AE,  # ®
    0xA9: 0x00A9,  # ©
    0xAA: 0x2122,  # ™
    0xAB: 0x00B4,  # ´
    0xAC: 0x00A8,  # ¨
    0xAD: 0x2260,  # ≠
    0xAE: 0x00C6,  # Æ
    0xAF: 0x00D8,  # Ø
    0xB0: 0x221E,  # ∞
    0xB1: 0x00B1,  # ±
    0xB2: 0x2264,  # ≤
    0xB3: 0x2265,  # ≥
    0xB4: 0x00A5,  # ¥
    0xB5: 0x00B5,  # µ
    0xB6: 0x2202,  # ∂
    0xB7: 0x2211,  # ∑
    0xB8: 0x220F,  # ∏
    0xB9: 0x03C0,  # π
    0xBA: 0x222B,  # ∫
    0xBB: 0x00AA,  # ª
    0xBC: 0x00BA,  # º
    0xBD: 0x2126,  # Ω
    0xBE: 0x00E6,  # æ
    0xBF: 0x00F8,  # ø
    0xC0: 0x00BF,  # ¿
    0xC1: 0x00A1,  # ¡
    0xC2: 0x00AC,  # ¬
    0xC3: 0x221A,  # √
    0xC4: 0x0192,  # ƒ
    0xC5: 0x2248,  # ≈
    0xC6: 0x2206,  # ∆
    0xC7: 0x00AB,  # «
    0xC8: 0x00BB,  # »
    0xC9: 0x2026,  # …
    0xCA: 0x00A0,  # non-breaking space
    0xCB: 0x00C0,  # À
    0xCC: 0x00C3,  # Ã
    0xCD: 0x00D5,  # Õ
    0xCE: 0x0152,  # Œ
    0xCF: 0x0153,  # œ
    0xD0: 0x2013,  # –
    0xD1: 0x2014,  # —
    0xD2: 0x201C,  # "
    0xD3: 0x201D,  # "
    0xD4: 0x2018,  # '
    0xD5: 0x2019,  # '
    0xD6: 0x00F7,  # ÷
    0xD7: 0x25CA,  # ◊ (lozenge)
    0xD8: 0x00FF,  # ÿ
    0xD9: 0x0178,  # Ÿ
    0xDA: 0x2044,  # ⁄
    0xDB: 0x20AC,  # €
    0xDC: 0x2039,  # ‹
    0xDD: 0x203A,  # ›
    0xDE: 0xFB01,  # fi
    0xDF: 0xFB02,  # fl
    0xE0: 0x2021,  # ‡
    0xE1: 0x00B7,  # ·
    0xE2: 0x201A,  # ‚
    0xE3: 0x201E,  # „
    0xE4: 0x2030,  # ‰
    0xE5: 0x00C2,  # Â
    0xE6: 0x00CA,  # Ê
    0xE7: 0x00C1,  # Á
    0xE8: 0x00CB,  # Ë
    0xE9: 0x00C8,  # È
    0xEA: 0x00CD,  # Í
    0xEB: 0x00CE,  # Î
    0xEC: 0x00CF,  # Ï
    0xED: 0x00CC,  # Ì
    0xEE: 0x00D3,  # Ó
    0xEF: 0x00D4,  # Ô
    0xF0: 0xF8FF,  # Apple logo
    0xF1: 0x00D2,  # Ò
    0xF2: 0x00DA,  # Ú
    0xF3: 0x00DB,  # Û
    0xF4: 0x00D9,  # Ù
    0xF5: 0x0131,  # ı
    0xF6: 0x02C6,  # ˆ
    0xF7: 0x02DC,  # ˜
    0xF8: 0x00AF,  # ¯
    0xF9: 0x02D8,  # ˘
    0xFA: 0x02D9,  # ˙
    0xFB: 0x02DA,  # ˚
    0xFC: 0x00B8,  # ¸
    0xFD: 0x02DD,  # ˝
    0xFE: 0x02DB,  # ˛
    0xFF: 0x02C7,  # ˇ
}

# Build reverse map: Unicode codepoint -> Mac Roman byte
UNICODE_TO_MAC_ROMAN = {}
for mr_byte, ucp in MAC_ROMAN_TO_UNICODE.items():
    UNICODE_TO_MAC_ROMAN[ucp] = mr_byte

# Additional Unicode -> ASCII approximations for chars without Mac Roman equivalents
# These are best-effort fallbacks (the original table already had many of these)
ASCII_APPROX = {
    # Superscripts/subscripts
    0x00B2: ord('2'),  # ²
    0x00B3: ord('3'),  # ³
    0x00B9: ord('1'),  # ¹
    0x2070: ord('0'),  # ⁰
    0x2074: ord('4'),  # ⁴
    0x2075: ord('5'),  # ⁵
    0x2076: ord('6'),  # ⁶
    0x2077: ord('7'),  # ⁷
    0x2078: ord('8'),  # ⁸
    0x2079: ord('9'),  # ⁹

    # Latin-like from other blocks (approximate to base letter)
    0x0100: ord('A'), 0x0101: ord('a'),  # Ā ā
    0x0102: ord('A'), 0x0103: ord('a'),  # Ă ă
    0x0104: ord('A'), 0x0105: ord('a'),  # Ą ą
    0x0106: ord('C'), 0x0107: ord('c'),  # Ć ć
    0x0108: ord('C'), 0x0109: ord('c'),  # Ĉ ĉ
    0x010A: ord('C'), 0x010B: ord('c'),  # Ċ ċ
    0x010C: ord('C'), 0x010D: ord('c'),  # Č č
    0x010E: ord('D'), 0x010F: ord('d'),  # Ď ď
    0x0110: ord('D'), 0x0111: ord('d'),  # Đ đ
    0x0112: ord('E'), 0x0113: ord('e'),  # Ē ē
    0x0116: ord('E'), 0x0117: ord('e'),  # Ė ė
    0x0118: ord('E'), 0x0119: ord('e'),  # Ę ę
    0x011A: ord('E'), 0x011B: ord('e'),  # Ě ě
    0x011C: ord('G'), 0x011D: ord('g'),  # Ĝ ĝ
    0x011E: ord('G'), 0x011F: ord('g'),  # Ğ ğ
    0x0120: ord('G'), 0x0121: ord('g'),  # Ġ ġ
    0x0122: ord('G'), 0x0123: ord('g'),  # Ģ ģ
    0x0124: ord('H'), 0x0125: ord('h'),  # Ĥ ĥ
    0x0126: ord('H'), 0x0127: ord('h'),  # Ħ ħ
    0x0128: ord('I'), 0x0129: ord('i'),  # Ĩ ĩ
    0x012A: ord('I'), 0x012B: ord('i'),  # Ī ī
    0x012E: ord('I'), 0x012F: ord('i'),  # Į į
    0x0130: ord('I'),                     # İ
    0x0134: ord('J'), 0x0135: ord('j'),  # Ĵ ĵ
    0x0136: ord('K'), 0x0137: ord('k'),  # Ķ ķ
    0x0139: ord('L'), 0x013A: ord('l'),  # Ĺ ĺ
    0x013B: ord('L'), 0x013C: ord('l'),  # Ļ ļ
    0x013D: ord('L'), 0x013E: ord('l'),  # Ľ ľ
    0x0141: ord('L'), 0x0142: ord('l'),  # Ł ł
    0x0143: ord('N'), 0x0144: ord('n'),  # Ń ń
    0x0145: ord('N'), 0x0146: ord('n'),  # Ņ ņ
    0x0147: ord('N'), 0x0148: ord('n'),  # Ň ň
    0x0150: ord('O'), 0x0151: ord('o'),  # Ő ő
    0x0154: ord('R'), 0x0155: ord('r'),  # Ŕ ŕ
    0x0156: ord('R'), 0x0157: ord('r'),  # Ŗ ŗ
    0x0158: ord('R'), 0x0159: ord('r'),  # Ř ř
    0x015A: ord('S'), 0x015B: ord('s'),  # Ś ś
    0x015C: ord('S'), 0x015D: ord('s'),  # Ŝ ŝ
    0x015E: ord('S'), 0x015F: ord('s'),  # Ş ş
    0x0160: ord('S'), 0x0161: ord('s'),  # Š š
    0x0162: ord('T'), 0x0163: ord('t'),  # Ţ ţ
    0x0164: ord('T'), 0x0165: ord('t'),  # Ť ť
    0x0168: ord('U'), 0x0169: ord('u'),  # Ũ ũ
    0x016A: ord('U'), 0x016B: ord('u'),  # Ū ū
    0x016C: ord('U'), 0x016D: ord('u'),  # Ŭ ŭ
    0x016E: ord('U'), 0x016F: ord('u'),  # Ů ů
    0x0170: ord('U'), 0x0171: ord('u'),  # Ű ű
    0x0172: ord('U'), 0x0173: ord('u'),  # Ų ų
    0x0174: ord('W'), 0x0175: ord('w'),  # Ŵ ŵ
    0x0176: ord('Y'), 0x0177: ord('y'),  # Ŷ ŷ
    0x0179: ord('Z'), 0x017A: ord('z'),  # Ź ź
    0x017B: ord('Z'), 0x017C: ord('z'),  # Ż ż
    0x017D: ord('Z'), 0x017E: ord('z'),  # Ž ž

    # Common symbols
    0x2010: ord('-'),  # ‐ hyphen
    0x2011: ord('-'),  # ‑ non-breaking hyphen
    0x2012: ord('-'),  # ‒ figure dash
    0x2015: 0xD1,      # ― horizontal bar -> em dash
    0x2024: 0xE1,      # ․ one dot leader -> Mac Roman middle dot (·)
    0x2027: 0xE1,      # ‧ hyphenation point -> Mac Roman middle dot (·)
    0x2032: ord("'"),  # ′ prime
    0x2033: ord('"'),  # ″ double prime
    0x2016: ord('|'),  # ‖ double vertical line
    0x2043: ord('-'),  # ⁃ hyphen bullet
    0x2212: ord('-'),  # − minus sign
    0x2219: 0xE1,      # ∙ bullet operator -> Mac Roman middle dot (·)
    0x2261: ord('='),  # ≡ identical to

    # Subscript digits
    0x2080: ord('0'),  # ₀
    0x2081: ord('1'),  # ₁
    0x2082: ord('2'),  # ₂
    0x2083: ord('3'),  # ₃
    0x2084: ord('4'),  # ₄
    0x2085: ord('5'),  # ₅
    0x2086: ord('6'),  # ₆
    0x2087: ord('7'),  # ₇
    0x2088: ord('8'),  # ₈
    0x2089: ord('9'),  # ₉

    # Arrows (not in symbol font, approximate)
    0x21D0: ord('<'),  # ⇐
    0x21D2: ord('>'),  # ⇒
    0x21D4: ord('='),  # ⇔

    # Greek (approximate to Latin where possible)
    0x0391: ord('A'), 0x0392: ord('B'), 0x0393: ord('G'),
    0x0394: 0xC6,     # Δ -> ∆ (Mac Roman)
    0x0395: ord('E'), 0x0396: ord('Z'), 0x0397: ord('H'),
    0x0398: ord('O'), 0x0399: ord('I'), 0x039A: ord('K'),
    0x039B: ord('L'), 0x039C: ord('M'), 0x039D: ord('N'),
    0x039E: ord('X'), 0x039F: ord('O'), 0x03A0: ord('P'),
    0x03A1: ord('R'), 0x03A3: ord('S'), 0x03A4: ord('T'),
    0x03A5: ord('Y'), 0x03A6: ord('F'), 0x03A7: ord('X'),
    0x03A8: ord('Y'), 0x03A9: 0xBD,     # Ω (Mac Roman)
    0x03B1: ord('a'), 0x03B2: ord('b'), 0x03B3: ord('g'),
    0x03B4: ord('d'), 0x03B5: ord('e'), 0x03B6: ord('z'),
    0x03B7: ord('h'), 0x03B8: ord('o'), 0x03B9: ord('i'),
    0x03BA: ord('k'), 0x03BB: ord('l'), 0x03BC: 0xB5,  # µ
    0x03BD: ord('n'), 0x03BE: ord('x'), 0x03BF: ord('o'),
    # 0x03C0 = π -> 0xB9 (already in UNICODE_TO_MAC_ROMAN)
    0x03C1: ord('r'), 0x03C2: ord('s'), 0x03C3: ord('s'),
    0x03C4: ord('t'), 0x03C5: ord('u'), 0x03C6: ord('f'),
    0x03C7: ord('x'), 0x03C8: ord('y'), 0x03C9: ord('w'),

    # Fullwidth ASCII (map back to regular ASCII)
    # FF01-FF5E -> 0x21-0x7E
}

# Add fullwidth ASCII mappings
for cp in range(0xFF01, 0xFF5F):
    ASCII_APPROX[cp] = cp - 0xFF01 + 0x21


LOZENGE = 0xD7


def generate():
    """Generate the unicode.h file content."""
    # Build the full 65536-entry table
    table = [LOZENGE] * 65536

    # 0x00-0x7F: ASCII identity
    for i in range(128):
        table[i] = i

    # Exact Mac Roman reverse mappings (highest priority)
    for ucp, mr in UNICODE_TO_MAC_ROMAN.items():
        if ucp < 65536:
            table[ucp] = mr

    # ASCII approximations (only if not already mapped to Mac Roman)
    for ucp, approx in ASCII_APPROX.items():
        if ucp < 65536 and table[ucp] == LOZENGE:
            table[ucp] = approx

    # Also map some Latin-1 supplement chars that have direct Mac Roman equivalents
    # These are in the 0x00A0-0x00FF range and should already be covered by
    # UNICODE_TO_MAC_ROMAN, but add explicit ASCII fallbacks for any gaps
    latin1_ascii = {
        0x00A0: ord(' '),   # NBSP -> space
        0x00AD: ord('-'),   # soft hyphen -> hyphen
        0x00BC: ord(' '),   # ¼ (no good approximation)
        0x00BD: ord(' '),   # ½
        0x00BE: ord(' '),   # ¾
        0x00D0: ord('D'),   # Ð
        0x00D7: ord('x'),   # × (multiplication sign, NOT Mac Roman lozenge!)
        0x00DE: ord('T'),   # Þ
        0x00F0: ord('d'),   # ð
        0x00FE: ord('t'),   # þ
    }
    for ucp, approx in latin1_ascii.items():
        if table[ucp] == LOZENGE:
            table[ucp] = approx

    # Output
    print('#pragma once')
    print('#include <stdint.h>')
    print('')
    print('// printable lozenge character in the Mac Roman encoding')
    print('const char MAC_ROMAN_LOZENGE = 0xD7;')
    print('')
    print('#define LOZENGE_STR "\\xD7"')
    print('')
    print('#define NONE_STR LOZENGE_STR')
    print('')
    print('// Unicode BMP (0x0000-0xFFFF) to Mac Roman translation table')
    print('// Characters with exact Mac Roman equivalents use proper encoding.')
    print('// Others get ASCII approximations or lozenge (0xD7) fallback.')
    print('// Generated by tools/generate_unicode_table.py - do not edit manually.')
    print('const char* UNICODE_BMP_NORMALIZER = ""')

    # Output the table as C string literal
    # Group by 256-byte pages for readability
    for page in range(256):
        base = page * 256
        # Check if this entire page is all lozenges
        page_bytes = table[base:base+256]
        if all(b == LOZENGE for b in page_bytes):
            # Emit a compact representation
            print(f'/* U+{base:04X}-U+{base+255:04X}: all unmapped */')
            for _ in range(16):
                print('"\\xD7\\xD7\\xD7\\xD7\\xD7\\xD7\\xD7\\xD7\\xD7\\xD7\\xD7\\xD7\\xD7\\xD7\\xD7\\xD7"')
            continue

        # Emit with hex escapes
        if page == 0:
            print(f'/* U+{base:04X}-U+{base+255:04X}: ASCII + Latin-1 */')
        else:
            print(f'/* U+{base:04X}-U+{base+255:04X} */')

        for row in range(16):
            row_base = base + row * 16
            parts = []
            for col in range(16):
                b = table[row_base + col]
                parts.append(f'\\x{b:02X}')
            print(f'"{"".join(parts)}"')

    print(';')


if __name__ == '__main__':
    generate()
