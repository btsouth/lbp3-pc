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
- HeroSystem: inventories as (u32 CRC-32 of the class name, u32 length, data); data is 'INVT',
  u32 item count, then per item (u32 class CRC, u32 length, blob), then more fields (loader
  0x101e470). GameEquipInventory holds the weapons: WeaponItemPrius blobs are big-endian, u16
  field mask, u64 item GUID, u64 asset id of configs/equipment/<item>_item.config, then a u16
  inventory index at bytes 22..23 (1001, 1002, ...) and zeros (no XP, level or mods).
  The game keeps added weapon items (it fills a big-endian float at bytes 32..35, likely ammo,
  and drops wpn_bouncer), but its Weapons menu still lists none: something else gates it.
  Asset ids are 0x8000000000000000 | crc64(path) >> 2 (CRC-64/ECMA reflected, seeded with
  its polynomial; 0xe9e120).

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
INVENTORY_MAGIC = b'TVNI'
WEAPON_INDEX = slice(22, 24)


def crc32(text):
    """The game's string CRC-32: reflected table, seeded with the polynomial, no final xor."""
    crc = 0xEDB88320
    for byte in text.encode() if isinstance(text, str) else text:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 if crc & 1 else 0)
    return crc


def asset_id(path):
    """The game's asset id of a path like configs/equipment/wpn_ryno_item.config."""
    poly = 0xC96C5795D7870F42
    crc = poly
    for byte in path.lower().replace('\\', '/').encode():
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (poly if crc & 1 else 0)
    return 1 << 63 | crc >> 2


SECTION_CHECKPOINT = crc32('Checkpoint')
SECTION_HERO = crc32('HeroSystem')
EQUIP_INVENTORY = crc32('GameEquipInventory')
WEAPON_ITEM = crc32('WeaponItemPrius')
# Player weapons and the gadgets that are weapon items (WeaponItemConfig). The other gadgets
# (boots, packs, hoverboard, ...) use their own item classes and are not written.
WEAPONS = ['wpn_blaster', 'wpn_fusion_grenade', 'wpn_pyrocitor', 'wpn_buzzblades',
           'wpn_glove_of_doom', 'wpn_zurkon', 'wpn_groovitron', 'wpn_sheepinator',
           'wpn_plasma_striker', 'wpn_warmonger', 'wpn_predator_launcher', 'wpn_bouncer',
           'wpn_pixelizer', 'wpn_thumper', 'wpn_ryno', 'gdgt_swingshot', 'gdgt_trespasser',
           'gdgt_hologuise', 'gdgt_hydrodisplacer']
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


def rebuild(data, replace):
    """The save with section payloads replaced ({id: bytes}), sections re-laid out in their
    order in the file at 16-byte boundaries, the table and size updated."""
    sections = parse(data)
    first = min(offset for _, offset, _ in sections)
    out = bytearray(data[:first])
    placed = {}
    for sid, offset, length in sorted(sections, key=lambda s: s[1]):
        out += b'\0' * (-len(out) % 16)
        placed[sid] = len(out)
        out += replace.get(sid, data[offset:offset + length])
    for i, (sid, offset, length) in enumerate(sections):
        new = replace.get(sid, data[offset:offset + length])
        struct.pack_into('<III', out, 16 + 12 * i, sid, placed[sid], len(new))
    struct.pack_into('<I', out, 8, len(out))
    return bytes(out)


def equip_items(hero):
    """(record offset, items start, [(offset, class, blob)], items end) of the equip inventory
    in a HeroSystem payload."""
    key = struct.pack('<I', EQUIP_INVENTORY)
    at = hero.find(key)
    while at != -1 and hero[at + 8:at + 12] != INVENTORY_MAGIC:
        at = hero.find(key, at + 1)
    if at == -1:
        raise SaveError('no GameEquipInventory in HeroSystem')
    length = struct.unpack_from('<I', hero, at + 4)[0]
    count = struct.unpack_from('<I', hero, at + 12)[0]
    items, pos = [], at + 16
    for _ in range(count):
        kind, size = struct.unpack_from('<II', hero, pos)
        items.append((pos, kind, hero[pos + 8:pos + 8 + size]))
        pos += 8 + size
    if pos > at + 8 + length:
        raise SaveError('equip inventory items run past its record')
    return at, at + 16, items, pos


def owned_weapons(data):
    offset, length = section(data, SECTION_HERO)
    _, _, items, _ = equip_items(data[offset:offset + length])
    return {int.from_bytes(blob[10:18], 'big') for _, kind, blob in items if kind == WEAPON_ITEM}


def add_weapons(data):
    """The save with every weapon of WEAPONS in the equip inventory (copies of an owned
    WeaponItemPrius with a new GUID, asset id and the next inventory index)."""
    offset, length = section(data, SECTION_HERO)
    hero = data[offset:offset + length]
    at, _, items, end = equip_items(hero)
    weapons = [blob for _, kind, blob in items if kind == WEAPON_ITEM and len(blob) >= 24]
    if not weapons:
        raise SaveError('no weapon item to copy in the equip inventory')
    template = weapons[0]
    owned = {int.from_bytes(blob[10:18], 'big') for blob in weapons}
    index = max(int.from_bytes(blob[WEAPON_INDEX], 'big') for _, _, blob in items
                if len(blob) >= 24)
    added = b''
    for name in WEAPONS:
        aid = asset_id(f'configs/equipment/{name}_item.config')
        if aid in owned:
            continue
        index += 1
        guid = crc32(name) << 32 | crc32(name[::-1])
        blob = bytearray(template)
        blob[2:10] = guid.to_bytes(8, 'big')
        blob[10:18] = aid.to_bytes(8, 'big')
        blob[WEAPON_INDEX] = index.to_bytes(2, 'big')
        added += struct.pack('<II', WEAPON_ITEM, len(blob)) + bytes(blob)
    if not added:
        return data
    record_length, count = struct.unpack_from('<I', hero, at + 4)[0], struct.unpack_from('<I', hero, at + 12)[0]
    new = bytearray(hero[:end] + added + hero[end:])
    struct.pack_into('<I', new, at + 4, record_length + len(added))
    struct.pack_into('<I', new, at + 12, count + len(added) // (8 + len(template)))
    return rebuild(data, {SECTION_HERO: bytes(new)})


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
    owned = owned_weapons(data)
    known = {asset_id(f'configs/equipment/{n}_item.config'): n for n in WEAPONS}
    lines.append(f'weapon items: {len(owned)} '
                 f'({", ".join(sorted(known.get(a, hex(a)) for a in owned)) or "none"})')
    return '\n'.join(lines)


def unlock(data, level=None, weapons=False):
    """A copy with every planet unlocked; with `level`, the game continues at its default spawn;
    with `weapons`, every weapon item of WEAPONS is added."""
    out = bytearray(add_weapons(data) if weapons else data)
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


def write_save_dir(source, target, level=None, weapons=False):
    """Copy a save folder (rcps4/ and rcps4.sce_sys/ of savedata/1/CUSA01073) with rcps4.save
    and rcps4.backup replaced by the unlocked save. The source is only read."""
    source, target = Path(source), Path(target)
    data = (source / 'rcps4' / 'rcps4.save').read_bytes()
    result = unlock(data, level, weapons)
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
    make.add_argument('--weapons', action='store_true',
                      help='also add every weapon item (kept by the game, not yet listed in its Weapons menu)')
    sub.add_parser('levels', help='list levels and spawn points')
    args = parser.parse_args(argv)
    try:
        if args.command == 'info':
            print(info(args.save.read_bytes()))
        elif args.command == 'unlock':
            if args.target.resolve() == args.source.resolve():
                raise SaveError('target must differ from the source')
            print(info(write_save_dir(args.source, args.target, args.level, args.weapons)))
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
