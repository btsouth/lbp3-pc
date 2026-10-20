"""LittleBigPlanet 3 launcher settings (launcher/lbp3_settings.py): environment, settings file, game check."""
from paths import ROOT
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(ROOT / 'launcher'))
import lbp3_settings as ls  # noqa: E402
import game_check  # noqa: E402
from test_game_check import param_sfo  # noqa: E402
from test_prepare import fixture  # noqa: E402


class EnvironmentTests(unittest.TestCase):
    def test_defaults_leave_the_frame_rate_to_run_sh(self):
        env = ls.game_environment(dict(ls.DEFAULTS, game_dir='/games/CUSA00063'), base={})
        self.assertEqual(env, {'BB_GAME_DIR': '/games/CUSA00063', 'BB_MODS_ENABLED': '0',
                               'BB_LANGUAGE': '1', 'BB_MENU_LANGUAGE': 'en', 'BB_FULLSCREEN': '0'})

    def test_choices_and_extra_variables(self):
        settings = dict(ls.DEFAULTS, game_dir='/g', fullscreen=True, gamepad='0300abcd', display='DP-1',
                        save_log=True, user_name='Sackgirl', extra_env='BB_FPS=60 junk')
        env = ls.game_environment(settings, base={'HOME': '/home/x'})
        self.assertEqual((env['BB_FULLSCREEN'], env['BB_GAMEPAD'], env['BB_DISPLAY'], env['BB_SAVE_LOG'],
                          env['BB_USER_NAME'], env['BB_FPS'], env['HOME']),
                         ('1', '0300abcd', 'DP-1', '1', 'Sackgirl', '60', '/home/x'))
        self.assertNotIn('junk', env)

    def test_no_frame_rate_or_upscaler_choice(self):
        self.assertEqual((ls.FPS_MODES, ls.UPSCALERS), ([], []))


class SettingsFileTests(unittest.TestCase):
    def test_round_trip_in_its_own_folder(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(ls.load_settings(directory), ls.DEFAULTS)
            ls.save_settings(dict(ls.DEFAULTS, user_name='Sackboy'), directory)
            self.assertEqual(json.loads((Path(directory) / 'settings.json').read_text())['user_name'], 'Sackboy')
            self.assertEqual(ls.load_settings(directory)['user_name'], 'Sackboy')
        self.assertEqual(ls.CONFIG_DIR.name, 'lbp3-port-launcher')

    def test_new_ini_is_headed_with_the_game(self):
        with tempfile.TemporaryDirectory() as directory:
            ini = Path(directory) / 'bbport.ini'
            ls.write_ini({'show_fps': '1'}, ini)
            self.assertEqual(ini.read_text(), '# LittleBigPlanet 3 port settings\nshow_fps=1\n')


class GameStatusTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def game(self, title, version):
        (self.path / 'sce_sys').mkdir(exist_ok=True)
        (self.path / 'sce_sys/param.sfo').write_bytes(param_sfo({'APP_VER': version, 'TITLE_ID': title}))
        (self.path / 'eboot.bin').write_bytes(fixture())
        return self.path

    def test_empty_folder(self):
        self.assertEqual(ls.game_status(self.path)[0], 'error')

    def test_supported(self):
        game = self.game('CUSA00063', '01.00')
        with mock.patch.object(game_check, 'LBP3_IMAGE', game_check.image_sha256(game)):
            self.assertEqual(ls.game_status(game), ('ok', 'LittleBigPlanet 3 CUSA00063, version 01.00'))

    def test_other_executable_warns(self):
        level, text = ls.game_status(self.game('CUSA00063', '01.00'))
        self.assertEqual(level, 'warning')
        self.assertIn('not been tested', text)

    def test_ratchet_is_not_lbp3(self):
        level, text = ls.game_status(self.game('CUSA01073', '01.00'))
        self.assertEqual(level, 'error')
        self.assertIn('not LittleBigPlanet 3', text)


if __name__ == '__main__':
    unittest.main()
