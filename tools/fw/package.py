# SPDX-License-Identifier: MIT
# Copyright (c) 2023 Andrey Grigoryev (kagaimiq), 2026 M-VAVE; Python port for Linux builds.
# Byte-for-byte port of sdk/Package.cs (JieLi formats and ciphers from jl-misctools 0a5b12d,
# FM1 layout and writer by M-VAVE). Full MIT terms and source links: THIRD_PARTY.md.
"""Usage: python3 package.py app.bin sdk/tools/pack out.fwsc FM-1_8000014"""
import json, os, re, sys

AREA, ENTRY_POINT, APP_DATA = 0x4000, 0x02000120, 0x120
FLASH_LIMIT, USER_SIZE, BT_SIZE, MINIMUM_VM = 0xff000, 536576, 4096, 32768
PUBLIC_CODE = 0x980F


def make_table():
    table = []
    for i in range(256):
        value = i << 8
        for _ in range(8):
            value = ((value << 1) ^ (0x1021 if value & 0x8000 else 0)) & 0xFFFFFFFF
        table.append(value & 0xFFFF)
    return table


CRC_TABLE = make_table()


def crc(data, begin, length):
    value = 0
    for b in data[begin:begin + length]:
        value = ((value << 8) ^ CRC_TABLE[(value >> 8) ^ b]) & 0xFFFF
    return value


def cipher(data, begin, length, state):
    for i in range(begin, begin + length):
        data[i] ^= state & 0xFF
        state = ((state << 1) ^ (0x1021 if state & 0x8000 else 0)) & 0xFFFF


def round_up(size, alignment):
    return (size + alignment - 1) & -alignment


class Image:
    def __init__(self, size, fill):
        self.b = bytearray([fill]) * size

    def number(self, offset, value, width):
        for n in range(width):
            self.b[offset + n] = (value >> (8 * n)) & 0xFF

    def copy(self, offset, data):
        self.b[offset:offset + len(data)] = data

    def text(self, offset, value):
        self.copy(offset, value.encode('ascii'))

    def checksum(self, field, offset, size):
        self.number(field, crc(self.b, offset, size), 2)

    def record(self, offset, name, crc_value, address, size, attributes, last, extra):
        self.b[offset:offset + 32] = bytes(32)
        self.number(offset + 2, crc_value, 2)
        self.number(offset + 4, address & 0xFFFFFFFF, 4)
        self.number(offset + 8, size & 0xFFFFFFFF, 4)
        self.number(offset + 12, attributes, 2)
        self.number(offset + 14, last, 2)
        for n in range(16, 32):
            self.b[offset + n] = 255
        self.text(offset + 16, name)
        self.b[offset + 16 + len(name)] = 0
        if extra is not None:
            self.number(offset + 28, extra, 4)
        self.checksum(offset, offset + 2, 30)


def read_resource(directory, name, size):
    data = open(os.path.join(directory, name), 'rb').read()
    if len(data) != size:
        raise ValueError('Unexpected FM1 resource: ' + name)
    return data


def u16(b, o): return b[o] | (b[o + 1] << 8)
def i32(b, o): return int.from_bytes(b[o:o + 4], 'little', signed=True)


def encode_boot(plain):
    result = bytearray(plain)
    banks = u16(plain, 0)
    if banks < 1 or banks > len(plain) // 16:
        raise ValueError('Invalid boot directory.')
    previous_end = banks * 16
    for i in range(banks):
        header = i * 16
        length, start = u16(plain, header + 2), i32(plain, header + 8)
        if (length == 0 or start < previous_end or start > len(plain) - length or
                crc(plain, header, 14) != u16(plain, header + 14) or crc(plain, start, length) != u16(plain, header + 12)):
            raise ValueError('Invalid boot bank.')
        cipher(result, header, 16, 0xFFFF)
        cipher(result, start, length, 0xFFFF)
        previous_end = start + length
    return bytes(result)


def build_flash(application, directory, end, vm_end):
    boot = read_resource(directory, 'uboot.boot', 14488)
    config = read_resource(directory, 'boot-config.bin', 721)
    tool = read_resource(directory, 'cfg_tool.bin', 383)
    image, settings = Image(end, 255), Image(34 + len(config), 0)
    for bit in range(16):
        settings.b[bit] = 16
        settings.b[16 + bit] = 16 ^ (255 if ((PUBLIC_CODE >> bit) & 1) == 0 else 0)
    settings.checksum(32, 0, 32)
    settings.copy(34, config)
    image.copy(0, bytes([0, 0, 0x20, 2, 0x2f, 0x10, 0x4c, 0xc9, 0, 0xf0, 0x0f, 0, 0x10, 0x10, 0xa4, 0xff,
                         0x4e, 0x5d, 0x0b, 0x41, 0xe0, 0xcd, 0x78, 0x1d, 0xc8, 0x56, 0x41, 0x7f, 0xb3, 0x67, 0xcf, 0x9f]))
    image.checksum(0, 2, 30)
    config_at = 160 + len(boot)
    image.record(32, 'uboot.boot', crc(boot, 0, len(boot)), 160, len(boot), 0, 0, None)
    image.record(64, 'isd_config.ini', crc(settings.b, 0, len(settings.b)), config_at, len(settings.b), 0x8002, 0, None)
    image.record(96, 'app_dir_head', 0xFFFF, AREA, 0xFFFFFFFF, 0xFF81, 0, None)
    image.record(128, 'key_mac', 0xFFFF, FLASH_LIMIT, 4096, 0x0112, 0xFFFF, None)
    for p in range(0, 160, 32):
        cipher(image.b, p, 32, 0xFFFF)
    image.copy(160, encode_boot(boot))
    image.copy(config_at, settings.b)
    payload = APP_DATA + len(application) + len(tool)
    app = AREA + APP_DATA
    image.copy(app, application)
    image.copy(app + len(application), tool)
    image.record(AREA + 32, 'app.bin', crc(application, 0, len(application)), APP_DATA, len(application), 0xFF82, 0, None)
    image.record(AREA + 64, 'cfg_tool.bin', crc(tool, 0, len(tool)), APP_DATA + len(application), len(tool), 0xFF82, 0, None)
    image.record(AREA + 96, 'VM', 0xFFFF, end, vm_end - end, 0x8112, 0, MINIMUM_VM)
    image.record(AREA + 128, 'PRCT', 0xFFFF, 0, end, 0x8292, 0, 0x45444F43)
    image.record(AREA + 160, 'BTIF', 0xFFFF, vm_end, BT_SIZE, 0x8192, 1, 0x4F545541)
    image.record(AREA, 'app_area_head', crc(image.b, AREA + 32, payload - 32), ENTRY_POINT, payload, 0xFF83, 0, None)
    reserved = AREA + payload
    image.record(reserved + 32, 'USER', 0xFFFF, FLASH_LIMIT - USER_SIZE, USER_SIZE, 0x8192, 1, 0x4F545541)
    image.record(reserved, 'EXT_RESERVED', crc(image.b, reserved + 32, 32), 32, 64, 0xFF93, 1, None)
    for p in range(0, payload + 64, 32):
        cipher(image.b, AREA + p, min(32, payload + 64 - p), PUBLIC_CODE ^ (p >> 2))
    return image


def wrap(flash, loader, product):
    if not 1 <= len(loader) < 32768:
        raise ValueError('Invalid FM1 update loader.')
    loader_at = 1024 + round_up(len(flash.b), 32)
    total = loader_at + round_up(len(loader), 32)
    ufw = Image(total, 255)
    ufw.b[0:224] = bytes(224)
    files, names = [bytes(flash.b), loader], ['flash.bin', 'ota.bin']
    for i, data in enumerate(files):
        record = 64 + i * 80
        ufw.number(record, 0 if i == 0 else 100, 2)
        ufw.number(record + 2, i, 2)
        ufw.number(record + 4, crc(data, 0, len(data)), 2)
        ufw.number(record + 8, 1024 if i == 0 else loader_at, 4)
        ufw.number(record + 12, len(data), 4)
        ufw.number(record + 16, round_up(len(data), 32), 4)
        ufw.text(record + 64, names[i])
        cipher(ufw.b, record, 80, 0xFFFF)
    ufw.number(4, total, 4); ufw.number(8, 2, 2); ufw.number(10, 4, 2); ufw.number(12, 512, 4)
    ufw.text(16, 'AC791N')
    ufw.checksum(2, 64, 160); ufw.checksum(0, 2, 62)
    cipher(ufw.b, 0, 64, 0xFFFF)
    ufw.copy(1024, flash.b)
    ufw.copy(loader_at, loader)
    out = bytearray()
    for source in range(total):
        out.append(ufw.b[source])
        if source < 940 and (source + 1) % 47 == 0:
            index = source // 47
            out.append((ord(product[index]) + index + 1) & 0xFF if index < len(product) else 125)
    return bytes(out)


def create(app, resources, output, user_bytes, product):
    if not re.fullmatch(r'FM-1_8[0-9]{6}', product or ''):
        raise ValueError('Invalid FM1 update identity.')
    if not app or len(app) > FLASH_LIMIT - USER_SIZE - BT_SIZE - MINIMUM_VM - AREA - APP_DATA - 383 - 64 or user_bytes != USER_SIZE:
        raise ValueError('Application does not fit the FM1 layout.')
    image_size = round_up(AREA + APP_DATA + len(app) + 383 + 64, 4096)
    user_at = FLASH_LIMIT - USER_SIZE
    vm_end = user_at - BT_SIZE
    if image_size > vm_end - MINIMUM_VM:
        raise ValueError('Insufficient FM1 settings space.')
    image = build_flash(app, resources, image_size, vm_end)
    package = wrap(image, open(os.path.join(resources, 'ota.bin'), 'rb').read(), product)
    open(output, 'wb').write(package)
    return {'FLASH_BIN_SIZE': image_size, 'VM_START_ADDR': image_size, 'VM_END_ADDR': vm_end,
            'VM_REAL_SIZE': vm_end - image_size, 'USER_RESERVED_START': user_at, 'user_bytes': USER_SIZE,
            'app_bytes': len(app), 'package_bytes': len(package)}


if __name__ == '__main__':
    app_bin, resources, output, product = sys.argv[1:5]
    print(json.dumps(create(open(app_bin, 'rb').read(), resources, output, USER_SIZE, product)))
