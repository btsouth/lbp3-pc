#!/usr/bin/env python3
"""Ratchet & Clank (2016, CUSA01073) saves: show them and write copies with every planet unlocked.

The port keeps saves decrypted in user/savedata/1/CUSA01073/rcps4/ (rcps4.save and rcps4.backup,
which the game writes identical; rcps4_userprefs.* holds the options). The format, from the
game's save code (eboot file offsets, vaddr + 0x4000):

- DAT1 container: '1TAD', u32 1, u32 file size, u32 section count, then (u32 id, u32 offset,
  u32 size) per section. Section ids are the game's CRC-32 of the system name (crc32() below,
  0xe95100). There is no checksum or hash over the data.
- Checkpoint: u32 0, then a tagged object (magic 0x07201969: u32 count, u32 size, count x (u32
  field hash, u32 type), count x u32 name offset, values). Writer 0x11b3c0b: CurrentLevel (s32
  level enum, LEVELS order), CurrentCheckpoint (u32 CRC-32 of the hero spawn point's name),
  CurrentCheckpointId (u64 instance id of that spawn point in the level file).
- RCPS4MacroSystem: 150 raw bytes (loader 0x4d9370): u32 current level number, u32 unlocked
  level mask (bit n = level number n; IsLevelUnlocked 0x4d9890 is what the galactic map asks),
  u32, 10 + 10 + 15 bytes, 5 single bytes, 19 bytes unlock order (UnlockLevel 0x4d98a0 gives a
  newly unlocked level max + 1), 79 bytes of story flags.

Level numbers come from configs/gamemacroconfig/gamemacroconfig.config, spawn points from each
levels/<name>/<name>.level (hero single spawn points; every level has a <CODE>_default one).
"""
import argparse
import shutil
import struct
import sys
from pathlib import Path

MAGIC = b'1TAD'
OBJECT_MAGIC = 0x07201969
VALUE_SIZES = {2: 4, 3: 8, 6: 4}  # types in the Checkpoint object: u32, u64, s32
MACRO_SIZE = 150
MACRO_CURRENT, MACRO_MASK, MACRO_ORDER = 0, 4, 0x34
LEVEL_COUNT = 19  # unlock order bytes: level numbers 0..18


def crc32(text):
    """The game's string CRC-32: reflected table, seeded with the polynomial, no final xor."""
    crc = 0xEDB88320
    for byte in text.encode() if isinstance(text, str) else text:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 if crc & 1 else 0)
    return crc


SECTION_CHECKPOINT = crc32('Checkpoint')
SECTION_MACRO = crc32('RCPS4MacroSystem')
SECTION_NAMES = {crc32(n): n for n in (
    'Checkpoint', 'HeroSystem', 'WeaponUpgradeSystem', 'XPSystem', 'ActorSaveSystem',
    'EmergentVOSystem', 'CardTradingSystem', 'RCPS4MacroSystem')}

# (level, enum, level number or 0, default spawn point, its instance id). Enum = CurrentLevel.
LEVELS = [
    ('veldin', 0, 1, 'VEL_default', 0x151b880f73e3581c),
    ('prequartu', 1, 2, None, None),
    ('novalis', 2, 3, 'NOV_default', 0xf92850df6bb792a2),
    ('aridia', 3, 4, 'ARI_default', 0x1b875177ce5ec4b6),
    ('metropolis', 4, 5, 'MET_default', 0x57fe400b10533c07),  # Kerwan
    ('rilgar', 5, 6, 'RIL_default', 0xbbe07ff240ffcf54),
    ('nebula', 6, 7, 'NEB_default', 0x89e7d3442cd83c6d),
    ('gaspar', 7, 8, 'GAS_default', 0xda5c21b4f04a2c23),
    ('batalia', 8, 9, 'BAT_default', 0xf3bdef66fe21182f),
    ('quartu', 9, 10, 'QUA_default', 0xede3551dce910b28),
    ('pokitaru', 10, 11, 'POK_default', 0x177af2db445f77f8),
    ('deplanetizer_exterior', 11, 0, 'DEP_EXT_1_01', 0xc1aeb6dd8a0cd919),
    ('phoenix', 12, 12, 'PHO_default', 0x3a5c4ffdac0a7eec),
    ('deplanetizer', 13, 13, 'DEP_default', 0xa0700b32f8895a0d),
    ('kalebo', 14, 14, 'KAL_default', 0xf1987dd7466207e0),
    ('deplanetizer_umbris', 15, 0, None, None),
    ('deplanetizer_nefarious', 16, 15, 'DEP_9_01', 0xd86dce851b468d14),
    ('museum', 17, 0, 'MUS_default', 0xb585e98d224d522a),
]
LEVEL_BY_NAME = {row[0]: row for row in LEVELS}
LEVEL_BY_ENUM = {row[1]: row for row in LEVELS}
# Planets on the galactic map, in story order (level numbers 1..15 without Pre-Quartu).
ALL_LEVEL_MASK = sum(1 << number for _, _, number, _, _ in LEVELS if number)


class SaveError(ValueError):
    pass


def parse(data):
    """[(section id, offset, size)] of a DAT1 save, checked against the file."""
    if len(data) < 16 or data[:4] != MAGIC:
        raise SaveError('not a DAT1 save (no 1TAD header)')
    _, size, count = struct.unpack_from('<III', data, 4)
    if size != len(data):
        raise SaveError(f'header size {size} but file is {len(data)} bytes')
    sections = [struct.unpack_from('<III', data, 16 + 12 * i) for i in range(count)]
    for sid, offset, length in sections:
        if offset + length > len(data):
            raise SaveError(f'section {sid:#x} runs past the end')
    return sections


def section(data, sid):
    for found, offset, length in parse(data):
        if found == sid:
            return offset, length
    raise SaveError(f'no section {SECTION_NAMES.get(sid, hex(sid))}')


def checkpoint_fields(data):
    """{field name: (offset, type, value)} of the Checkpoint object."""
    offset, length = section(data, SECTION_CHECKPOINT)
    obj = offset + 4
    magic, count, size = struct.unpack_from('<III', data, obj)
    if magic != OBJECT_MAGIC or obj + 12 + size > offset + length:
        raise SaveError('unexpected Checkpoint object')
    entries = [struct.unpack_from('<II', data, obj + 12 + 8 * i) for i in range(count)]
    value = obj + 12 + 12 * count
    value += -value % 4
    names = {crc32(n): n for n in ('CurrentLevel', 'CurrentCheckpoint', 'CurrentCheckpointId')}
    fields = {}
    for field_hash, info in entries:
        kind = info >> 24
        if field_hash not in names or VALUE_SIZES.get(kind) is None:
            raise SaveError(f'unexpected Checkpoint field {field_hash:#x} type {kind}')
        width = VALUE_SIZES[kind]
        fmt = {4: '<i' if kind == 6 else '<I', 8: '<Q'}[width]
        fields[names[field_hash]] = (value, fmt, struct.unpack_from(fmt, data, value)[0])
        value += width
    return fields


def macro(data):
    offset, length = section(data, SECTION_MACRO)
    if length != MACRO_SIZE:
        raise SaveError(f'RCPS4MacroSystem is {length} bytes, expected {MACRO_SIZE}')
    return offset


def info(data):
    """Readable summary of what this tool understands."""
    lines = [f'{len(data)} bytes, sections: ' + ', '.join(
        f'{SECTION_NAMES.get(sid, hex(sid))}@{off:#x}+{size:#x}' for sid, off, size in parse(data))]
    fields = checkpoint_fields(data)
    level = fields['CurrentLevel'][2]
    lines.append(f"checkpoint: level {LEVEL_BY_ENUM.get(level, ('?',))[0]} ({level}), "
                 f"spawn crc {fields['CurrentCheckpoint'][2]:#x}, "
                 f"spawn id {fields.get('CurrentCheckpointId', (0, 0, 0))[2]:#x}")
    m = macro(data)
    current, mask = struct.unpack_from('<II', data, m)
    unlocked = [row[0] for row in LEVELS if row[2] and mask >> row[2] & 1]
    lines.append(f'macro: current level number {current}, unlocked mask {mask:#x} '
                 f'({", ".join(unlocked) or "none"})')
    return '\n'.join(lines)


def unlock(data, level=None):
    """A copy with every planet unlocked; with `level`, the game continues at its default spawn."""
    out = bytearray(data)
    m = macro(out)
    _, mask = struct.unpack_from('<II', out, m)
    order = out[m + MACRO_ORDER:m + MACRO_ORDER + LEVEL_COUNT]
    for _, _, number, _, _ in LEVELS:
        if number and not order[number]:
            order[number] = max(order) + 1
    out[m + MACRO_ORDER:m + MACRO_ORDER + LEVEL_COUNT] = order
    struct.pack_into('<I', out, m + MACRO_MASK, mask | ALL_LEVEL_MASK)
    if level is not None:
        row = LEVEL_BY_NAME.get(level)
        if row is None or row[3] is None:
            raise SaveError(f'no spawn point known for level {level!r}')
        name, enum, number, spawn, instance = row
        fields = checkpoint_fields(out)
        for field, value in (('CurrentLevel', enum), ('CurrentCheckpoint', crc32(spawn)),
                             ('CurrentCheckpointId', instance)):
            if field not in fields:
                raise SaveError(f'Checkpoint has no {field}')
            offset, fmt, _ = fields[field]
            struct.pack_into(fmt, out, offset, value)
        if number:
            struct.pack_into('<I', out, m + MACRO_CURRENT, number)
    return bytes(out)


def write_save_dir(source, target, level=None):
    """Copy a save folder (rcps4/ and rcps4.sce_sys/ of savedata/1/CUSA01073) with rcps4.save
    and rcps4.backup replaced by the unlocked save. The source is only read."""
    source, target = Path(source), Path(target)
    data = (source / 'rcps4' / 'rcps4.save').read_bytes()
    result = unlock(data, level)
    if target.exists():
        shutil.rmtree(target)
    shutil.copytree(source, target)
    for name in ('rcps4.save', 'rcps4.backup'):
        (target / 'rcps4' / name).write_bytes(result)
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = parser.add_subparsers(dest='command', required=True)
    show = sub.add_parser('info', help='summarise a rcps4.save')
    show.add_argument('save', type=Path)
    make = sub.add_parser('unlock', help='write a save folder with every planet unlocked')
    make.add_argument('source', type=Path, help='savedata/1/CUSA01073 folder to copy (read only)')
    make.add_argument('target', type=Path, help='new CUSA01073 folder (replaced)')
    make.add_argument('--level', choices=[r[0] for r in LEVELS if r[3]],
                      help='continue at this level\'s default spawn point')
    sub.add_parser('levels', help='list levels and spawn points')
    args = parser.parse_args(argv)
    try:
        if args.command == 'info':
            print(info(args.save.read_bytes()))
        elif args.command == 'unlock':
            if args.target.resolve() == args.source.resolve():
                raise SaveError('target must differ from the source')
            print(info(write_save_dir(args.source, args.target, args.level)))
        else:
            for name, enum, number, spawn, instance in LEVELS:
                print(f'{enum:2} {name:24} number {number:2} {spawn or "-":14} '
                      f'{instance or 0:#018x}')
    except (OSError, SaveError) as error:
        print(f'ratchet_save: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
