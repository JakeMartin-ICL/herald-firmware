"""Run the production RFID implementation against a stateful card/reader model."""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile


root = Path(__file__).resolve().parents[1]
compiler = shlex.split(os.environ.get("CXX", "c++"))
with tempfile.TemporaryDirectory(prefix="herald-rfid-tests-") as temporary:
    temporary = Path(temporary)
    # Compile the actual implementation, replacing only its hardware boundary.
    # Copying keeps its quoted globals.h include from selecting the Arduino one.
    shutil.copyfile(root / "src/rfid.cpp", temporary / "production_rfid.cpp")
    executable = temporary / "test_rfid"
    subprocess.run(
        compiler + [
            "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-I", str(temporary), "-I", str(root / "test/rfid_fakes"),
            str(root / "test/test_rfid.cpp"), "-o", str(executable),
        ],
        check=True,
    )
    subprocess.run([str(executable), *sys.argv[1:]], check=True, timeout=10)
