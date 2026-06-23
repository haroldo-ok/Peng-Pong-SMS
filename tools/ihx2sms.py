#!/usr/bin/env python3
"""Convert Intel HEX to SMS ROM with header checksum patch."""
import sys, struct

def ihx_to_sms(ihx, out, size=32768):
    rom = bytearray(b'\xFF' * size)
    with open(ihx) as f:
        for line in f:
            line = line.strip()
            if not line.startswith(':'): continue
            raw = bytes.fromhex(line[1:])
            cnt, addr, rtype = raw[0], (raw[1]<<8)|raw[2], raw[3]
            if rtype == 0: 
                for i in range(cnt):
                    if addr + i < size: rom[addr+i] = raw[4+i]
            elif rtype == 1: break
    # Patch SMS checksum
    chk = sum(rom[0:0x7FF0]) & 0xFFFF
    rom[0x7FFA] = chk & 0xFF
    rom[0x7FFB] = (chk >> 8) & 0xFF
    with open(out, 'wb') as f: f.write(rom)
    print(f"OK: {out} ({size//1024}KB)")

if __name__ == '__main__':
    ihx_to_sms(sys.argv[1], sys.argv[2])
