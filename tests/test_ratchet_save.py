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
    return build([(rs.SECTION_MACRO, macro_section()), (rs.crc32('XPSystem'), b'\0' * 8),
                  (rs.SECTION_CHECKPOINT, checkpoint_section())])


class Crc32Tests(unittest.TestCase):
    def test_matches_the_games_hashes(self):
        # Field and section ids seen in a real save, and a Veldin spawn point name.
        self.assertEqual(rs.crc32('CurrentLevel'), 0x0a33c2ea)
        self.assertEqual(rs.crc32('Checkpoint'), 0xd436a987)
        self.assertEqual(rs.crc32('RCPS4MacroSystem'), 0x0028338a)
        self.assertEqual(rs.crc32('VEL_1_02'), 0x1910a3e7)


class ParseTests(unittest.TestCase):
    def test_reads_sections_and_checkpoint(self):
        data = sample()
        self.assertEqual(len(rs.parse(data)), 3)
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
