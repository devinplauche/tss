"""Phase 2: generate the UoP source tree from a validated descriptor.

Generates, for a UoP named ``<uop>``::

    <uop>.c          lifecycle, callbacks, publish helpers (USER CODE regions)
    <uop>_types.h    message codecs (via scaffold.types_emit)
    CMakeLists.txt   C99 -Werror build against TSS_ROOT

USER CODE regions are marked::

    /* USER CODE BEGIN: <name> */
    ... hand-written code ...
    /* USER CODE END: <name> */

On regeneration, region bodies are extracted from the existing ``<uop>.c``
and re-inserted; everything else is regenerated from the template.
Regions found in the old file but no longer emitted (e.g. a callback
removed from the descriptor) are reported as warnings, not silently kept.
"""

import argparse
import re
import sys
from pathlib import Path

from .model import load_descriptor, DescriptorError
from .types_emit import emit_types_header
from .util import member_stems, render_region
from .gen_harness import emit_harness_c, emit_cmake_harness

__all__ = [
    "RegionError",
    "extract_regions",
    "default_regions",
    "emit_uop_c",
    "emit_cmakelists",
    "generate_tree",
    "write_tree",
    "main",
]


class RegionError(Exception):
    """A USER CODE region marker is duplicated or unbalanced."""


_BEGIN_RE = re.compile(r"/\* USER CODE BEGIN: ([A-Za-z_][A-Za-z0-9_]*) \*/\r?")
_END_RE = re.compile(r"/\* USER CODE END: ([A-Za-z_][A-Za-z0-9_]*) \*/\r?")
_PAIR_RE = re.compile(
    r"^[ \t]*/\* USER CODE BEGIN: ([A-Za-z_][A-Za-z0-9_]*) \*/\r?\n"
    r"(.*?)"
    r"^[ \t]*/\* USER CODE END: \1 \*/",
    re.DOTALL | re.MULTILINE,
)

_DIRECTION_C = {
    "source": "FACE_TSS_SOURCE",
    "destination": "FACE_TSS_DESTINATION",
}
_ROLE_C = {
    "publisher": "FACE_TSS_ROLE_PUBLISHER",
    "subscriber": "FACE_TSS_ROLE_SUBSCRIBER",
}
_TRANSPORT_C = {
    "pubsub": "FACE_TSS_TRANSPORT_PUBSUB",
}


def extract_regions(text):
    """Extract ``{name: body}`` from USER CODE regions in existing source."""
    regions = {}
    for m in _PAIR_RE.finditer(text):
        name = m.group(1)
        if name in regions:
            raise RegionError(f"duplicate USER CODE region '{name}'")
        regions[name] = m.group(2)
    rest = _PAIR_RE.sub("", text)
    m = _BEGIN_RE.search(rest) or _END_RE.search(rest)
    if m:
        raise RegionError(f"unbalanced USER CODE marker for '{m.group(1)}'")
    return regions


_members = member_stems  # backward-compat alias
_region = render_region  # backward-compat alias


def default_regions(model):
    """Default region bodies for a fresh generation."""
    pubs = [c.name for c in model.connections if c.role == "publisher"]
    regions = {
        "ctx_fields":
            "/* Add your UoP state fields here. They survive regeneration. */\n",
        "startup":
            "/* One-shot logic after connections are open (e.g. publish\n"
            "   stimulus, read a config file). Runs once before the main\n"
            "   loop. `ctx` is the context struct (not a pointer) here;\n"
            "   `exit_code = 1; goto cleanup;` aborts startup on error. */\n",
        "shutdown":
            "/* One-shot logic after the main loop exits, before teardown\n"
            "   (e.g. offload state: publish a final save message to a\n"
            "   storage UoP). Connections are still open here; `ctx` is\n"
            "   the context struct (not a pointer). */\n",
    }
    for c in model.connections:
        if c.role != "subscriber":
            continue
        if pubs:
            todo = ("/* TODO: handle msg; publish results with\n"
                    + "".join(f" *   {p}(ctx, &out).\n" for p in
                              [f"publish_{p}" for p in pubs])
                    + " */\n(void)msg;\n")
        else:
            todo = "/* TODO: handle msg. */\n(void)msg;\n"
        regions[c.callback] = todo
    return regions


def _callback_sig():
    return ("(FACE_TSS_CONNECTION_ID_TYPE connection_id,\n"
            "                             FACE_TSS_TRANSACTION_ID_TYPE transaction_id,\n"
            "                             FACE_TSS_MESSAGE_GUID_TYPE message_guid,\n"
            "                             const uint8_t *payload, size_t payload_len,\n"
            "                             const FACE_TSS_HEADER *header,\n"
            "                             const FACE_TSS_QOS_EVENT *qos, void *user,\n"
            "                             FACE_TSS_RETURN_CODE *return_code)")


def _emit_publish_scalar(L, ctx_t, c, t, stem):
    """Publish helper for scalar (fixed-size, infallible-encode) types."""
    L.append(f"int publish_{c.name}({ctx_t} *ctx, const {t.name}_t *msg)")
    L.append("{")
    L.append(f"    uint8_t wire[{t.name.upper()}_WIRE_SIZE];")
    L.append("    FACE_TSS_TRANSACTION_ID_TYPE txn ="
             " FACE_TSS_TRANSACTION_ID_UNSPECIFIED;")
    L.append("")
    L.append(f"    {t.name}_encode(msg, wire);")
    L.append(f"    if (face_tss_send_message(ctx->tss, ctx->{stem}_id,")
    L.append("                              SEND_TIMEOUT_NS, &txn, wire,")
    L.append("                              sizeof(wire))")
    L.append("        != FACE_TSS_RC_NO_ERROR) {")
    L.append("        ctx->errors++;")
    L.append("        return -1;")
    L.append("    }")
    L.append("    ctx->published++;")
    L.append("    return 0;")
    L.append("}")
    L.append("")


def _emit_publish_idl(L, ctx_t, c, t, stem):
    """Publish helper for IDL-defined types.

    The codec serializes to a caller-owned heap buffer (FlatBuffers wire
    format); the helper sends it and frees it. Serialization can fail
    (allocation), unlike the scalar path.
    """
    L.append(f"int publish_{c.name}({ctx_t} *ctx, const {t.name} *msg)")
    L.append("{")
    L.append("    uint8_t *payload = NULL;")
    L.append("    size_t payload_len = 0;")
    L.append("    FACE_TSS_TRANSACTION_ID_TYPE txn ="
             " FACE_TSS_TRANSACTION_ID_UNSPECIFIED;")
    L.append("    FACE_TSS_RETURN_CODE rc;")
    L.append("")
    L.append(f"    rc = {t.name}_serialize(msg, &payload, &payload_len);")
    L.append("    if (rc != FACE_TSS_RC_NO_ERROR) {")
    L.append("        ctx->errors++;")
    L.append("        return -1;")
    L.append("    }")
    L.append(f"    rc = face_tss_send_message(ctx->tss, ctx->{stem}_id,")
    L.append("                               SEND_TIMEOUT_NS, &txn, payload,")
    L.append("                               payload_len);")
    L.append("    free(payload);")
    L.append("    if (rc != FACE_TSS_RC_NO_ERROR) {")
    L.append("        ctx->errors++;")
    L.append("        return -1;")
    L.append("    }")
    L.append("    ctx->published++;")
    L.append("    return 0;")
    L.append("}")
    L.append("")


def _emit_callback_scalar(L, ctx_t, c, t, region):
    L.append(f"static void {c.callback}{_callback_sig()}")
    L.append("{")
    L.append(f"    {ctx_t} *ctx = ({ctx_t} *)user;")
    L.append(f"    {t.name}_t msg;")
    L.append("")
    L.append("    (void)connection_id;")
    L.append("    (void)transaction_id;")
    L.append("    (void)message_guid;")
    L.append("    (void)header;")
    L.append("    (void)qos;")
    L.append("")
    L.append("    *return_code = FACE_TSS_RC_NO_ERROR;")
    L.append(f"    if ({t.name}_decode(payload, payload_len, &msg) != 0) {{")
    L.append("        ctx->errors++;")
    L.append("        return;")
    L.append("    }")
    L.append("    ctx->received++;")
    L.append("")
    L.append(region(c.callback))
    L.append("}")
    L.append("")


def _emit_callback_idl(L, ctx_t, c, t, region):
    """Subscription callback for IDL-defined types.

    Deserializes into a heap-owning struct, runs USER CODE, then releases
    with ``<T>_fini``. The fini call is generated after the region, so
    USER CODE must not ``return`` early from the region.
    """
    L.append(f"static void {c.callback}{_callback_sig()}")
    L.append("{")
    L.append(f"    {ctx_t} *ctx = ({ctx_t} *)user;")
    L.append(f"    {t.name} msg;")
    L.append("")
    L.append("    (void)connection_id;")
    L.append("    (void)transaction_id;")
    L.append("    (void)message_guid;")
    L.append("    (void)header;")
    L.append("    (void)qos;")
    L.append("")
    L.append("    *return_code = FACE_TSS_RC_NO_ERROR;")
    L.append(f"    if ({t.name}_deserialize(payload, payload_len, &msg)")
    L.append("        != FACE_TSS_RC_NO_ERROR) {")
    L.append("        ctx->errors++;")
    L.append("        return;")
    L.append("    }")
    L.append("    ctx->received++;")
    L.append("")
    L.append(f"    /* NOTE: {t.name}_fini(&msg) runs after your code below;")
    L.append("       do not 'return' early from the USER CODE region. */")
    L.append(region(c.callback))
    L.append(f"    {t.name}_fini(&msg);")
    L.append("}")
    L.append("")


def emit_uop_c(model, regions):
    """Render ``<uop>.c``. ``regions`` maps region name -> body text."""
    uop = model.name
    ctx_t = f"{uop}_ctx_t"
    members = member_stems(model)
    defaults = default_regions(model)
    used = set()

    def region(name, indent=""):
        used.add(name)
        return _region(name, regions.get(name, defaults[name]), indent)

    subs = [c for c in model.connections if c.role == "subscriber"]
    pubs = [c for c in model.connections if c.role == "publisher"]

    L = []
    # File header ------------------------------------------------------
    L.append("/*")
    L.append(f" * {uop}.c -- GENERATED by uop-scaffold. DO NOT EDIT by hand.")
    L.append(" *")
    L.append(f" * Regenerate with: python3 -m scaffold.gen_uop <descriptor> --out <dir>")
    L.append(" * Hand-written logic lives in USER CODE regions and survives")
    L.append(" * regeneration.")
    L.append(" *")
    L.append(f" * Usage: {uop} <base_port>")
    for i, c in enumerate(model.connections):
        L.append(f" *   {c.name} on tcp://127.0.0.1:<base_port+{i}>"
                 f"  ({c.role})")
    L.append(" *")
    L.append(" * Terminates gracefully on SIGTERM/SIGINT.")
    L.append(" */")
    L.append("#define _POSIX_C_SOURCE 200809L /* signal(), sleep() */")
    L.append("#include <signal.h>")
    L.append("#include <stdint.h>")
    L.append("#include <stdio.h>")
    L.append("#include <stdlib.h> /* free() for IDL-typed payloads */")
    L.append("#include <string.h>")
    L.append("#include <unistd.h>")
    L.append("")
    L.append("#include <face_tss/tss.h>")
    L.append("")
    L.append(f'#include "{uop}_types.h"')
    L.append("")
    L.append("#define MAX_MESSAGE_SIZE 65536")
    L.append("#define QUEUE_DEPTH 64")
    L.append("#define SEND_TIMEOUT_NS 1000000000LL /* 1 s */")
    L.append("")

    # Context ----------------------------------------------------------
    L.append("/* ------------------------------------------------------------------ */")
    L.append("/* UoP context                                                         */")
    L.append("/* ------------------------------------------------------------------ */")
    L.append("typedef struct {")
    L.append("    FACE_TSS *tss;")
    for c in model.connections:
        L.append(f"    FACE_TSS_CONNECTION_ID_TYPE {members[c.name]}_id;"
                 f" /* {c.name} */")
    L.append(region("ctx_fields", "    "))
    L.append("    uint64_t received;")
    L.append("    uint64_t published;")
    L.append("    uint64_t errors;")
    L.append(f"}} {ctx_t};")
    L.append("")

    # Publish helpers (non-static: no -Wunused-function before user code
    # calls them) ------------------------------------------------------
    if pubs:
        L.append("/* ------------------------------------------------------------------ */")
        L.append("/* Publish helpers (non-static so -Wunused-function stays quiet      */")
        L.append("/* until USER CODE calls them)                                         */")
        L.append("/* ------------------------------------------------------------------ */")
        for c in pubs:
            t = model.type_by_name(c.type)
            stem = members[c.name]
            if t.is_idl:
                _emit_publish_idl(L, ctx_t, c, t, stem)
            else:
                _emit_publish_scalar(L, ctx_t, c, t, stem)

    # Subscription callbacks -------------------------------------------
    if subs:
        L.append("/* ------------------------------------------------------------------ */")
        L.append("/* Subscription callbacks                                              */")
        L.append("/* ------------------------------------------------------------------ */")
        for c in subs:
            t = model.type_by_name(c.type)
            if t.is_idl:
                _emit_callback_idl(L, ctx_t, c, t, region)
            else:
                _emit_callback_scalar(L, ctx_t, c, t, region)

    # Lifecycle --------------------------------------------------------
    L.append("/* ------------------------------------------------------------------ */")
    L.append("/* Lifecycle                                                           */")
    L.append("/* ------------------------------------------------------------------ */")
    L.append("static volatile sig_atomic_t g_stop = 0;")
    L.append("")
    L.append("static void on_signal(int sig)")
    L.append("{")
    L.append("    (void)sig;")
    L.append("    g_stop = 1;")
    L.append("}")
    L.append("")
    L.append("static FACE_TSS_RETURN_CODE add_connection(FACE_TSS_CONFIG *cfg,")
    L.append("                                           const char *name,")
    L.append("                                           const char *address,")
    L.append("                                           FACE_TSS_DIRECTION direction,")
    L.append("                                           FACE_TSS_TRANSPORT_KIND transport,")
    L.append("                                           FACE_TSS_ROLE role)")
    L.append("{")
    L.append("    FACE_TSS_CONNECTION_CONFIG c;")
    L.append("")
    L.append("    memset(&c, 0, sizeof(c));")
    L.append("    strncpy(c.name, name, sizeof(c.name) - 1);")
    L.append("    strncpy(c.address, address, sizeof(c.address) - 1);")
    L.append("    c.direction = direction;")
    L.append("    c.transport = transport;")
    L.append("    c.role = role;")
    L.append("    c.max_message_size = MAX_MESSAGE_SIZE;")
    L.append("    c.queue_depth = QUEUE_DEPTH;")
    L.append("    return face_tss_config_add(cfg, &c);")
    L.append("}")
    L.append("")
    L.append("#define CHECK_RC(call)                                                  \\")
    L.append("    do {                                                                \\")
    L.append("        FACE_TSS_RETURN_CODE rc_ = (call);                               \\")
    L.append("        if (rc_ != FACE_TSS_RC_NO_ERROR) {                               \\")
    L.append(f'            fprintf(stderr, "{uop}: %s:%d: %s -> %s\\n", __FILE__,    \\')
    L.append('                    __LINE__, #call, face_tss_rc_str(rc_));              \\')
    L.append("            exit_code = 1;                                              \\")
    L.append("            goto cleanup;                                               \\")
    L.append("        }                                                               \\")
    L.append("    } while (0)")
    L.append("")
    L.append("int main(int argc, char **argv)")
    L.append("{")
    for c in model.connections:
        L.append(f"    char {members[c.name]}_addr[FACE_TSS_MAX_ADDRESS];")
    L.append("    long base_port;")
    L.append("    FACE_TSS_CONFIG cfg;")
    L.append(f"    {ctx_t} ctx;")
    L.append("    FACE_TSS_MESSAGE_SIZE_TYPE max_size;")
    L.append("    int exit_code = 0;")
    L.append("    int cfg_inited = 0;")
    L.append("    int tss_created = 0;")
    L.append("")
    L.append("    if (argc != 2) {")
    L.append('        fprintf(stderr, "usage: %s <base_port>\\n", argv[0]);')
    L.append("        return 2;")
    L.append("    }")
    L.append("    base_port = 0;")
    max_base = 65535 - (len(model.connections) - 1)
    L.append("    if (sscanf(argv[1], \"%ld\", &base_port) != 1 || base_port <= 0 ||")
    L.append(f"        base_port > {max_base}) {{")
    L.append(f'        fprintf(stderr, "{uop}: invalid base port \'%s\'\\n", argv[1]);')
    L.append("        return 2;")
    L.append("    }")
    for i, c in enumerate(model.connections):
        stem = members[c.name]
        L.append(f"    snprintf({stem}_addr, sizeof({stem}_addr),")
        L.append(f'             "tcp://127.0.0.1:%ld", base_port + {i});')
    L.append("")
    L.append("    memset(&ctx, 0, sizeof(ctx));")
    L.append("    signal(SIGTERM, on_signal);")
    L.append("    signal(SIGINT, on_signal);")
    L.append("")
    L.append("    /* 1. Describe every connection this UoP needs. */")
    L.append(f'    face_tss_config_init(&cfg, "{uop}");')
    L.append("    cfg_inited = 1;")
    for c in model.connections:
        stem = members[c.name]
        L.append(f"    CHECK_RC(add_connection(&cfg, \"{c.name}\", {stem}_addr,")
        L.append(f"                            {_DIRECTION_C[c.direction]},"
                 f" {_TRANSPORT_C[c.transport]}, {_ROLE_C[c.role]}));")
    L.append("")
    L.append("    /* 2. Create + initialize the TSS instance. */")
    L.append(f'    ctx.tss = face_tss_create("{uop}");')
    L.append("    if (!ctx.tss) {")
    L.append(f'        fprintf(stderr, "{uop}: face_tss_create failed\\n");')
    L.append("        exit_code = 1;")
    L.append("        goto cleanup;")
    L.append("    }")
    L.append("    tss_created = 1;")
    L.append("    CHECK_RC(face_tss_initialize(ctx.tss, &cfg));")
    L.append("")
    L.append("    /* 3. Open the connections. */")
    for c in model.connections:
        stem = members[c.name]
        L.append(f"    CHECK_RC(face_tss_create_connection(ctx.tss, \"{c.name}\","
                 f" &ctx.{stem}_id,")
        L.append("                                        &max_size, 0));")
    L.append("")
    if subs:
        L.append("    /* 4. Register subscription callbacks; then run until signalled. */")
        for c in subs:
            stem = members[c.name]
            L.append(f"    CHECK_RC(face_tss_register_callback(ctx.tss, ctx.{stem}_id,"
                     f" {c.callback},")
            L.append("                                        &ctx));")
    else:
        L.append("    /* 4. No subscriptions; run until signalled. */")
    sub_names = ", ".join(f"sub {c.name}" for c in subs)
    pub_names = ", ".join(f"pub {c.name}" for c in pubs)
    running_what = ", ".join(x for x in (sub_names, pub_names) if x)
    L.append(f'    printf("{uop}: running ({running_what})\\n");')
    L.append("    fflush(stdout);")
    L.append("    /* One-shot USER CODE: connections are open, callbacks are")
    L.append("       registered, the main loop has not started yet. */")
    L.append(region("startup", "    "))
    L.append("    while (!g_stop) {")
    L.append("        sleep(1);")
    L.append("    }")
    L.append(f'    printf("{uop}: shutting down (rx=%llu tx=%llu err=%llu)\\n",')
    L.append("           (unsigned long long)ctx.received,")
    L.append("           (unsigned long long)ctx.published,")
    L.append("           (unsigned long long)ctx.errors);")
    L.append("    /* One-shot USER CODE: the main loop has exited but the")
    L.append("       connections are still open, so state can still be")
    L.append("       published/offloaded before teardown. */")
    L.append(region("shutdown", "    "))
    L.append("")
    L.append("cleanup:")
    L.append("    if (tss_created) {")
    for c in reversed(subs):
        stem = members[c.name]
        L.append(f"        face_tss_unregister_callback(ctx.tss, ctx.{stem}_id);")
    for c in reversed(model.connections):
        stem = members[c.name]
        L.append(f"        face_tss_destroy_connection(ctx.tss, ctx.{stem}_id);")
    L.append("        face_tss_destroy(ctx.tss);")
    L.append("    }")
    L.append("    if (cfg_inited) {")
    L.append("        face_tss_config_fini(&cfg);")
    L.append("    }")
    L.append("    return exit_code;")
    L.append("}")
    L.append("")

    orphans = sorted(set(regions) - used - set(defaults))
    return "\n".join(L), orphans


def emit_cmakelists(model):
    """Render CMakeLists.txt for the generated tree.

    IDL-defined types add their generated ``<stem>_typed.c`` codec plus
    the flatcc runtime headers/library (vendored under the tss build
    tree); scalar-only projects need nothing beyond TSS.
    """
    uop = model.name
    idl_types = [t for t in model.types if t.is_idl]
    sources = " ".join([f"{uop}.c"] + [f"{t.typed_stem}.c" for t in idl_types])
    flatcc = ""
    if idl_types:
        flatcc = (
            "\n# IDL-defined message types use the flatcc runtime.\n"
            f'target_include_directories({uop} PRIVATE\n'
            '  "${TSS_ROOT}/build/_deps/flatcc-src/include")\n'
            f'target_link_directories({uop} PRIVATE\n'
            '  "${TSS_ROOT}/build/_deps/flatcc-src/lib")\n'
            f'target_link_libraries({uop} PRIVATE flatccrt)\n'
        )
    return f"""\
# Generated by uop-scaffold. Regenerate; do not edit by hand.
#
# Build:
#   cmake -S . -B build -DTSS_ROOT=/path/to/tss
#   cmake --build build
cmake_minimum_required(VERSION 3.16)
project({uop} C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror)

set(TSS_ROOT "" CACHE PATH "Root of the tss repo (headers in c/include, lib in build)")
if(TSS_ROOT STREQUAL "")
  message(FATAL_ERROR "Set -DTSS_ROOT=/path/to/tss")
endif()

# Locate libTSS.so plus its private shared deps (nng, flatccrt) so the
# binary runs without LD_LIBRARY_PATH.
set(CMAKE_BUILD_RPATH
    "${{TSS_ROOT}}/build"
    "${{TSS_ROOT}}/build/_deps/nng-build"
    "${{TSS_ROOT}}/build/_deps/flatcc-src/lib")

add_executable({uop} {sources})
target_include_directories({uop} PRIVATE "${{TSS_ROOT}}/c/include")
target_link_directories({uop} PRIVATE "${{TSS_ROOT}}/build")
target_link_libraries({uop} PRIVATE TSS)
{flatcc}"""


def generate_tree(model, regions=None, harness_regions=None):
    """Generate the full tree. Returns (files, orphans).

    ``files`` maps filename -> content. ``regions`` (from a previous
    ``<uop>.c`` via :func:`extract_regions`) and ``harness_regions`` (from
    ``<uop>_harness.c``) are preserved; anything not consumed becomes an
    orphan warning.
    """
    from .idl_emit import idl_type_files

    regions = dict(regions or {})
    harness_regions = dict(harness_regions or {})
    uop_c, orphans = emit_uop_c(model, regions)
    harness_c, h_orphans = emit_harness_c(model, harness_regions)
    files = {
        f"{model.name}.c": uop_c,
        f"{model.name}_harness.c": harness_c,
        f"{model.name}_types.h": emit_types_header(model),
        "CMakeLists.txt": emit_cmakelists(model) + "\n" + emit_cmake_harness(model),
    }
    for t in model.types:
        if t.is_idl:
            for fname, content in idl_type_files(t).items():
                if fname in files:  # pragma: no cover - defensive
                    raise RegionError(f"generated file name clash: {fname}")
                files[fname] = content
    return files, orphans + h_orphans


def write_tree(out_dir, files):
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    for name, content in files.items():
        (out / name).write_text(content)


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Generate a UoP source tree from a descriptor.")
    ap.add_argument("descriptor", help="component descriptor YAML")
    ap.add_argument("--out", required=True, help="output directory")
    args = ap.parse_args(argv)

    try:
        model = load_descriptor(args.descriptor)
    except DescriptorError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    out = Path(args.out)
    regions = {}
    harness_regions = {}
    for fname, store in ((f"{model.name}.c", regions),
                         (f"{model.name}_harness.c", harness_regions)):
        existing = out / fname
        if existing.exists():
            try:
                store.update(extract_regions(existing.read_text()))
            except RegionError as e:
                print(f"error: {existing}: {e}", file=sys.stderr)
                return 1

    try:
        files, orphans = generate_tree(model, regions, harness_regions)
    except (ValueError, DescriptorError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    write_tree(out, files)
    for name in files:
        print(f"wrote {out / name}")
    for o in orphans:
        print(f"warning: dropped USER CODE region '{o}' "
              f"(no longer emitted by the template)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
