"""Tests for the C type-codec emitter.

Beyond string checks, the emitted header is compiled with
``-std=c99 -Wall -Wextra -Werror`` and a test program verifies exact
little-endian wire bytes plus decode round-trip.
"""

import subprocess
import textwrap
from pathlib import Path

import pytest

from scaffold.gen_uop import generate_tree
from scaffold.model import load_descriptor
from scaffold.types_emit import emit_types_header

_HERE = Path(__file__).resolve().parent
SENSOR = str(_HERE.parent / "examples" / "sensor" / "sensor_uop.yaml")


def _find_tss_root():
    d = _HERE
    for _ in range(8):
        if (d / "c" / "include" / "face_tss" / "tss.h").exists():
            return d
        d = d.parent
    raise RuntimeError("could not locate the tss repo root")


TSS_ROOT = _find_tss_root()


@pytest.fixture(scope="module")
def header(tmp_path_factory):
    model = load_descriptor(SENSOR)
    text = emit_types_header(model)
    assert "SENSOR_UOP_TYPES_H" in text
    assert "#define RAW_DETECTION_WIRE_SIZE 8" in text
    # FusedTrack is IDL-defined now: no fixed wire size, the generated
    # FlatBuffers codec header is included instead.
    assert '#include "fusedtrack_typed.h"' in text
    assert "FUSED_TRACK_WIRE_SIZE" not in text
    assert "typedef struct" in text
    d = tmp_path_factory.mktemp("gen")
    # The types header includes the IDL codec header; write every
    # generated file so the include chain resolves. face_tss/typed.h
    # comes from the TSS include dir (added to the compile below).
    files, _ = generate_tree(model, {})
    for name, content in files.items():
        (d / name).write_text(content)
    return d, d / "sensor_uop_types.h"


C_PROG = textwrap.dedent("""\
    #include <stdio.h>
    #include <string.h>
    #include "sensor_uop_types.h"

    int main(void) {
        /* scalar codec: boundary values round-trip */
        raw_detection_t det = { 10, -20 };
        uint8_t w[RAW_DETECTION_WIRE_SIZE];
        raw_detection_encode(&det, w);
        for (int i = 0; i < RAW_DETECTION_WIRE_SIZE; i++)
            printf("%02x", w[i]);
        printf("\\n");

        /* decode round-trip, including a short buffer rejection */
        raw_detection_t back;
        if (raw_detection_decode(w, sizeof(w), &back) != 0) return 2;
        if (back.x != 10 || back.y != -20) return 3;
        if (raw_detection_decode(w, sizeof(w) - 1, &back) != -1) return 4;
        {   /* over-long payloads are rejected too */
            uint8_t w2[RAW_DETECTION_WIRE_SIZE + 1];
            memcpy(w2, w, sizeof(w));
            if (raw_detection_decode(w2, sizeof(w2), &back) != -1) return 5;
        }

        /* IDL codec: the FusedTrack type is visible through the include
           chain (sizeof needs the complete type, no link needed) */
        printf("%zu\\n", sizeof(FusedTrack));

        printf("OK\\n");
        return 0;
    }
    """)


def _build_and_run(tmp_path, header_dir, prog_src, prog_name="tprog"):
    src = tmp_path / f"{prog_name}.c"
    src.write_text(prog_src)
    exe = tmp_path / prog_name
    r = subprocess.run(
        ["gcc", "-std=c99", "-Wall", "-Wextra", "-Werror",
         "-I", str(header_dir),
         "-I", str(TSS_ROOT / "c" / "include"),
         str(src), "-o", str(exe)],
        capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, f"compile failed:\n{r.stderr}"
    r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
    return r


def test_wire_bytes_and_round_trip(tmp_path, header):
    header_dir, _ = header
    r = _build_and_run(tmp_path, header_dir, C_PROG)
    assert r.returncode == 0, f"test program failed: rc={r.returncode}"
    lines = r.stdout.splitlines()
    # raw_detection{10, -20} LE: 0a000000 ecffffff
    assert lines[0] == "0a000000ecffffff"
    # FusedTrack struct is visible (complete type through the include)
    assert int(lines[1]) > 0
    assert lines[2] == "OK"


ALL_SCALARS_DESC = """\
uop:
  name: wide
  language: c99
  profile: general_purpose
  types:
    - name: all
      fields:
        - name: a
          type: int8
        - name: b
          type: uint8
        - name: c
          type: int16
        - name: d
          type: uint16
        - name: e
          type: int32
        - name: f
          type: uint32
        - name: g
          type: int64
        - name: h
          type: uint64
        - name: i
          type: bool
        - name: j
          type: float
        - name: k
          type: double
  connections:
    - name: CH
      direction: source
      transport: pubsub
      role: publisher
      type: all
"""


def test_all_scalars_round_trip(tmp_path):
    desc = tmp_path / "wide.yaml"
    desc.write_text(ALL_SCALARS_DESC)
    model = load_descriptor(desc)
    assert model.type_by_name("all").wire_size == 1 + 1 + 2 + 2 + 4 + 4 + 8 + 8 + 1 + 4 + 8
    hdr = tmp_path / "wide_types.h"
    hdr.write_text(emit_types_header(model))
    prog = textwrap.dedent("""\
        #include <stdio.h>
        #include <string.h>
        #include "wide_types.h"
        int main(void) {
            all_t m;
            memset(&m, 0, sizeof m);
            m.a = -128; m.b = 255; m.c = -32768; m.d = 65535;
            m.e = -2147483647 - 1; m.f = 4294967295u;
            m.g = -9223372036854775807LL - 1; m.h = 18446744073709551615ULL;
            m.i = true; m.j = 1.5f; m.k = -2.25;
            uint8_t w[ALL_WIRE_SIZE];
            all_encode(&m, w);
            all_t b;
            memset(&b, 0xA5, sizeof b);
            if (all_decode(w, sizeof w, &b) != 0) return 1;
            if (b.a != m.a || b.b != m.b || b.c != m.c || b.d != m.d ||
                b.e != m.e || b.f != m.f || b.g != m.g || b.h != m.h ||
                b.i != m.i || b.j != m.j || b.k != m.k) return 2;
            printf("OK\\n");
            return 0;
        }
        """)
    src = tmp_path / "tall.c"
    src.write_text(prog)
    exe = tmp_path / "tall"
    r = subprocess.run(
        ["gcc", "-std=c99", "-Wall", "-Wextra", "-Werror",
         "-I", str(tmp_path), str(src), "-o", str(exe)],
        capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, f"compile failed:\n{r.stderr}"
    r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
    assert r.returncode == 0 and r.stdout.strip() == "OK"
