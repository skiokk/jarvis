#!/usr/bin/env python3
"""Convert an esp-sr packed model image (srmodels.bin format) into a C/C++ header.

Usage (run with -I, stdlib only):
  python3 -I gen_model_header.py <srmodels.bin> <out.h> [--symbol SR_MODEL_JARVIS]
         [--esp-sr-commit <sha>] [--esp-sr-version <ver>]

The image layout (esp-sr model/pack_model.py, little endian):
  u32 model_num
  model_num x { char name[32]; u32 file_num; file_num x { char fname[32]; u32 offset; u32 len } }
  file data (offsets are absolute from image start, no padding/alignment)
The script validates that layout before emitting anything.
"""
import argparse
import binascii
import hashlib
import os
import struct
import sys


def fnv1a32(data: bytes) -> int:
    h = 0x811C9DC5
    for b in data:
        h ^= b
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


def parse_image(d: bytes):
    if len(d) < 4:
        raise ValueError("image too small")
    n = struct.unpack_from("<I", d, 0)[0]
    if n == 0 or n > 64:
        raise ValueError(f"implausible model count {n}")
    p = 4
    models = []
    for _ in range(n):
        if p + 36 > len(d):
            raise ValueError("truncated model header")
        name = d[p:p + 32].split(b"\0")[0].decode("ascii")
        p += 32
        fn = struct.unpack_from("<I", d, p)[0]
        p += 4
        files = []
        for _ in range(fn):
            if p + 40 > len(d):
                raise ValueError("truncated file header")
            fname = d[p:p + 32].split(b"\0")[0].decode("ascii")
            p += 32
            off, ln = struct.unpack_from("<II", d, p)
            p += 8
            if off + ln > len(d):
                raise ValueError(f"{name}/{fname}: data out of bounds")
            info = None
            if fname == "_MODEL_INFO_":
                txt = d[off:off + ln].decode("utf-8", "replace")
                info = [l for l in txt.splitlines() if l and not l.startswith("#")]
                info = info[0] if info else ""
            files.append((fname, off, ln, info))
        models.append((name, files))
    return models, p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("out")
    ap.add_argument("--symbol", default="SR_MODEL_JARVIS")
    ap.add_argument("--esp-sr-commit", default="unknown")
    ap.add_argument("--esp-sr-version", default="unknown")
    a = ap.parse_args()

    with open(a.image, "rb") as f:
        d = f.read()
    models, header_end = parse_image(d)
    crc = binascii.crc32(d) & 0xFFFFFFFF
    fnv = fnv1a32(d)
    sha = hashlib.sha256(d).hexdigest()
    sym = a.symbol
    erase = (len(d) + 4095) // 4096 * 4096

    lines = []
    lines.append("// AUTO-GENERATED FILE - DO NOT EDIT.")
    lines.append("//")
    lines.append("// esp-sr packed model image (srmodels.bin format, see esp-sr model/pack_model.py),")
    lines.append("// embedded in the firmware so it can be copied to the \"model\" data partition")
    lines.append("// (or loaded directly with srmodel_load()).")
    lines.append("//")
    lines.append(f"// Models in image ({len(models)}):")
    for name, files in models:
        lines.append(f"//   {name}")
        for fname, off, ln, info in files:
            extra = f"  info=\"{info}\"" if info is not None else ""
            lines.append(f"//     {fname:<14} offset={off:<7} len={ln}{extra}")
    lines.append(f"// Header length: {header_end} bytes; file offsets are NOT padded/aligned (same as")
    lines.append("// the official srmodels.bin shipped with arduino-esp32, where wn9_data is also at an")
    lines.append("// offset == 2 mod 4).")
    lines.append("//")
    lines.append(f"// Source: https://github.com/espressif/esp-sr  version {a.esp_sr_version}")
    lines.append(f"//         commit {a.esp_sr_commit}")
    lines.append("//         (same esp-sr bundled in arduino-esp32 3.3.12 esp32s3-libs; headers and")
    lines.append("//         prebuilt libs are byte-identical to that commit)")
    lines.append(f"// Image size: {len(d)} bytes (erase size rounded to 4 KiB sectors: {erase} bytes)")
    lines.append(f"// SHA-256:    {sha}")
    lines.append(f"// CRC-32:     0x{crc:08X}  (IEEE/zlib; equals esp_rom_crc32_le(0, data, len))")
    lines.append(f"// FNV-1a 32:  0x{fnv:08X}")
    lines.append("//")
    lines.append("// Regenerate:")
    lines.append("//   git clone https://github.com/espressif/esp-sr && git -C esp-sr checkout <commit>")
    lines.append("//   mkdir -p pack/wn9_jarvis_tts && cp esp-sr/model/wakenet_model/wn9_jarvis_tts/* pack/wn9_jarvis_tts/")
    lines.append("//   python3 -I esp-sr/model/pack_model.py -m pack -o srmodels_jarvis.bin   # writes pack/srmodels_jarvis.bin")
    lines.append("//   python3 -I gen_model_header.py pack/srmodels_jarvis.bin sr_model_jarvis.h"
                 " --esp-sr-commit <commit> --esp-sr-version <ver>")
    lines.append("// Note: pack_model.py orders files in os.walk() order (filesystem dependent); a")
    lines.append("// different order yields a different (equally valid) image and hash.")
    lines.append("#pragma once")
    lines.append("#include <stdint.h>")
    lines.append("#ifndef __cplusplus")
    lines.append("#include <stdalign.h>")
    lines.append("#endif")
    lines.append("")
    first_name = models[0][0]
    lines.append(f"#define {sym}_NAME \"{first_name}\"")
    lines.append("")
    lines.append(f"alignas(4) static const uint8_t {sym}[] = {{")
    for i in range(0, len(d), 16):
        chunk = d[i:i + 16]
        lines.append("  " + ",".join(f"0x{b:02X}" for b in chunk) + ",")
    lines.append("};")
    lines.append("")
    lines.append(f"static const uint32_t {sym}_LEN = {len(d)}u;")
    lines.append(f"static const uint32_t {sym}_HASH = 0x{crc:08X}u;  // CRC-32 IEEE (zlib) of the whole image")
    lines.append(f"static const uint32_t {sym}_FNV1A = 0x{fnv:08X}u; // FNV-1a 32 of the whole image")
    lines.append("")

    tmp = a.out + ".tmp"
    with open(tmp, "w", newline="\n") as f:
        f.write("\n".join(lines))
    os.replace(tmp, a.out)
    print(f"wrote {a.out}: {len(d)} bytes, crc32=0x{crc:08X}, fnv1a=0x{fnv:08X}, sha256={sha}")
    for name, files in models:
        print(f"  {name}: " + ", ".join(f"{fn}@{off}+{ln}" for fn, off, ln, _ in files))


if __name__ == "__main__":
    sys.exit(main())
