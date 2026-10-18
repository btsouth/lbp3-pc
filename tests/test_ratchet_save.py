import struct
import tempfile
import unittest
from pathlib import Path

import paths  # noqa: F401  (puts scripts/ on the import path)
import ratchet_save as rs


def checkpoint_section(level=0, crc=0x1910a3e7, instance=0xc591815c0a4aece8):
    fields = [('CurrentLevel', 6), ('CurrentCheckpoint', 2), ('CurrentCheckpointId', 3)]
    entries = b''.join(struct.pack('<II', rs.crc32(n), kind << 24 | 0x10) for n, kind in fields)
    names = struct.pack('<3I', 0x81, 0x8e, 0xa0)
    values = struct.pack('<iIQ', level, crc, instance)
    body = entries + names + values
    return struct.pack('<IIII', 0, rs.OBJECT_MAGIC, len(fields), len(body)) + body


def macro_section(current=1, mask=0):
    data = bytearray(rs.MACRO_SIZE)
    struct.pack_into('<III', data, 0, current, mask, 2)
    data[rs.MACRO_ORDER + 1] = 1  # Veldin first, as in a new game
    data[0x20:0x2f] = b'\xff' * 15
    return bytes(data)


def weapon_blob(name, index, mask=b'\xff\x3f'):
    aid = rs.asset_id(f'configs/equipment/{name}_item.config')
    return (mask + (0x1122334455667788).to_bytes(8, 'big') + aid.to_bytes(8, 'big') +
            b'\x01\0\0\0' + index.to_bytes(2, 'big') + b'\0' * 95)


def hero_section():
    items = [(rs.crc32('MeleeWeaponItemPrius'), weapon_blob('wpn_wrench', 1001)),
             (rs.WEAPON_ITEM, weapon_blob('gdgt_swingshot', 1002))]
    body = rs.INVENTORY_MAGIC + struct.pack('<I', len(items))
    body += b''.join(struct.pack('<II', kind, len(blob)) + blob for kind, blob in items)
    body += b'\x01\0\0\xff' + b'\0' * 20  # fields after the items
    general = rs.INVENTORY_MAGIC + struct.pack('<II', 0, 0)
    return (b'\0' + struct.pack('<II', rs.crc32('GameGeneralInventory'), len(general)) + general +
            struct.pack('<II', rs.EQUIP_INVENTORY, len(body)) + body + b'\0' * 7)


def build(sections):
    header = 16 + 12 * len(sections)
    table, blob, offset = b'', b'', header
    for sid, payload in sections:
        offset += -offset % 16
        blob += b'\0' * (offset - header - len(blob))
        table += struct.pack('<III', sid, offset, len(payload))
        blob += payload
        offset += len(payload)
    size = header + len(blob)
    return b'1TAD' + struct.pack('<III', 1, size, len(sections)) + table + blob


def sample():
    return build([(rs.SECTION_MACRO, macro_section()), (rs.SECTION_HERO, hero_section()),
                  (rs.crc32('XPSystem'), b'\0' * 8), (rs.SECTION_CHECKPOINT, checkpoint_section())])


class Crc32Tests(unittest.TestCase):
    def test_matches_the_games_hashes(self):
        # Field and section ids seen in a real save, and a Veldin spawn point name.
        self.assertEqual(rs.crc32('CurrentLevel'), 0x0a33c2ea)
        self.assertEqual(rs.crc32('Checkpoint'), 0xd436a987)
        self.assertEqual(rs.crc32('RCPS4MacroSystem'), 0x0028338a)
        self.assertEqual(rs.crc32('VEL_1_02'), 0x1910a3e7)
        self.assertEqual(rs.crc32('WeaponItemPrius'), 0xa89e9c3c)
        self.assertEqual(rs.crc32('GameEquipInventory'), 0x62d731c8)

    def test_asset_ids_match_the_games(self):
        # From the game's layout.csv (built file names) and its save.
        self.assertEqual(rs.asset_id('levels/veldin/veldin.level'), 0x8804d604ded7f9ee)
        self.assertEqual(rs.asset_id('configs\\equipment\\wpn_wrench_item.config'),
                         0xa178983a0ccbec09)


class ParseTests(unittest.TestCase):
    def test_reads_sections_and_checkpoint(self):
        data = sample()
        self.assertEqual(len(rs.parse(data)), 4)
        fields = rs.checkpoint_fields(data)
        self.assertEqual(fields['CurrentLevel'][2], 0)
        self.assertEqual(fields['CurrentCheckpoint'][2], 0x1910a3e7)
        self.assertEqual(fields['CurrentCheckpointId'][2], 0xc591815c0a4aece8)
        self.assertIn('level veldin (0)', rs.info(data))
        self.assertIn('unlocked mask 0x0 (none)', rs.info(data))

    def test_rejects_damaged_files(self):
        data = sample()
        with self.assertRaises(rs.SaveError):
            rs.parse(b'XXXX' + data[4:])
        with self.assertRaises(rs.SaveError):
            rs.parse(data[:-1])
        no_macro = build([(rs.SECTION_CHECKPOINT, checkpoint_section())])
        with self.assertRaises(rs.SaveError):
            rs.unlock(no_macro)
        short_macro = build([(rs.SECTION_MACRO, b'\0' * 100),
                             (rs.SECTION_CHECKPOINT, checkpoint_section())])
        with self.assertRaises(rs.SaveError):
            rs.unlock(short_macro)


class WeaponTests(unittest.TestCase):
    def test_rebuild_without_changes_is_identity(self):
        data = sample()
        self.assertEqual(rs.rebuild(data, {}), data)

    def test_adds_every_weapon_once(self):
        data = sample()
        self.assertEqual(rs.owned_weapons(data),
                         {rs.asset_id('configs/equipment/gdgt_swingshot_item.config')})
        out = rs.add_weapons(data)
        owned = rs.owned_weapons(out)
        self.assertEqual(owned, {rs.asset_id(f'configs/equipment/{n}_item.config')
                                 for n in rs.WEAPONS})
        offset, length = rs.section(out, rs.SECTION_HERO)
        at, _, items, end = rs.equip_items(out[offset:offset + length])
        self.assertEqual(len(items), 1 + len(rs.WEAPONS))
        indices = [int.from_bytes(blob[rs.WEAPON_INDEX], 'big') for _, _, blob in items]
        self.assertEqual(indices, list(range(1001, 1001 + len(items))))
        guids = {blob[2:10] for _, _, blob in items[2:]}
        self.assertEqual(len(guids), len(items) - 2)
        # The fields after the items and the other sections survive the move.
        hero = out[offset:offset + length]
        self.assertEqual(hero[end:end + 4], b'\x01\0\0\xff')
        self.assertEqual(rs.checkpoint_fields(out)['CurrentCheckpoint'][2], 0x1910a3e7)
        self.assertEqual(out[rs.macro(out):rs.macro(out) + 150], macro_section())
        self.assertEqual(rs.add_weapons(out), out)
        self.assertEqual(rs.unlock(data, weapons=True), rs.unlock(out))


class UnlockTests(unittest.TestCase):
    def test_unlocks_every_planet_in_place(self):
        data = sample()
        out = rs.unlock(data)
        self.assertEqual(len(out), len(data))
        m = rs.macro(out)
        current, mask = struct.unpack_from('<II', out, m)
        self.assertEqual(current, 1)
        self.assertEqual(mask, 0xfffe)  # level numbers 1..15
        order = out[m + rs.MACRO_ORDER:m + rs.MACRO_ORDER + rs.LEVEL_COUNT]
        self.assertEqual(order[1], 1)  # Veldin keeps its place
        self.assertEqual(sorted(order[1:16]), list(range(1, 16)))
        self.assertEqual(order[0], 0)
        self.assertEqual(order[16:], b'\0\0\0')
        # Only the mask and order bytes change; the checkpoint stays.
        changed = [i for i in range(len(data)) if data[i] != out[i]]
        self.assertTrue(all(m + 4 <= i < m + 8 or
                            m + rs.MACRO_ORDER <= i < m + rs.MACRO_ORDER + rs.LEVEL_COUNT
                            for i in changed))
        self.assertEqual(rs.unlock(out), out)

    def test_moves_the_checkpoint_to_a_level(self):
        out = rs.unlock(sample(), 'kalebo')
        fields = rs.checkpoint_fields(out)
        self.assertEqual(fields['CurrentLevel'][2], 14)
        self.assertEqual(fields['CurrentCheckpoint'][2], rs.crc32('KAL_default'))
        self.assertEqual(fields['CurrentCheckpointId'][2], 0xf1987dd7466207e0)
        self.assertEqual(struct.unpack_from('<I', out, rs.macro(out))[0], 14)
        with self.assertRaises(rs.SaveError):
            rs.unlock(sample(), 'prequartu')

    def test_level_enums_follow_the_macro_config(self):
        # CurrentLevel is the position in gamemacroconfig's Levels list; the planet tour loaded
        # Deplanetizer_Exterior for 12 and the museum for 16.
        self.assertEqual([row[1] for row in rs.LEVELS], list(range(len(rs.LEVELS))))
        enums = {name: enum for name, enum, *_ in rs.LEVELS}
        self.assertEqual((enums['phoenix'], enums['deplanetizer_exterior'], enums['museum']),
                         (11, 12, 16))
        self.assertNotIn('deplanetizer_umbris', enums)

    def test_writes_a_save_folder_without_touching_the_source(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'source'
            (source / 'rcps4').mkdir(parents=True)
            (source / 'rcps4.sce_sys').mkdir()
            (source / 'rcps4' / 'rcps4.save').write_bytes(sample())
            (source / 'rcps4' / 'rcps4.backup').write_bytes(sample())
            (source / 'rcps4' / 'rcps4_userprefs.save').write_bytes(b'prefs')
            (source / 'rcps4.sce_sys' / 'param.bin').write_bytes(b'param')
            target = Path(tmp) / 'target'
            self.assertEqual(rs.main(['unlock', str(source), str(target), '--level', 'novalis']), 0)
            saved = (target / 'rcps4' / 'rcps4.save').read_bytes()
            self.assertEqual(saved, (target / 'rcps4' / 'rcps4.backup').read_bytes())
            self.assertEqual(rs.checkpoint_fields(saved)['CurrentLevel'][2], 2)
            self.assertEqual((target / 'rcps4' / 'rcps4_userprefs.save').read_bytes(), b'prefs')
            self.assertEqual((source / 'rcps4' / 'rcps4.save').read_bytes(), sample())
            self.assertEqual(rs.main(['unlock', str(source), str(source)]), 1)


if __name__ == '__main__':
    unittest.main()
