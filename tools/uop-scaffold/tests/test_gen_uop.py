"""Tests for the UoP source-tree generator (Phase 2)."""

import subprocess
import textwrap
from pathlib import Path

import pytest

from scaffold.gen_uop import (
    RegionError,
    extract_regions,
    default_regions,
    emit_uop_c,
    emit_cmakelists,
    generate_tree,
    write_tree,
    main as gen_main,
)
from scaffold.model import load_descriptor

_HERE = Path(__file__).resolve().parent
SENSOR = str(_HERE.parent / "examples" / "sensor" / "sensor_uop.yaml")


def _find_tss_root():
    d = _HERE
    for _ in range(8):
        if (d / "c" / "include" / "face_tss" / "tss.h").exists():
            return str(d)
        d = d.parent
    raise RuntimeError("could not locate the tss repo root")


TSS_ROOT = _find_tss_root()


@pytest.fixture(scope="module")
def model():
    return load_descriptor(SENSOR)


def test_extract_regions_round_trip():
    src = ("int x;\n"
           "/* USER CODE BEGIN: thing */\n"
           "hello();\n"
           "/* USER CODE END: thing */\n"
           "int y;\n")
    regions = extract_regions(src)
    assert regions == {"thing": "hello();\n"}


def test_extract_regions_rejects_duplicates():
    src = ("/* USER CODE BEGIN: a */\n1;\n/* USER CODE END: a */\n"
           "/* USER CODE BEGIN: a */\n2;\n/* USER CODE END: a */\n")
    with pytest.raises(RegionError):
        extract_regions(src)


def test_extract_regions_rejects_unclosed():
    with pytest.raises(RegionError):
        extract_regions("/* USER CODE BEGIN: a */\n1;\n")


def test_generated_c_shape(model):
    src, orphans = emit_uop_c(model, {})
    assert orphans == []
    # lifecycle skeleton
    assert "face_tss_config_init" in src
    assert "face_tss_initialize" in src
    assert "face_tss_create_connection" in src
    assert "face_tss_register_callback" in src
    assert "face_tss_destroy" in src
    # descriptor-driven content
    assert '"RAW_DETECTION"' in src
    assert '"FUSED_TRACK"' in src
    assert "FACE_TSS_DESTINATION" in src
    assert "FACE_TSS_SOURCE" in src
    assert "static void on_raw_detection" in src
    assert "int publish_FUSED_TRACK" in src
    assert "raw_detection_decode(payload, payload_len, &msg)" in src
    assert "FusedTrack_serialize(msg, &payload, &payload_len)" in src
    # USER CODE regions with defaults
    assert "/* USER CODE BEGIN: ctx_fields */" in src
    assert "/* USER CODE BEGIN: on_raw_detection */" in src
    # teardown is reverse order of creation
    create = src.index('face_tss_create_connection(ctx.tss, "RAW_DETECTION"')
    create2 = src.index('face_tss_create_connection(ctx.tss, "FUSED_TRACK"')
    dest_pub = src.index("face_tss_destroy_connection(ctx.tss, ctx.fused_track_id);")
    dest_sub = src.index("face_tss_destroy_connection(ctx.tss, ctx.raw_detection_id);")
    assert create < create2 < dest_pub < dest_sub


def test_regeneration_preserves_user_code(model):
    src1, _ = emit_uop_c(model, {})
    regions = extract_regions(src1)
    regions["ctx_fields"] = "    int32_t next_track_id;\n"
    regions["on_raw_detection"] = ("    fused_track_t fused;\n"
                                   "    fused.x = msg.x;\n"
                                   "    publish_FUSED_TRACK(ctx, &fused);\n")
    src2, orphans = emit_uop_c(model, regions)
    assert orphans == []
    assert "int32_t next_track_id;" in src2
    assert "publish_FUSED_TRACK(ctx, &fused);" in src2
    assert "TODO" not in src2
    # byte-stable: extract + regenerate again changes nothing
    src3, _ = emit_uop_c(model, extract_regions(src2))
    assert src3 == src2


def test_startup_region_emitted(model):
    src, orphans = emit_uop_c(model, {})
    assert orphans == []
    assert "/* USER CODE BEGIN: startup */" in src
    # one-shot region runs after the "running" banner, before the main loop
    banner = src.index(": running (")
    region = src.index("/* USER CODE BEGIN: startup */")
    loop = src.index("while (!g_stop) {")
    assert banner < region < loop


def test_startup_region_preserved(model):
    src1, _ = emit_uop_c(model, {})
    regions = extract_regions(src1)
    regions["startup"] = ("    fused_track_t fused;\n"
                          "    publish_FUSED_TRACK(&ctx, &fused);\n")
    src2, orphans = emit_uop_c(model, regions)
    assert orphans == []
    assert "publish_FUSED_TRACK(&ctx, &fused);" in src2
    # byte-stable across a second regeneration
    src3, _ = emit_uop_c(model, extract_regions(src2))
    assert src3 == src2


def test_orphan_regions_reported(model):
    src, orphans = emit_uop_c(model, {"gone_region": "x();\n"})
    assert orphans == ["gone_region"]
    assert "gone_region" not in src


def test_cmakelists(model):
    cmake = emit_cmakelists(model)
    assert "project(sensor_uop C)" in cmake
    assert "add_executable(sensor_uop sensor_uop.c fusedtrack_typed.c)" in cmake
    assert "-Werror" in cmake


def test_cli_writes_tree(tmp_path, model):
    out = tmp_path / "gen"
    assert gen_main([SENSOR, "--out", str(out)]) == 0
    assert (out / "sensor_uop.c").exists()
    assert (out / "sensor_uop_types.h").exists()
    assert (out / "CMakeLists.txt").exists()
    # second run regenerates over the first without complaint
    assert gen_main([SENSOR, "--out", str(out)]) == 0


def test_cli_rejects_bad_descriptor(tmp_path):
    bad = tmp_path / "bad.yaml"
    bad.write_text("uop:\n  name: x\n")
    assert gen_main([str(bad), "--out", str(tmp_path / "o")]) == 1


def _compile(tmp_path, src_file, extra=None):
    exe = tmp_path / "uop_bin"
    gen_dir = src_file.parent
    # IDL types produce a FlatBuffers codec per type; link them in.
    typed_srcs = sorted(str(p) for p in gen_dir.glob("*_typed.c"))
    flatcc_inc = f"{TSS_ROOT}/build/_deps/flatcc-src/include"
    flatcc_lib = f"{TSS_ROOT}/build/_deps/flatcc-src/lib"
    cmd = ["gcc", "-std=c99", "-Wall", "-Wextra", "-Werror",
           "-I", f"{TSS_ROOT}/c/include", "-I", flatcc_inc,
           str(src_file)] + typed_srcs + ["-o", str(exe),
           f"-L{TSS_ROOT}/build", "-lTSS",
           f"-L{flatcc_lib}", "-lflatccrt",
           "-Wl,-rpath," + f"{TSS_ROOT}/build",
           "-Wl,-rpath," + flatcc_lib]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, f"compile failed:\n{r.stderr}"
    return exe


def test_generated_tree_compiles_clean(tmp_path, model):
    """Fresh skeleton (default no-op USER CODE) builds under -Werror."""
    out = tmp_path / "gen"
    files, _ = generate_tree(model, {})
    write_tree(out, files)
    _compile(tmp_path, out / "sensor_uop.c")


def test_regions_survive_crlf(model):
    """Region markers are found in CRLF-edited files (no silent data loss)."""
    files, _ = generate_tree(model, {}, {})
    crlf = files["sensor_uop.c"].replace("\n", "\r\n")
    regions = extract_regions(crlf)
    assert "on_raw_detection" in regions
    regions["on_raw_detection"] = "MARKER_BODY();\r\n"
    files2, orphans = generate_tree(model, regions, {})
    assert orphans == []
    assert "MARKER_BODY();" in files2["sensor_uop.c"]
