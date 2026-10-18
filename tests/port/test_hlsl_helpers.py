# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later

"""Compile and execute HLSL binding, root ABI, and storage-image IR tests."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class HlslHelperTests(unittest.TestCase):
    def test_storage_image_formats(self):
        root = Path(__file__).resolve().parents[2]
        cmake = shutil.which("cmake")
        binary = os.environ.get("NXBOX_HLSL_IR_TEST_BINARY")
        if not binary and not cmake:
            self.skipTest("CMake required to build HLSL IR fixtures")
        with tempfile.TemporaryDirectory(prefix="nxbox-hlsl-images-") as directory:
            if not binary:
                build = Path(directory) / "build"
                subprocess.run(
                    [cmake, "-S", str(root / "tests/port"), "-B", str(build)],
                    check=True,
                    capture_output=True,
                    text=True,
                    timeout=180,
                )
                # The real IR harness requires fmt and Boost, but no renderer or GPU.
                if not (build / "CMakeFiles/hlsl-ir-tests.dir").exists():
                    self.skipTest("fmt/Boost unavailable for the HLSL IR harness")
                subprocess.run(
                    [cmake, "--build", str(build), "--target", "hlsl-ir-tests", "--parallel", "2"],
                    check=True,
                    capture_output=True,
                    text=True,
                    timeout=600,
                )
                binary = str(build / "hlsl-ir-tests")
            result = subprocess.run(
                [binary],
                cwd=directory,
                capture_output=True,
                text=True,
                timeout=120,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            sources = list(Path(directory).glob("hlsl_storage_*.hlsl"))
            self.assertEqual(len(sources), 6)
            for source in sources:
                text = source.read_text()
                if "<float4> img0" in text:
                    self.assertIn("= asuint(img0[", text)
                    self.assertIn("] = asfloat(", text)
                else:
                    self.assertIn("<int> img0", text)
                    self.assertIn("] = asint(", text)

    def test_host_helpers(self):
        compiler = shutil.which(os.environ.get("CXX", "c++"))
        if compiler is None:
            self.skipTest("A host C++ compiler is required")
        root = Path(__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory(prefix="nxbox-hlsl-") as directory:
            binary = Path(directory) / "hlsl-helper-tests"
            subprocess.run(
                [
                    compiler,
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pedantic",
                    f"-I{root / 'src'}",
                    str(root / "tests/port/hlsl_helpers.cpp"),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
                timeout=60,
            )
            subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
