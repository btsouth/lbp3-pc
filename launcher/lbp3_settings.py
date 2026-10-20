# SPDX-License-Identifier: GPL-2.0-or-later
"""Settings of the LittleBigPlanet 3 launcher (lbp3_launcher.py), without GTK so they can be tested.

The window is the Ratchet & Clank launcher's; this module gives it LittleBigPlanet 3's choices.
Launcher choices live in ~/.config/lbp3-port-launcher/settings.json and reach run.sh as environment
variables; the FPS counter is a bbport.ini key the game reads. run.sh adds everything else the game
needs by its title ID. The game runs at 30 FPS, as on PS4, and keeps its own anti-aliasing: no
choice of either.
"""

import json
import os
from pathlib import Path

import ratchet_settings as common
from ratchet_settings import DATA_DIR, LANGUAGES, PORT_DIR, game_check, ini_path, read_ini, sfo, user_dir  # noqa: F401

CONFIG_DIR = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "lbp3-port-launcher"
TITLE = "LittleBigPlanet 3"
GAME_DESCRIPTION = "Your own dump of the European release, CUSA00063 version 01.00"
APP_ID = "io.github.lbp3_port.Launcher"
LOG_NAME = "lbp3-port"
FPS_MODES = []
UPSCALERS = []

DEFAULTS = {
    "game_dir": "",
    "user_dir": "",
    "user_name": "",
    "fullscreen": False,
    "display": "",
    "gamepad": "",
    "gamepad_name": "",
    "language": "1",
    "save_log": False,
    "extra_env": "",
}


def load_settings(config_dir=None):
    settings = dict(DEFAULTS)
    try:
        settings.update(json.loads((Path(config_dir or CONFIG_DIR) / "settings.json").read_text(encoding="utf-8")))
    except (OSError, ValueError):
        pass
    return settings


def save_settings(settings, config_dir=None):
    common.save_settings(settings, config_dir or CONFIG_DIR)


def write_ini(changes, path=None):
    common.write_ini(changes, path, title=TITLE)


def game_status(path):
    """(level, text) for a game folder: level 'ok', 'warning' (starts anyway) or 'error'."""
    path = Path(path).expanduser()
    if not (path / "eboot.bin").is_file():
        return "error", "No eboot.bin here. Pick the CUSA00063 folder of your own dump."
    try:
        info = sfo((path / "sce_sys/param.sfo").read_bytes())
    except (OSError, ValueError, IndexError):
        info = {}
    title, version = info.get("TITLE_ID", "?"), info.get("APP_VER", "?")
    if title != game_check.LBP3_TITLE:
        return "error", (f"This is not LittleBigPlanet 3 (found {title}, sce_sys/param.sfo). The port "
                         "runs the European release CUSA00063, version 01.00.")
    found = game_check.problem(path)
    if found is None:
        return "ok", f"LittleBigPlanet 3 {title}, version {version}"
    return ("warning" if found[0] in game_check.WARNINGS else "error"), game_check.explain(*found)


def game_environment(settings, base=None):
    """Environment for run.sh from the launcher settings."""
    s = settings
    env = dict(os.environ if base is None else base)
    env["BB_GAME_DIR"] = str(Path(s["game_dir"]).expanduser())
    if s["user_dir"]:
        env["BB_USER_DIR"] = str(Path(s["user_dir"]).expanduser())
    if s["user_name"]:
        env["BB_USER_NAME"] = s["user_name"]
    env["BB_MODS_ENABLED"] = "0"  # loose-file mods are Bloodborne's
    env["BB_LANGUAGE"] = s["language"]
    env["BB_MENU_LANGUAGE"] = "en"
    env["BB_FULLSCREEN"] = "1" if s["fullscreen"] else "0"
    if s["gamepad"]:
        env["BB_GAMEPAD"] = s["gamepad"]
    if s["display"]:
        env["BB_DISPLAY"] = s["display"]
    if s["save_log"]:
        env["BB_SAVE_LOG"] = "1"
    for item in s["extra_env"].split():
        if "=" in item:
            key, value = item.split("=", 1)
            env[key] = value
    return env
