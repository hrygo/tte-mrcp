#!/usr/bin/env python3
"""Compatibility entry point for tools/stress/stress_test.py."""

import os
import pathlib
import sys


ROOT_DIR = pathlib.Path(__file__).resolve().parent
TARGET = ROOT_DIR / "tools" / "stress" / "stress_test.py"
os.execv(sys.executable, [sys.executable, str(TARGET), *sys.argv[1:]])
