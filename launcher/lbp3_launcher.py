#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""LittleBigPlanet 3 launcher: the Ratchet & Clank launcher's window (ratchet_launcher.py) with
LittleBigPlanet 3's settings (lbp3_settings.py). --play starts the game with the saved settings."""

import sys

import lbp3_settings
import ratchet_launcher

ratchet_launcher.rs = lbp3_settings

if __name__ == "__main__":
    sys.exit(ratchet_launcher.main())
