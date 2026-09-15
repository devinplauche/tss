"""Tests for the loopback harness generator (Phase 3)."""

import subprocess

import pytest

from scaffold.gen_harness import (
    default_regions,
    emit_harness_c,
    emit_cmake_harness,
    CTEST_BASE_PORT,
)
from scaffold.gen_uop import (
    extract_regions,
    generate_tree,
    write_tree,
    main as gen_main,
)
from scaffold.model import load_descriptor

SENSOR = "/home/hatch/workspace/uop-scaffolder/phase0/sensor_uop.yaml"
TSS_ROOT = "/home/hatch/workspace/tss"


@pytest.fixture(scope="module")
def model():
    return load_descriptor(SENSOR)


def test_mirror_config(model):
    """Harness flips every direction and role."""
    src, orphans = emit_harness_c(model, {})
    assert orphans == []
    # UoP subscribes RAW_DETECTION -> harness publishes it
    assert "send_RAW_DETECTION" in src
    assert "recv_RAW_DETECTION" not in src
    # UoP publishes FUSED_TRACK -> harness subscribes to it
    assert "recv_FUSED_TRACK" in src
    assert "send_FUSED_TRACK" not in src
    # flipped enums adjacent to the connection names
    i = src.index('"RAW_DETECTION"')
    assert "FACE_TSS_SOURCE" in src[i:i + 400]
    assert "FACE_TSS_ROLE_PUBLISHER" in src[i:i + 400]
    j = src.index('"FUSED_TRACK"')
    assert "FACE_TSS_DESTINATION" in src[j:j + 400]
    assert "FACE_TSS_ROLE_SUBSCRIBER" in src[j:j + 400]


def test_spawn_and_reap(model):
    src, _ = emit_harness_c(model, {})
    assert "fork()" in src
    assert "execl(argv[1], argv[1], portstr" in src
    assert "kill(child, SIGTERM)" in src
    assert "waitpid(child, &status, 0)" in src
    assert "WIFEXITED(status) && WEXITSTATUS(status) == 0" in src


def test_drive_region_fails_loud_by_default(model):
    regions = default_regions(model)
    assert "drive" in regions
    assert "failures++" in regions["drive"]
    src, _ = emit_harness_c(model, {})
    assert "/* USER CODE BEGIN: drive */" in src


def test_drive_region_preserved(model):
    src1, _ = emit_harness_c(model, {})
    regions = extract_regions(src1)
    regions["drive"] = "    CHECK(1, \"custom\");\n"
    src2, orphans = emit_harness_c(model, regions)
    assert orphans == []
    assert 'CHECK(1, "custom");' in src2
    assert "not implemented" not in src2
    src3, _ = emit_harness_c(model, extract_regions(src2))
    assert src3 == src2


def test_cmake_registers_ctest(model):
    cmake = emit_cmake_harness(model)
    assert "add_executable(sensor_uop_harness sensor_uop_harness.c)" in cmake
    assert "enable_testing()" in cmake
    assert "add_test(NAME sensor_uop_loopback" in cmake
    assert "$<TARGET_FILE:sensor_uop>" in cmake
    assert str(CTEST_BASE_PORT) in cmake


def test_cli_emits_harness(tmp_path):
    out = tmp_path / "gen"
    assert gen_main([SENSOR, "--out", str(out)]) == 0
    assert (out / "sensor_uop_harness.c").exists()
    assert "sensor_uop_loopback" in (out / "CMakeLists.txt").read_text()


def test_harness_compiles_clean(tmp_path, model):
    """Generated harness (default drive) builds under -Werror."""
    out = tmp_path / "gen"
    files, _ = generate_tree(model, {}, {})
    write_tree(out, files)
    exe = tmp_path / "harness_bin"
    r = subprocess.run(
        ["gcc", "-std=c99", "-Wall", "-Wextra", "-Werror",
         "-I", f"{TSS_ROOT}/c/include", str(out / "sensor_uop_harness.c"),
         "-o", str(exe), f"-L{TSS_ROOT}/build", "-lTSS",
         "-Wl,-rpath," + f"{TSS_ROOT}/build"],
        capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, f"compile failed:\n{r.stderr}"
