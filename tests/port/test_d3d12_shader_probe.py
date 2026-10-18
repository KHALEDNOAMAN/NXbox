# SPDX-FileCopyrightText: Copyright 2026 NSPX
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise shader compile probe policy without a Windows SDK or graphics driver."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RENDERER = ROOT / "src/video_core/renderer_d3d12"


class D3D12ShaderProbeTests(unittest.TestCase):
    def run_cpp(self, body):
        compiler = shutil.which("clang++") or shutil.which("g++")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "probe.cpp"
            executable = Path(directory) / "probe"
            source.write_text(
                "#include <cassert>\n#include <fstream>\n"
                '#include "video_core/renderer_d3d12/d3d12_shader_probe.h"\n'
                "using namespace D3D12::ShaderProbe;\n"
                "int main() {\n" + body + "\n}\n"
            )
            subprocess.run(
                [
                    compiler,
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "src"),
                    str(source),
                    "-o",
                    str(executable),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(executable)], cwd=directory, check=True)

    def test_knob_parsing(self):
        self.run_cpp(r"""
    assert(!Config::Parse(nullptr, nullptr).Enabled());
    assert(!Config::Parse("", "15").Enabled());
    assert(Config::Parse("variants", nullptr).Enabled());
    assert(Config::Parse("variants", nullptr).directory == "variants");
    assert(Config::Parse("/variants", nullptr).directory.is_absolute());
    assert(Config::Parse("variants", nullptr).timeout_s == 45);
    for (const auto* bad : {"", "bad", "45s", "1.5", " 2", "2 ", "+2",
                            "999999999999999999999999999999"})
        assert(Config::Parse("variants", bad).timeout_s == 45);
    assert(Config::Parse("variants", "0").timeout_s == 1);
    assert(Config::Parse("variants", "-100").timeout_s == 1);
    assert(Config::Parse("variants", "1").timeout_s == 1);
    assert(Config::Parse("variants", "75").timeout_s == 75);
    assert(Config::Parse("variants", "600").timeout_s == 600);
    assert(Config::Parse("variants", "601").timeout_s == 600);
    assert(Config::Parse("variants", "2147483648").timeout_s == 600);
""")

    def test_target_first_line(self):
        self.run_cpp(r"""
    assert(Target("") == "cs_6_0");
    assert(Target("void main() {}") == "cs_6_0");
    assert(Target("// target: cs_6_6\nvoid main() {}") == "cs_6_6");
    assert(Target("// target: ps_6_0\r\nvoid main() {}") == "ps_6_0");
    assert(Target("// target: \tcs_6_2 \t\r\n") == "cs_6_2");
    assert(Target("// target: cs_6_1") == "cs_6_1");
    assert(Target("// target:\r\n") == "cs_6_0");
    assert(Target("// comment\n// target: cs_6_6") == "cs_6_0");
    assert(Target(" // target: cs_6_6") == "cs_6_0");
    assert(Target("// targets: cs_6_6") == "cs_6_0");
""")

    def test_error_snippet(self):
        self.run_cpp(r"""
    assert(ErrorSnippet("first\nsecond\r\nthird") == "first second  third");
    assert(ErrorSnippet("\"quoted\" \\path") == "\\\"quoted\\\" \\\\path");
    assert(ErrorSnippet(std::string(350, 'x')) == std::string(300, 'x'));
    assert(ErrorSnippet("").empty());
""")

    def test_stop_after_four_timeouts(self):
        self.run_cpp(r"""
    Counts counts;
    for (int i = 0; i < 10; ++i) {
        counts.Record(Result::Ok);
        counts.Record(Result::Fail);
    }
    assert(!counts.Stopped() && counts.ok == 10 && counts.fail == 10);
    for (int i = 0; i < 3; ++i) {
        counts.Record(Result::Timeout);
        assert(!counts.Stopped());
        counts.Record(Result::Ok); // successes never reset the process timeout budget
    }
    counts.Record(Result::Timeout);
    assert(counts.Stopped() && counts.timeout == 4);
    assert(counts.ok == 13 && counts.fail == 10);
    unsigned started = 0;
    Counts run;
    for (int i = 0; i < 20; ++i) {
        ++started;
        run.Record(Result::Timeout);
        if (run.Stopped()) break;
    }
    assert(started == 4);
""")

    def test_file_ordering_and_filter(self):
        self.run_cpp(r"""
    std::filesystem::create_directory("variants");
    for (const auto* name : {"z.hlsl", "02.hlsl", "01.hlsl", "a.txt", "b.hlsl.bak"})
        std::ofstream{std::filesystem::path{"variants"} / name} << "// variant\n";
    std::filesystem::create_directory("variants/sub.hlsl");
    std::ofstream{"variants/sub.hlsl/nested.hlsl"} << "// nested\n";
    const auto files = Files("variants");
    assert(files.size() == 3);
    assert(files[0].filename() == "01.hlsl");
    assert(files[1].filename() == "02.hlsl");
    assert(files[2].filename() == "z.hlsl");
""")

    def test_opt_in_device_and_lifecycle_wiring(self):
        context = (RENDERER / "d3d12_context.cpp").read_text()
        constructor = context.split("Context::Context() {", 1)[1].split("Context::~Context()", 1)[0]
        self.assertLess(
            constructor.index("D3D12CreateDevice("), constructor.index("StartShaderProbe(")
        )
        gate = constructor.split('std::getenv("NXBOX_D3D12_SHADER_PROBE_DIR")', 1)[1]
        self.assertLess(gate.index("directory && *directory"), gate.index("StartShaderProbe("))
        self.assertIn('std::getenv("NXBOX_D3D12_SHADER_PROBE_TIMEOUT_S")', gate)
        destructor = context.split("Context::~Context() {", 1)[1].split(
            "void Context::LogInfoQueue", 1
        )[0]
        self.assertLess(
            destructor.index("shader_probe_stop->store(true)"), destructor.index("Finish(")
        )
        self.assertNotIn("join", destructor)
        probe = (RENDERER / "d3d12_shader_probe.cpp").read_text()
        self.assertIn("static std::atomic_flag started", probe)
        self.assertIn("started.test_and_set()", probe)
        self.assertIn("std::make_shared<PsoState>()", probe)
        self.assertIn("std::thread worker{[state]", probe)
        self.assertIn("worker.detach()", probe)
        self.assertIn("state->ready.wait_for", probe)
        self.assertIn("CoreApplication::Suspending(", probe)
        self.assertIn("if (stop->load())", probe)
        self.assertIn("if (counts.Stopped())", probe)
        self.assertIn('Diagnostic("SHADER_PROBE stopped after 4 timeouts")', probe)
        self.assertIn('compiler.Compile(source, L"main"', probe)
        self.assertNotIn("DxcCache::", probe)
        self.assertNotIn("DiskPipelineCache", probe)
        self.assertIn("CreateRootSignature(0, state->dxil.data(), state->dxil.size()", probe)
        self.assertIn("desc.pRootSignature = state->root.Get()", probe)
        self.assertIn("THREAD_PRIORITY_BELOW_NORMAL", probe)
        self.assertIn('SetCurrentThreadName("D3D12 shader probe")', probe)
        self.assertIn(
            "renderer_d3d12/d3d12_shader_probe.cpp",
            (ROOT / "src/video_core/CMakeLists.txt").read_text(),
        )
