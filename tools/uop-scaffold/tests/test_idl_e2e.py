"""End-to-end: IDL -> .fbs -> C codec -> -Werror build -> round-trip run.

This is the test that would have caught the sensor loopback crash class of
bugs: it exercises heap-ownership (nested struct, string, vector) through
serialize/deserialize/fini in a real compiled binary.
"""

import subprocess
from pathlib import Path

import pytest

from scaffold.gen_uop import generate_tree, write_tree
from scaffold.model import load_descriptor

_HERE = Path(__file__).resolve().parent


def _find_tss_root():
    d = _HERE
    for _ in range(8):
        if (d / "c" / "include" / "face_tss" / "tss.h").exists():
            return d
        d = d.parent
    raise RuntimeError("could not locate the tss repo root")


TSS_ROOT = _find_tss_root()
FLATCC_INC = TSS_ROOT / "build" / "_deps" / "flatcc-src" / "include"
FLATCC_LIB = TSS_ROOT / "build" / "_deps" / "flatcc-src" / "lib"

IDL_TEXT = """\
module M {
    enum Kind { Alpha, Beta };
    struct Inner { long x; string s; };
    struct Msg {
        long id;
        Inner inner;
        Kind k;
        sequence<double> vals;
        string name;
    };
};
"""

YAML_TEXT = """\
uop:
  name: idl_e2e
  language: c99
  profile: general_purpose

  types:
    - name: Msg
      idl: types.idl
      idl_type: M::Msg

  connections:
    - name: CH
      type: Msg
      direction: source
      transport: pubsub
      role: publisher
"""

C_MAIN = r"""\
#define _POSIX_C_SOURCE 200809L
#include "msg_typed.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    /* Build a message exercising every ownership class: nested struct,
       string, vector, enum, scalar. Heap-allocate everything the codec
       owns (Msg_fini frees it). */
    Msg m;
    memset(&m, 0, sizeof m);
    m.id = 42;
    m.inner = calloc(1, sizeof *m.inner);
    assert(m.inner != NULL);
    m.inner->x = 7;
    m.inner->s = strdup("hello");
    m.k = Kind_Beta;
    m.vals_count = 2;
    m.vals = malloc(sizeof(double) * m.vals_count);
    assert(m.vals != NULL);
    m.vals[0] = 1.5;
    m.vals[1] = 2.5;
    m.name = strdup("test-msg");

    uint8_t *payload = NULL;
    size_t len = 0;
    assert(Msg_serialize(&m, &payload, &len) ==
           FACE_TSS_RC_NO_ERROR);
    assert(payload != NULL && len > 0);
    Msg_fini(&m);  /* original is ours */

    Msg out;
    memset(&out, 0, sizeof out);
    assert(Msg_deserialize(payload, len, &out) ==
           FACE_TSS_RC_NO_ERROR);
    free(payload);

    assert(out.id == 42);
    assert(out.inner != NULL && out.inner->x == 7);
    assert(strcmp(out.inner->s, "hello") == 0);
    assert(out.k == Kind_Beta);
    assert(out.vals_count == 2);
    assert(out.vals[0] == 1.5 && out.vals[1] == 2.5);
    assert(strcmp(out.name, "test-msg") == 0);
    Msg_fini(&out);

    printf("IDL round-trip OK (%zu bytes)\n", len);
    return 0;
}
"""


@pytest.fixture(scope="module")
def gen_dir(tmp_path_factory):
    d = tmp_path_factory.mktemp("idl_e2e")
    (d / "types.idl").write_text(IDL_TEXT)
    (d / "uop.yaml").write_text(YAML_TEXT)
    model = load_descriptor(str(d / "uop.yaml"))
    files, _ = generate_tree(model, {})
    out = d / "gen"
    write_tree(out, files)
    assert (out / "msg_typed.c").exists()
    assert (out / "msg_typed.h").exists()
    assert (out / "msg.fbs").exists()
    return out


def test_codec_compiles_clean(gen_dir, tmp_path):
    """Generated codec builds under -std=c99 -Wall -Wextra -Werror."""
    main_c = tmp_path / "rt_main.c"
    main_c.write_text(C_MAIN)
    exe = tmp_path / "rt"
    cmd = ["gcc", "-std=c99", "-Wall", "-Wextra", "-Werror",
           "-I", str(gen_dir),
           "-I", str(TSS_ROOT / "c" / "include"),
           "-I", str(FLATCC_INC),
           str(main_c), str(gen_dir / "msg_typed.c"),
           "-o", str(exe),
           "-L", str(FLATCC_LIB), "-lflatccrt",
           "-Wl,-rpath," + str(FLATCC_LIB)]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    assert r.returncode == 0, f"compile failed:\n{r.stderr}"
    r = subprocess.run([str(exe)], capture_output=True, text=True,
                       timeout=60)
    assert r.returncode == 0, f"round-trip failed:\n{r.stderr}"
    assert "IDL round-trip OK" in r.stdout


def test_fbs_matches_idl_shape(gen_dir):
    """The lowered schema carries the namespace and every member."""
    fbs = (gen_dir / "msg.fbs").read_text()
    assert "namespace M;" in fbs
    assert "table Msg" in fbs
    assert "inner: Inner;" in fbs
    assert "vals: [double];" in fbs
    assert "root_type Msg;" in fbs
