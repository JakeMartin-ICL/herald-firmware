"""Compile and run the production sector-expansion code without ESP32 tools."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile


root = Path(__file__).resolve().parents[1]
compiler = shlex.split(os.environ.get("CXX", "c++"))
with tempfile.TemporaryDirectory(prefix="herald-led-tests-") as temporary:
    executable = Path(temporary) / "test_led_sectors"
    subprocess.run(
        compiler + [
            "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(root / "src"), str(root / "test/test_led_sectors.cpp"),
            "-o", str(executable),
        ],
        check=True,
    )
    subprocess.run([str(executable)], check=True, timeout=10)
