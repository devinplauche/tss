#!/usr/bin/env python3
"""face_tss_codegen.py - generate FACE TSS typed C wrappers from .fbs schemas.

Reads a FlatBuffers schema and generates a C header + source implementing
FACE_TSS_TYPE_SUPPORT for its root table:

    python3 tools/face_tss_codegen.py c/schemas/telemetry.fbs \\
        --out-dir c/generated

The generated codec uses the flatcc *runtime* builder API directly (the same
technique as c/src/envelope.c), so no flatcc compiler step is needed at build
time. The message GUID is a deterministic FNV-1a 64-bit hash of the qualified
root type name, masked to a positive 63-bit value.

Supported schema features:
  - multiple tables per file (built via generated static helpers)
  - scalar fields: bool, byte/ubyte/int8/uint8, short/ushort/int16/uint16,
    int/uint/int32/uint32, long/ulong/int64/uint64, float, double
  - string fields (owned char *, freed by <Root>_fini)
  - nested table fields (owned struct pointer, NULL when absent)
  - vectors of scalars, e.g. [float], [ubyte] (owned pointer + _count)
  - vectors of strings, e.g. [string] (owned char ** + _count)
  - vectors of tables, e.g. [GeoPoint] (owned struct ** + _count;
    NULL elements are rejected at build time)
  - nested vectors of scalars, e.g. [[float]] (owned ctype ** + per-row
    counts + _count)
  - unions of tables, e.g. union U { A, B } with a field u:U. Generated as
    a <U>_value struct { <U> type; void *value; }: the discriminator enum
    <U> { <U>_NONE = 0, <U>_<Member> = 1, ... } plus an owned pointer to
    the member struct, NULL when NONE. Discriminators are serialized as
    ubyte; a union field occupies two vtable slots (type, value).
  - enums with an explicit integral base type, e.g. enum Fix : byte {...}
    (generated as a C enum; wire format is the base scalar)
  - explicit field `id` attributes, e.g. f:int (id: 3); vtable slots follow
    the ids. All-or-none per table: if one field has an id, all must.
    A union field's value slot is id+1 and must not collide.

Unsupported (rejected with an error): unions with non-table members,
vectors of unions, deeper nesting (e.g. [[[int]]], [[string]]),
enums without an explicit base type, unknown field types.

C ownership model: strings, nested tables, vectors and union values are
heap-allocated on deserialize and released by <Root>_fini. Serialize never
takes ownership.
"""

import argparse
import re
import sys
from pathlib import Path

SCALARS = {
    # fbs type: (c type, width, kind)
    "bool": ("bool", 1, "u8"),
    "byte": ("int8_t", 1, "i8"),
    "int8": ("int8_t", 1, "i8"),
    "ubyte": ("uint8_t", 1, "u8"),
    "uint8": ("uint8_t", 1, "u8"),
    "short": ("int16_t", 2, "i16"),
    "int16": ("int16_t", 2, "i16"),
    "ushort": ("uint16_t", 2, "u16"),
    "uint16": ("uint16_t", 2, "u16"),
    "int": ("int32_t", 4, "i32"),
    "int32": ("int32_t", 4, "i32"),
    "uint": ("uint32_t", 4, "u32"),
    "uint32": ("uint32_t", 4, "u32"),
    "long": ("int64_t", 8, "i64"),
    "int64": ("int64_t", 8, "i64"),
    "ulong": ("uint64_t", 8, "u64"),
    "uint64": ("uint64_t", 8, "u64"),
    "float": ("float", 4, "f32"),
    "double": ("double", 8, "f64"),
}

INTEGRAL = {
    "byte", "int8", "ubyte", "uint8", "short", "int16", "ushort", "uint16",
    "int", "int32", "uint", "uint32", "long", "int64", "ulong", "uint64",
}

# kind -> (little-endian writer, writer parameter c type)
PE = {
    "i16": ("flatbuffers_int16_write_to_pe", "int16_t"),
    "u16": ("flatbuffers_uint16_write_to_pe", "uint16_t"),
    "i32": ("flatbuffers_int32_write_to_pe", "int32_t"),
    "u32": ("flatbuffers_uint32_write_to_pe", "uint32_t"),
    "i64": ("flatbuffers_int64_write_to_pe", "int64_t"),
    "u64": ("flatbuffers_uint64_write_to_pe", "uint64_t"),
    "f32": ("flatbuffers_uint32_write_to_pe", "uint32_t"),
    "f64": ("flatbuffers_uint64_write_to_pe", "uint64_t"),
}


def fnv1a64(s: str) -> int:
    h = 0xCBF29CE484222325
    for b in s.encode("utf-8"):
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


class EnumDef:
    def __init__(self, name, utype, members):
        self.name = name            # FBS name
        self.utype = utype          # FBS scalar base type
        self.members = members      # [(name, value)]


class UnionDef:
    def __init__(self, name, members):
        self.name = name            # FBS name
        self.members = members      # [table FBS names], declaration order


class FieldDef:
    def __init__(self, name, kind, ftype=None, ref=None, velem=None,
                 fid=None):
        self.name = name
        # scalar|string|enum|table|vector|vecvec|union
        self.kind = kind
        # FBS scalar name (scalar, vector/vecvec elem, enum base)
        self.ftype = ftype
        # table/enum/union FBS name (table, enum, union, vector-of-table)
        self.ref = ref
        # for vector: "scalar"|"string"|"table"
        self.velem = velem
        # vtable slot (explicit id or assigned); union value uses fid + 1
        self.fid = fid


class TableDef:
    def __init__(self, name, fields):
        self.name = name
        self.fields = fields
        self.nslots = 0             # vtable slots = max slot id + 1


def parse_fbs(path: Path):
    text = path.read_text()
    text = re.sub(r"//.*", "", text)
    bare_enum = re.search(r"enum\s+(\w+)\s*\{", text)
    if bare_enum:
        raise ValueError(
            f"{path}: enum '{bare_enum.group(1)}' needs an explicit "
            f"integral base type, e.g. 'enum {bare_enum.group(1)} : byte {{...}}'")
    ns_m = re.search(r"namespace\s+([\w.]+)\s*;", text)
    namespace = ns_m.group(1) if ns_m else ""

    unions = {}
    for m in re.finditer(r"union\s+(\w+)\s*\{([^}]*)\}", text):
        uname, body = m.group(1), m.group(2)
        members = []
        for part in body.replace(";", ",").split(","):
            part = part.strip()
            if not part:
                continue
            if not re.fullmatch(r"\w+", part):
                raise ValueError(
                    f"{path}: union '{uname}': bad member '{part}'")
            members.append(part)
        if not members:
            raise ValueError(f"{path}: union '{uname}' has no members")
        if uname in unions:
            raise ValueError(f"{path}: duplicate union '{uname}'")
        unions[uname] = UnionDef(uname, members)

    enums = {}
    for m in re.finditer(r"enum\s+(\w+)\s*:\s*(\w+)\s*\{([^}]*)\}", text):
        ename, utype, body = m.group(1), m.group(2), m.group(3)
        if utype not in INTEGRAL:
            raise ValueError(
                f"{path}: enum '{ename}' needs an explicit integral base "
                f"type, got '{utype}'")
        members = []
        next_val = 0
        for part in body.replace(";", ",").split(","):
            part = part.strip()
            if not part:
                continue
            mm = re.match(r"(\w+)\s*(?:=\s*(-?\d+))?$", part)
            if not mm:
                raise ValueError(f"{path}: bad enum member '{part}'")
            val = int(mm.group(2)) if mm.group(2) is not None else next_val
            members.append((mm.group(1), val))
            next_val = val + 1
        if not members:
            raise ValueError(f"{path}: enum '{ename}' has no members")
        enums[ename] = EnumDef(ename, utype, members)

    raw_tables = re.findall(r"table\s+(\w+)\s*\{([^}]*)\}", text)
    if not raw_tables:
        raise ValueError(f"{path}: no table found")
    table_names = {t[0] for t in raw_tables}

    for uname, udef in unions.items():
        for mem in udef.members:
            if mem not in table_names:
                raise ValueError(
                    f"{path}: union '{uname}' member '{mem}' is not a "
                    f"table (only tables are supported as union members)")

    tables = []
    for tname, tbody in raw_tables:
        fields = []
        for fm in re.finditer(
                r"(\w+)\s*:\s*([\w\[\]]+)\s*(?:=\s*[^;(]+)?\s*"
                r"(\([^)]*\))?\s*;",
                tbody):
            fname, ftype, attrs = fm.group(1), fm.group(2), fm.group(3)
            fid = None
            if attrs:
                im = re.search(r"\bid\s*:\s*(\d+)", attrs)
                if im:
                    fid = int(im.group(1))
                    if fid > 32767:
                        raise ValueError(
                            f"{path}: table '{tname}' field '{fname}': "
                            f"id {fid} exceeds the maximum 32767")
            fields.append(parse_field(path, tname, fname, ftype,
                                      enums, table_names, unions, fid))
        if not fields:
            raise ValueError(f"{path}: table '{tname}' has no fields")
        nslots = assign_field_ids(path, tname, fields)
        tdef = TableDef(tname, fields)
        tdef.nslots = nslots
        tables.append(tdef)

    root_m = re.search(r"root_type\s+(\w+)\s*;", text)
    root_name = root_m.group(1) if root_m else tables[-1].name
    by_name = {t.name: t for t in tables}
    if root_name not in by_name:
        raise ValueError(f"{path}: root_type '{root_name}' is not a table")
    qname = f"{namespace}.{root_name}" if namespace else root_name
    return namespace, root_name, qname, tables, enums, unions


def assign_field_ids(path, tname, fields):
    """Assign vtable slots. A union field occupies two slots: the
    discriminator at fid and the value at fid + 1."""
    explicit = [f for f in fields if f.fid is not None]
    if explicit and len(explicit) != len(fields):
        raise ValueError(
            f"{path}: table '{tname}': explicit field ids are all-or-none "
            f"(field '{explicit[0].name}' has one, others do not)")
    used = set()
    if not explicit:
        nxt = 0
        for f in fields:
            f.fid = nxt
            nxt += 1
            if f.kind == "union":
                nxt += 1  # value slot
    for f in fields:
        slots = (f.fid, f.fid + 1) if f.kind == "union" else (f.fid,)
        for s in slots:
            if s in used:
                raise ValueError(
                    f"{path}: table '{tname}': field id {s} is used twice "
                    f"(field '{f.name}')")
            used.add(s)
    return max(used) + 1


def parse_field(path, tname, fname, ftype, enums, table_names, unions, fid):
    if ftype == "string":
        return FieldDef(fname, "string", fid=fid)
    if ftype in SCALARS:
        return FieldDef(fname, "scalar", ftype, fid=fid)
    vm2 = re.fullmatch(r"\[\[(\w+)\]\]", ftype)
    if vm2:
        elem = vm2.group(1)
        if elem not in SCALARS:
            raise ValueError(
                f"{path}: table '{tname}' field '{fname}': nested vectors "
                f"support scalars only, got '[[{elem}]]'")
        return FieldDef(fname, "vecvec", elem, fid=fid)
    vm = re.fullmatch(r"\[(\w+)\]", ftype)
    if vm:
        elem = vm.group(1)
        if elem in SCALARS:
            return FieldDef(fname, "vector", elem, velem="scalar", fid=fid)
        if elem == "string":
            return FieldDef(fname, "vector", velem="string", fid=fid)
        if elem in table_names:
            return FieldDef(fname, "vector", None, elem, velem="table",
                            fid=fid)
        raise ValueError(
            f"{path}: table '{tname}' field '{fname}': unsupported vector "
            f"element type '[{elem}]' (scalars, strings and tables only; "
            f"vectors of unions are not supported)")
    if ftype in enums:
        return FieldDef(fname, "enum", enums[ftype].utype, ftype, fid=fid)
    if ftype in table_names:
        return FieldDef(fname, "table", None, ftype, fid=fid)
    if ftype in unions:
        return FieldDef(fname, "union", None, ftype, fid=fid)
    raise ValueError(
        f"{path}: table '{tname}' field '{fname}' has unsupported type "
        f"'{ftype}'")


# ---------------------------------------------------------------------------
# header
# ---------------------------------------------------------------------------

def gen_header(root_name, qname, tables, enums, unions, guid):
    guard = f"{root_name.upper()}_TYPED_H"
    L = []
    A = L.append
    A(f"#ifndef {guard}")
    A(f"#define {guard}")
    A("")
    A("/* Generated by tools/face_tss_codegen.py - DO NOT EDIT. */")
    A("")
    A('#include "face_tss/typed.h"')
    A("#include <stdbool.h>")
    A("#include <stddef.h>")
    A("#include <stdint.h>")
    A("")
    A("#ifdef __cplusplus")
    A('extern "C" {')
    A("#endif")
    A("")
    A(f"/* FACE data type: {qname} (message GUID {guid}). */")
    A("")
    for ename, edef in enums.items():
        A(f"/* Enum {ename} : {edef.utype} */")
        A(f"typedef enum {ename} {{")
        for mname, mval in edef.members:
            A(f"    {ename}_{mname} = {mval},")
        A(f"}} {ename};")
        A("")
    for uname, udef in unions.items():
        A(f"/* Union {uname} (discriminator serialized as ubyte) */")
        A(f"typedef enum {uname} {{")
        A(f"    {uname}_NONE = 0,")
        for idx, mem in enumerate(udef.members, 1):
            A(f"    {uname}_{mem} = {idx},")
        A(f"}} {uname};")
        A(f"typedef struct {uname}_value {{")
        A(f"    {uname} type;")
        A(f"    void *value; /* owned struct per type, NULL when NONE */")
        A(f"}} {uname}_value;")
        A("")
    for t in tables:
        A(f"struct {t.name};")
    A("")
    for t in tables:
        A(f"typedef struct {t.name} {{")
        for f in t.fields:
            if f.kind == "string":
                A(f"    char *{f.name}; /* owned, freed by {root_name}_fini */")
            elif f.kind == "scalar":
                A(f"    {SCALARS[f.ftype][0]} {f.name};")
            elif f.kind == "enum":
                A(f"    {f.ref} {f.name};")
            elif f.kind == "table":
                A(f"    struct {f.ref} *{f.name}; /* owned, NULL if absent */")
            elif f.kind == "vector":
                if f.velem == "scalar":
                    ctype = SCALARS[f.ftype][0]
                    A(f"    {ctype} *{f.name}; /* owned */")
                elif f.velem == "string":
                    A(f"    char **{f.name}; /* owned array of owned */")
                else:  # table
                    A(f"    struct {f.ref} **{f.name}; "
                      f"/* owned array of owned */")
                A(f"    size_t {f.name}_count;")
            elif f.kind == "vecvec":
                ctype = SCALARS[f.ftype][0]
                A(f"    {ctype} **{f.name}; /* owned array of owned arrays */")
                A(f"    size_t *{f.name}_counts; /* per-row counts, owned */")
                A(f"    size_t {f.name}_count;")
            elif f.kind == "union":
                A(f"    {f.ref}_value {f.name}; /* owned, value NULL "
                  f"when type is {f.ref}_NONE */")
        A(f"}} {t.name};")
        A("")
    A(f"#define {root_name.upper()}_MESSAGE_GUID "
      f"((FACE_TSS_MESSAGE_GUID_TYPE){guid}LL)")
    A("")
    A(f"FACE_TSS_RETURN_CODE {root_name}_serialize(")
    A("    const void *msg, uint8_t **payload_out, size_t *payload_len_out);")
    A(f"FACE_TSS_RETURN_CODE {root_name}_deserialize(")
    A("    const uint8_t *payload, size_t payload_len, void *msg_out);")
    A(f"void {root_name}_fini(void *msg);")
    A("")
    A(f"extern const FACE_TSS_TYPE_SUPPORT {root_name}_type_support;")
    A("")
    A("#ifdef __cplusplus")
    A("}")
    A("#endif")
    A("")
    A(f"#endif /* {guard} */")
    return "\n".join(L) + "\n"


# ---------------------------------------------------------------------------
# source
# ---------------------------------------------------------------------------

def emit_scalar_store(L, idx, ftype, value_expr):
    """Emit code storing a scalar value into table field idx. Returns 0->err."""
    ctype, width, kind = SCALARS[ftype]
    L.append("    {")
    L.append(f"        {ctype} *slot = ({ctype} *)"
             f"flatcc_builder_table_add(B, {idx}, {width}, {width});")
    L.append("        if (!slot) return 0;")
    if width == 1:
        L.append(f"        *slot = ({ctype})({value_expr});")
        L.append("    }")
        return
    fn, pct = PE[kind]
    if kind in ("f32", "f64"):
        ut = "uint32_t" if kind == "f32" else "uint64_t"
        uw = 4 if kind == "f32" else 8
        L.append(f"        {ctype} tmp_v = ({ctype})({value_expr});")
        L.append(f"        {ut} tmp_u; memcpy(&tmp_u, &tmp_v, {uw});")
        L.append(f"        {fn}(({ut} *)slot, tmp_u);")
        L.append("    }")
        return
    L.append(f"        {fn}(slot, ({pct})({value_expr}));")
    L.append("    }")


def gen_build_table(t, unions):
    L = []
    A = L.append
    A(f"static flatcc_builder_ref_t build_{t.name}("
      f"flatcc_builder_t *B, const struct {t.name} *m)")
    A("{")
    A("    flatcc_builder_ref_t root;")
    # Phase 1: create strings, vectors, nested tables first (innermost first).
    for f in t.fields:
        i = f.fid
        if f.kind == "string":
            A(f"    /* field {i}: {f.name} (string) */")
            A(f"    flatcc_builder_ref_t {f.name}_ref =")
            A(f"        flatcc_builder_create_string_str("
              f"B, m->{f.name} ? m->{f.name} : \"\");")
            A(f"    if (!{f.name}_ref) return 0;")
        elif f.kind == "vector" and f.velem == "scalar":
            ctype, width, _ = SCALARS[f.ftype]
            A(f"    /* field {i}: {f.name} ([{f.ftype}]) */")
            A(f"    flatcc_builder_ref_t {f.name}_ref = 0;")
            A(f"    if (m->{f.name} && m->{f.name}_count > 0) {{")
            A(f"        {f.name}_ref = flatcc_builder_create_vector(")
            A(f"            B, m->{f.name}, m->{f.name}_count, "
              f"{width}, {width}, 16777215);")
            A(f"        if (!{f.name}_ref) return 0;")
            A("    }")
        elif f.kind == "vector" and f.velem in ("string", "table"):
            what = "strings" if f.velem == "string" else f"tables {f.ref}"
            A(f"    /* field {i}: {f.name} (vector of {what}) */")
            A(f"    flatcc_builder_ref_t {f.name}_ref = 0;")
            A(f"    if (m->{f.name} && m->{f.name}_count > 0) {{")
            A("        size_t _n;")
            A("        if (flatcc_builder_start_offset_vector(B)) return 0;")
            A(f"        for (_n = 0; _n < m->{f.name}_count; _n++) {{")
            A("            flatcc_builder_ref_t _r;")
            A(f"            if (!m->{f.name}[_n]) return 0; "
              f"/* NULL elements are not supported */")
            if f.velem == "string":
                A(f"            _r = flatcc_builder_create_string_str("
                  f"B, m->{f.name}[_n]);")
            else:
                A(f"            _r = build_{f.ref}(B, m->{f.name}[_n]);")
            A("            if (!_r) return 0;")
            A("            if (!flatcc_builder_offset_vector_push(B, _r))")
            A("                return 0;")
            A("        }")
            A(f"        {f.name}_ref = "
              f"flatcc_builder_end_offset_vector(B);")
            A(f"        if (!{f.name}_ref) return 0;")
            A("    }")
        elif f.kind == "vecvec":
            ctype, width, _ = SCALARS[f.ftype]
            A(f"    /* field {i}: {f.name} ([[{f.ftype}]]) */")
            A(f"    flatcc_builder_ref_t {f.name}_ref = 0;")
            A(f"    if (m->{f.name} && m->{f.name}_count > 0) {{")
            A("        size_t _n;")
            A("        if (flatcc_builder_start_offset_vector(B)) return 0;")
            A(f"        for (_n = 0; _n < m->{f.name}_count; _n++) {{")
            A("            flatcc_builder_ref_t _r;")
            A(f"            if (m->{f.name}[_n] && m->{f.name}_counts[_n] > 0)")
            A("                _r = flatcc_builder_create_vector(")
            A(f"                    B, m->{f.name}[_n], m->{f.name}_counts[_n],"
              f" {width}, {width}, 16777215);")
            A("            else")
            A("                /* empty inner: emit a real empty vector "
              "(null refs are invalid) */")
            A("                _r = flatcc_builder_create_vector(")
            A(f"                    B, \"\", 0, {width}, {width}, 16777215);")
            A("            if (!_r) return 0;")
            A("            if (!flatcc_builder_offset_vector_push(B, _r))")
            A("                return 0;")
            A("        }")
            A(f"        {f.name}_ref = "
              f"flatcc_builder_end_offset_vector(B);")
            A(f"        if (!{f.name}_ref) return 0;")
            A("    }")
        elif f.kind == "table":
            A(f"    /* field {i}: {f.name} (nested {f.ref}) */")
            A(f"    flatcc_builder_ref_t {f.name}_ref = 0;")
            A(f"    if (m->{f.name}) {{")
            A(f"        {f.name}_ref = build_{f.ref}(B, m->{f.name});")
            A(f"        if (!{f.name}_ref) return 0;")
            A("    }")
        elif f.kind == "union":
            udef = unions[f.ref]
            A(f"    /* field {i}/{i + 1}: {f.name} (union {f.ref}) */")
            A(f"    flatcc_builder_ref_t {f.name}_ref = 0;")
            A(f"    uint8_t {f.name}_type = {f.ref}_NONE;")
            A(f"    if (m->{f.name}.value) {{")
            A(f"        switch (m->{f.name}.type) {{")
            for idx, mem in enumerate(udef.members, 1):
                A(f"        case {f.ref}_{mem}:")
                A(f"            {f.name}_ref = build_{mem}(B, "
                  f"(const struct {mem} *)m->{f.name}.value);")
                A("            break;")
            A("        default:")
            A("            return 0; /* unknown union discriminator */")
            A("        }")
            A(f"        if (!{f.name}_ref) return 0;")
            A(f"        {f.name}_type = (uint8_t)m->{f.name}.type;")
            A(f"    }} else if (m->{f.name}.type != {f.ref}_NONE) {{")
            A("        return 0; /* discriminator set but no value */")
            A("    }")
    # Phase 2: the table itself.
    A(f"    if (flatcc_builder_start_table(B, {t.nslots})) return 0;")
    for f in t.fields:
        i = f.fid
        if f.kind == "string":
            A(f"    /* field {i}: {f.name} */")
            A("    {")
            A(f"        flatcc_builder_ref_t *pref = "
              f"flatcc_builder_table_add_offset(B, {i});")
            A("        if (!pref) return 0;")
            A(f"        *pref = {f.name}_ref;")
            A("    }")
        elif f.kind in ("vector", "vecvec", "table"):
            A(f"    /* field {i}: {f.name} */")
            A(f"    if ({f.name}_ref) {{")
            A("        flatcc_builder_ref_t *pref = "
              f"flatcc_builder_table_add_offset(B, {i});")
            A("        if (!pref) return 0;")
            A(f"        *pref = {f.name}_ref;")
            A("    }")
        elif f.kind == "union":
            A(f"    /* field {i}/{i + 1}: {f.name} */")
            A(f"    if ({f.name}_ref) {{")
            A("        uint8_t *tslot = (uint8_t *)"
              f"flatcc_builder_table_add(B, {i}, 1, 1);")
            A("        flatcc_builder_ref_t *pref;")
            A("        if (!tslot) return 0;")
            A(f"        *tslot = {f.name}_type;")
            A("        pref = "
              f"flatcc_builder_table_add_offset(B, {i + 1});")
            A("        if (!pref) return 0;")
            A(f"        *pref = {f.name}_ref;")
            A("    }")
        elif f.kind == "scalar":
            A(f"    /* field {i}: {f.name} */")
            emit_scalar_store(L, i, f.ftype, f"m->{f.name}")
        elif f.kind == "enum":
            A(f"    /* field {i}: {f.name} (enum {f.ref}) */")
            emit_scalar_store(L, i, f.ftype, f"m->{f.name}")
    A("    root = flatcc_builder_end_table(B);")
    A("    return root;")
    A("}")
    return "\n".join(L)


def _parse_vector_head(L, tname, fname, fid, what):
    """Emit the shared 'read an offset vector field' preamble. The caller
    has already emitted the field comment, entry lookup and opened
    'if (entry) {'. Leaves n (count), start (first element) set."""
    A = L.append
    A("        uint32_t off, vat, n, start;")
    A("        if (entry + 4 > tsize)")
    A("            return FACE_TSS_RC_INVALID_PARAM;")
    A("        at = table + entry;")
    A("        if (at > (uint32_t)(len - 4))")
    A("            return FACE_TSS_RC_INVALID_PARAM;")
    A("        off = rd_u32(buf + at);")
    A("        if (off == 0 || off > (uint32_t)(len - at - 4))")
    A("            return FACE_TSS_RC_INVALID_PARAM;")
    A("        vat = at + off;")
    A("        if (vat > (uint32_t)(len - 4))")
    A("            return FACE_TSS_RC_INVALID_PARAM;")
    A("        n = rd_u32(buf + vat); start = vat + 4;")


def _parse_elem_off(L, tname):
    """Inside a per-element loop with _vi: compute _eat (element target)
    with bounds checks. Uses start/n/len from the vector head."""
    A = L.append
    A("            uint32_t _eoff, _eat, _ebase;")
    A("            _ebase = start + _vi * 4;")
    A("            if (_ebase > (uint32_t)(len - 4))")
    A(f"                {{ fini_{tname}(m); "
      f"return FACE_TSS_RC_INVALID_PARAM; }}")
    A("            _eoff = rd_u32(buf + _ebase);")
    A("            if (_eoff == 0 || _eoff > (uint32_t)(len - _ebase - 4))")
    A(f"                {{ fini_{tname}(m); "
      f"return FACE_TSS_RC_INVALID_PARAM; }}")
    A("            _eat = _ebase + _eoff;")


def gen_parse_table(t, unions):
    L = []
    A = L.append
    A(f"static FACE_TSS_RETURN_CODE parse_{t.name}("
      f"const uint8_t *buf, size_t len, uint32_t table, struct {t.name} *m)")
    A("{")
    A("    uint32_t vtable, vsize, tsize, entry, at;")
    A("    FACE_TSS_RETURN_CODE rc;")
    A("    if (table_header(buf, len, table, &vtable, &vsize, &tsize))")
    A("        return FACE_TSS_RC_INVALID_PARAM;")
    for f in t.fields:
        i = f.fid
        A(f"    /* field {i}: {f.name} */")
        A(f"    entry = field_at(buf, vtable, vsize, {i});")
        A("    if (entry) {")
        if f.kind == "string":
            A("        uint32_t off, n, start;")
            A(f"        if (entry + 4 > tsize)")
            A("            return FACE_TSS_RC_INVALID_PARAM;")
            A("        at = table + entry;")
            A("        if (at > (uint32_t)(len - 4))")
            A("            return FACE_TSS_RC_INVALID_PARAM;")
            A("        off = rd_u32(buf + at);")
            A("        if (off == 0 || off > (uint32_t)(len - at - 4))")
            A("            return FACE_TSS_RC_INVALID_PARAM;")
            A("        at += off;")
            A("        if (at > (uint32_t)(len - 4))")
            A("            return FACE_TSS_RC_INVALID_PARAM;")
            A("        n = rd_u32(buf + at); start = at + 4;")
            A("        if (n + 1 > len - start || buf[start + n] != '\\0')")
            A("            return FACE_TSS_RC_INVALID_PARAM;")
            A(f"        m->{f.name} = (char *)malloc(n + 1);")
            A(f"        if (!m->{f.name}) {{ fini_{t.name}(m);")
            A("            return FACE_TSS_RC_NOT_AVAILABLE; }")
            A(f"        memcpy(m->{f.name}, buf + start, n);")
            A(f"        m->{f.name}[n] = '\\0';")
        elif f.kind in ("scalar", "enum"):
            ctype, width, _ = SCALARS[f.ftype]
            A(f"        if (entry + {width} > tsize)")
            A("            return FACE_TSS_RC_INVALID_PARAM;")
            A("        at = table + entry;")
            A(f"        if (at > (uint32_t)(len - {width}))")
            A("            return FACE_TSS_RC_INVALID_PARAM;")
            if f.kind == "scalar":
                A(f"        memcpy(&m->{f.name}, buf + at, {width});")
            else:
                A(f"        {{ {ctype} tmp; memcpy(&tmp, buf + at, {width});")
                A(f"          m->{f.name} = ({f.ref})tmp; }}")
        elif f.kind == "table":
            A("        uint32_t off, nat;")
            A("        if (entry + 4 > tsize)")
            A("            return FACE_TSS_RC_INVALID_PARAM;")
            A("        at = table + entry;")
            A("        if (at > (uint32_t)(len - 4))")
            A("            return FACE_TSS_RC_INVALID_PARAM;")
            A("        off = rd_u32(buf + at);")
            A("        if (off == 0 || off > (uint32_t)(len - at - 4))")
            A("            return FACE_TSS_RC_INVALID_PARAM;")
            A("        nat = at + off;")
            A(f"        m->{f.name} = (struct {f.ref} *)"
              f"calloc(1, sizeof(*m->{f.name}));")
            A(f"        if (!m->{f.name}) {{ fini_{t.name}(m);")
            A("            return FACE_TSS_RC_NOT_AVAILABLE; }")
            A(f"        rc = parse_{f.ref}(buf, len, nat, m->{f.name});")
            A(f"        if (rc != FACE_TSS_RC_NO_ERROR) {{ "
              f"fini_{t.name}(m); return rc; }}")
        elif f.kind == "vector" and f.velem == "scalar":
            ctype, width, _ = SCALARS[f.ftype]
            A("        uint32_t off, vat, n, start;")
            A("        if (entry + 4 > tsize)")
            A("            return FACE_TSS_RC_INVALID_PARAM;")
            A("        at = table + entry;")
            A("        if (at > (uint32_t)(len - 4))")
            A("            return FACE_TSS_RC_INVALID_PARAM;")
            A("        off = rd_u32(buf + at);")
            A("        if (off == 0 || off > (uint32_t)(len - at - 4))")
            A("            return FACE_TSS_RC_INVALID_PARAM;")
            A("        vat = at + off;")
            A("        if (vat > (uint32_t)(len - 4))")
            A("            return FACE_TSS_RC_INVALID_PARAM;")
            A("        n = rd_u32(buf + vat); start = vat + 4;")
            A(f"        if (n > (len - start) / {width})")
            A("            return FACE_TSS_RC_INVALID_PARAM;")
            A("        if (n > 0) {")
            A(f"            m->{f.name} = ({ctype} *)"
              f"malloc((size_t)n * {width});")
            A(f"            if (!m->{f.name}) {{ fini_{t.name}(m);")
            A("                return FACE_TSS_RC_NOT_AVAILABLE; }")
            A(f"            memcpy(m->{f.name}, buf + start, "
              f"(size_t)n * {width});")
            A(f"            m->{f.name}_count = n;")
            A("        }")
        elif f.kind == "vector" and f.velem == "string":
            _parse_vector_head(L, t.name, f.name, i, "[string]")
            A("        if (n > (len - start) / 4)")
            A("            return FACE_TSS_RC_INVALID_PARAM;")
            A("        if (n > 0) {")
            A("            uint32_t _vi;")
            A(f"            m->{f.name} = (char **)calloc(n, "
              f"sizeof(*m->{f.name}));")
            A(f"            if (!m->{f.name}) {{ fini_{t.name}(m);")
            A("                return FACE_TSS_RC_NOT_AVAILABLE; }")
            A("            for (_vi = 0; _vi < n; _vi++) {")
            A("                uint32_t _slen, _sstart;")
            _parse_elem_off(L, t.name)
            A("                if (_eat > (uint32_t)(len - 4))")
            A(f"                    {{ fini_{t.name}(m); "
              f"return FACE_TSS_RC_INVALID_PARAM; }}")
            A("                _slen = rd_u32(buf + _eat);")
            A("                _sstart = _eat + 4;")
            A("                if (_slen + 1 > len - _sstart ||")
            A("                    buf[_sstart + _slen] != '\\0')")
            A(f"                    {{ fini_{t.name}(m); "
              f"return FACE_TSS_RC_INVALID_PARAM; }}")
            A(f"                m->{f.name}[_vi] = "
              f"(char *)malloc(_slen + 1);")
            A(f"                if (!m->{f.name}[_vi]) {{ fini_{t.name}(m);")
            A("                    return FACE_TSS_RC_NOT_AVAILABLE; }")
            A(f"                memcpy(m->{f.name}[_vi], buf + _sstart, _slen);")
            A(f"                m->{f.name}[_vi][_slen] = '\\0';")
            A("            }")
            A(f"            m->{f.name}_count = n;")
            A("        }")
            A("    }")
            continue
        elif f.kind == "vector" and f.velem == "table":
            _parse_vector_head(L, t.name, f.name, i, f"[{f.ref}]")
            A("        if (n > (len - start) / 4)")
            A("            return FACE_TSS_RC_INVALID_PARAM;")
            A("        if (n > 0) {")
            A("            uint32_t _vi;")
            A(f"            m->{f.name} = (struct {f.ref} **)calloc(n, "
              f"sizeof(*m->{f.name}));")
            A(f"            if (!m->{f.name}) {{ fini_{t.name}(m);")
            A("                return FACE_TSS_RC_NOT_AVAILABLE; }")
            A("            for (_vi = 0; _vi < n; _vi++) {")
            _parse_elem_off(L, t.name)
            A(f"                m->{f.name}[_vi] = (struct {f.ref} *)"
              f"calloc(1, sizeof(*m->{f.name}[_vi]));")
            A(f"                if (!m->{f.name}[_vi]) {{ fini_{t.name}(m);")
            A("                    return FACE_TSS_RC_NOT_AVAILABLE; }")
            A(f"                rc = parse_{f.ref}(buf, len, _eat, "
              f"m->{f.name}[_vi]);")
            A(f"                if (rc != FACE_TSS_RC_NO_ERROR) {{ "
              f"fini_{t.name}(m); return rc; }}")
            A("            }")
            A(f"            m->{f.name}_count = n;")
            A("        }")
            A("    }")
            continue
        elif f.kind == "vecvec":
            ctype, width, _ = SCALARS[f.ftype]
            _parse_vector_head(L, t.name, f.name, i, f"[[{f.ftype}]]")
            A("        if (n > (len - start) / 4)")
            A("            return FACE_TSS_RC_INVALID_PARAM;")
            A("        if (n > 0) {")
            A("            uint32_t _vi;")
            A(f"            m->{f.name} = ({ctype} **)calloc(n, "
              f"sizeof(*m->{f.name}));")
            A(f"            m->{f.name}_counts = (size_t *)calloc(n, "
              f"sizeof(*m->{f.name}_counts));")
            A(f"            if (!m->{f.name} || !m->{f.name}_counts) {{ "
              f"fini_{t.name}(m);")
            A("                return FACE_TSS_RC_NOT_AVAILABLE; }")
            A("            for (_vi = 0; _vi < n; _vi++) {")
            A("                uint32_t _in, _istart;")
            _parse_elem_off(L, t.name)
            A("                if (_eat > (uint32_t)(len - 4))")
            A(f"                    {{ fini_{t.name}(m); "
              f"return FACE_TSS_RC_INVALID_PARAM; }}")
            A("                _in = rd_u32(buf + _eat);")
            A("                _istart = _eat + 4;")
            A(f"                if (_in > (len - _istart) / {width})")
            A(f"                    {{ fini_{t.name}(m); "
              f"return FACE_TSS_RC_INVALID_PARAM; }}")
            A("                if (_in > 0) {")
            A(f"                    m->{f.name}[_vi] = ({ctype} *)"
              f"malloc((size_t)_in * {width});")
            A(f"                    if (!m->{f.name}[_vi]) {{ "
              f"fini_{t.name}(m);")
            A("                        return FACE_TSS_RC_NOT_AVAILABLE; }")
            A(f"                    memcpy(m->{f.name}[_vi], buf + _istart, "
              f"(size_t)_in * {width});")
            A(f"                    m->{f.name}_counts[_vi] = _in;")
            A("                }")
            A("            }")
            A(f"            m->{f.name}_count = n;")
            A("        }")
            A("    }")
            continue
        elif f.kind == "union":
            udef = unions[f.ref]
            A(f"        uint8_t _ut;")
            A(f"        if (entry + 1 > tsize)")
            A("            return FACE_TSS_RC_INVALID_PARAM;")
            A("        at = table + entry;")
            A("        if (at > (uint32_t)(len - 1))")
            A("            return FACE_TSS_RC_INVALID_PARAM;")
            A("        _ut = buf[at];")
            A(f"        if (_ut != {f.ref}_NONE) {{")
            A("            uint32_t _vent, _voff, _vnat;")
            A(f"            _vent = field_at(buf, vtable, vsize, {i + 1});")
            A("            if (!_vent)")
            A(f"                {{ fini_{t.name}(m); "
              f"return FACE_TSS_RC_INVALID_PARAM; }} "
              f"/* type set but no value */")
            A("            if (_vent + 4 > tsize)")
            A("                return FACE_TSS_RC_INVALID_PARAM;")
            A("            at = table + _vent;")
            A("            if (at > (uint32_t)(len - 4))")
            A("                return FACE_TSS_RC_INVALID_PARAM;")
            A("            _voff = rd_u32(buf + at);")
            A("            if (_voff == 0 || _voff > (uint32_t)(len - at - 4))")
            A("                return FACE_TSS_RC_INVALID_PARAM;")
            A("            _vnat = at + _voff;")
            A("            switch (_ut) {")
            for mem in udef.members:
                A(f"            case {f.ref}_{mem}:")
                A(f"                m->{f.name}.value = "
                  f"calloc(1, sizeof(struct {mem}));")
                A(f"                if (!m->{f.name}.value) {{ "
                  f"fini_{t.name}(m);")
                A("                    return FACE_TSS_RC_NOT_AVAILABLE; }")
                A(f"                rc = parse_{mem}(buf, len, _vnat, "
                  f"(struct {mem} *)m->{f.name}.value);")
                A("                break;")
            A("            default:")
            A(f"                fini_{t.name}(m);")
            A("                return FACE_TSS_RC_INVALID_PARAM; "
              "/* unknown discriminator */")
            A("            }")
            A(f"            if (rc != FACE_TSS_RC_NO_ERROR) {{ "
              f"fini_{t.name}(m); return rc; }}")
            A(f"            m->{f.name}.type = ({f.ref})_ut;")
            A("        }")
        A("    }")
    A("    (void)rc;")
    A("    return FACE_TSS_RC_NO_ERROR;")
    A("}")
    return "\n".join(L)


def gen_fini_table(t, unions):
    L = []
    A = L.append
    A(f"static void fini_{t.name}(struct {t.name} *m)")
    A("{")
    A("    if (!m) return;")
    for f in t.fields:
        if f.kind == "string":
            A(f"    free(m->{f.name}); m->{f.name} = NULL;")
        elif f.kind == "table":
            A(f"    if (m->{f.name}) {{ fini_{f.ref}(m->{f.name});")
            A(f"        free(m->{f.name}); m->{f.name} = NULL; }}")
        elif f.kind == "vector" and f.velem == "scalar":
            A(f"    free(m->{f.name}); m->{f.name} = NULL;")
            A(f"    m->{f.name}_count = 0;")
        elif f.kind == "vector" and f.velem == "string":
            A(f"    if (m->{f.name}) {{")
            A(f"        size_t _i;")
            A(f"        for (_i = 0; _i < m->{f.name}_count; _i++)")
            A(f"            free(m->{f.name}[_i]);")
            A(f"        free(m->{f.name}); m->{f.name} = NULL;")
            A(f"    }}")
            A(f"    m->{f.name}_count = 0;")
        elif f.kind == "vector" and f.velem == "table":
            A(f"    if (m->{f.name}) {{")
            A(f"        size_t _i;")
            A(f"        for (_i = 0; _i < m->{f.name}_count; _i++) {{")
            A(f"            if (m->{f.name}[_i]) {{")
            A(f"                fini_{f.ref}(m->{f.name}[_i]);")
            A(f"                free(m->{f.name}[_i]);")
            A(f"            }}")
            A(f"        }}")
            A(f"        free(m->{f.name}); m->{f.name} = NULL;")
            A(f"    }}")
            A(f"    m->{f.name}_count = 0;")
        elif f.kind == "vecvec":
            A(f"    if (m->{f.name}) {{")
            A(f"        size_t _i;")
            A(f"        for (_i = 0; _i < m->{f.name}_count; _i++)")
            A(f"            free(m->{f.name}[_i]);")
            A(f"        free(m->{f.name}); m->{f.name} = NULL;")
            A(f"    }}")
            A(f"    free(m->{f.name}_counts); m->{f.name}_counts = NULL;")
            A(f"    m->{f.name}_count = 0;")
        elif f.kind == "union":
            udef = unions[f.ref]
            A(f"    if (m->{f.name}.value) {{")
            A(f"        switch (m->{f.name}.type) {{")
            for mem in udef.members:
                A(f"        case {f.ref}_{mem}:")
                A(f"            fini_{mem}((struct {mem} *)m->{f.name}.value);")
                A("            break;")
            A("        default:")
            A("            break;")
            A("        }")
            A(f"        free(m->{f.name}.value);")
            A(f"        m->{f.name}.value = NULL;")
            A("    }")
            A(f"    m->{f.name}.type = {f.ref}_NONE;")
    A("}")
    return "\n".join(L)


def gen_source(root_name, qname, tables, enums, unions, guid):
    L = []
    A = L.append
    A("/* Generated by tools/face_tss_codegen.py - DO NOT EDIT. */")
    A(f"/* Data type: {qname}. Codec over the flatcc runtime builder API,")
    A(" * mirroring c/src/envelope.c (no generated flatcc code needed). */")
    A("")
    A(f'#include "{root_name.lower()}_typed.h"')
    A("")
    A("#include <stdlib.h>")
    A("#include <string.h>")
    A("")
    A("#include <flatcc/flatcc_builder.h>")
    A("#include <flatcc/flatcc_endian.h>")
    A("")
    # ---- shared decode helpers ----
    A("/* Little-endian field readers (same approach as c/src/envelope.c). */")
    A("static uint16_t rd_u16(const uint8_t *p)")
    A("{ uint16_t v; memcpy(&v, p, 2); return v; }")
    A("static uint32_t rd_u32(const uint8_t *p)")
    A("{ uint32_t v; memcpy(&v, p, 4); return v; }")
    A("")
    A("/* Parse a table header: vtable location and vtable/table sizes. */")
    A("static int table_header(const uint8_t *buf, size_t len, uint32_t table,")
    A("                        uint32_t *vtable_out, uint32_t *vsize_out,")
    A("                        uint32_t *tsize_out)")
    A("{")
    A("    int32_t soff = (int32_t)rd_u32(buf + table);")
    A("    uint32_t vtable = table - (uint32_t)soff;")
    A("    uint32_t vsize, tsize;")
    A("    if (vtable > (uint32_t)(len - 4)) return -1;")
    A("    vsize = rd_u16(buf + vtable);")
    A("    tsize = rd_u16(buf + vtable + 2);")
    A("    if (vsize < 4 || (vsize & 1u) || vsize > (uint32_t)(len - vtable) ||")
    A("        tsize < 4 || tsize > (uint32_t)(len - table)) return -1;")
    A("    *vtable_out = vtable; *vsize_out = vsize; *tsize_out = tsize;")
    A("    return 0;")
    A("}")
    A("")
    A("/* Vtable slot for field idx, or 0 when the field is absent/default. */")
    A("static uint32_t field_at(const uint8_t *buf, uint32_t vtable,")
    A("                         uint32_t vsize, int idx)")
    A("{")
    A("    if (vsize < (uint32_t)(4 + 2 * idx + 2)) return 0;")
    A("    return rd_u16(buf + vtable + 4 + 2 * idx);")
    A("}")
    A("")
    # ---- forward declarations (tables may reference later tables) ----
    for t in tables:
        A(f"struct {t.name};")
    for t in tables:
        A(f"static flatcc_builder_ref_t build_{t.name}("
          f"flatcc_builder_t *B, const struct {t.name} *m);")
    for t in tables:
        A(f"static FACE_TSS_RETURN_CODE parse_{t.name}("
          f"const uint8_t *buf, size_t len, uint32_t table, "
          f"struct {t.name} *m);")
    for t in tables:
        A(f"static void fini_{t.name}(struct {t.name} *m);")
    A("")
    # ---- per-table builders ----
    for t in tables:
        A(gen_build_table(t, unions))
        A("")
    # ---- per-table finis (before parsers: parsers call fini on error) ----
    for t in tables:
        A(gen_fini_table(t, unions))
        A("")
    # ---- per-table parsers ----
    for t in tables:
        A(gen_parse_table(t, unions))
        A("")
    # ---- public fini for the root type ----
    A(f"void {root_name}_fini(void *msg)")
    A("{")
    A(f"    fini_{root_name}((struct {root_name} *)msg);")
    A("}")
    A("")
    # ---- public serialize ----
    A(f"FACE_TSS_RETURN_CODE {root_name}_serialize(")
    A("    const void *msg, uint8_t **payload_out, size_t *payload_len_out)")
    A("{")
    A(f"    const struct {root_name} *m = (const struct {root_name} *)msg;")
    A("    flatcc_builder_t builder;")
    A("    flatcc_builder_t *B = &builder;")
    A("    flatcc_builder_ref_t root;")
    A("    void *buf;")
    A("    size_t size;")
    A("    if (!msg || !payload_out || !payload_len_out)")
    A("        return FACE_TSS_RC_INVALID_PARAM;")
    A("    flatcc_builder_init(B);")
    A(f"    root = build_{root_name}(B, m);")
    A("    if (!root || flatcc_builder_start_buffer(B, 0, 0, 0) ||")
    A("        !flatcc_builder_end_buffer(B, root)) {")
    A("        flatcc_builder_clear(B);")
    A("        return FACE_TSS_RC_NOT_AVAILABLE; }")
    A("    buf = flatcc_builder_finalize_buffer(B, &size);")
    A("    flatcc_builder_clear(B);")
    A("    if (!buf || size == 0) {")
    A("        if (buf) flatcc_builder_free(buf);")
    A("        return FACE_TSS_RC_NOT_AVAILABLE; }")
    A("    /* Copy to caller-owned malloc buffer: the finalize buffer must be")
    A("     * released with flatcc_builder_free, so keep ownership explicit. */")
    A("    {")
    A("        uint8_t *copy = (uint8_t *)malloc(size);")
    A("        if (!copy) { flatcc_builder_free(buf);")
    A("            return FACE_TSS_RC_NOT_AVAILABLE; }")
    A("        memcpy(copy, buf, size);")
    A("        flatcc_builder_free(buf);")
    A("        *payload_out = copy;")
    A("    }")
    A("    *payload_len_out = size;")
    A("    return FACE_TSS_RC_NO_ERROR;")
    A("}")
    A("")
    # ---- public deserialize ----
    A(f"FACE_TSS_RETURN_CODE {root_name}_deserialize(")
    A("    const uint8_t *payload, size_t payload_len, void *msg_out)")
    A("{")
    A(f"    struct {root_name} *m = (struct {root_name} *)msg_out;")
    A("    const uint8_t *buf = payload;")
    A("    size_t len = payload_len;")
    A("    uint32_t root, table, vtable, vsize, tsize;")
    A("    int32_t soff;")
    A("    uint8_t *copy = NULL;")
    A("    FACE_TSS_RETURN_CODE rc;")
    A("    if (!msg_out)")
    A("        return FACE_TSS_RC_INVALID_PARAM;")
    A("    memset(m, 0, sizeof(*m));")
    A("    if (!buf || len == 0)")
    A("        return FACE_TSS_RC_INVALID_PARAM;")
    A("    if (((uintptr_t)buf & 3u) != 0u) {")
    A("        copy = (uint8_t *)malloc(len);")
    A("        if (!copy) return FACE_TSS_RC_NOT_AVAILABLE;")
    A("        memcpy(copy, buf, len); buf = copy;")
    A("    }")
    A("    if (len < 8 || len > (size_t)0xFFFFFFF0UL)")
    A("        { free(copy); return FACE_TSS_RC_INVALID_PARAM; }")
    A("    root = rd_u32(buf);")
    A("    if (root < 4 || root > (uint32_t)(len - 4))")
    A("        { free(copy); return FACE_TSS_RC_INVALID_PARAM; }")
    A("    table = root;")
    A("    soff = (int32_t)rd_u32(buf + table);")
    A("    vtable = table - (uint32_t)soff;")
    A("    if (vtable > (uint32_t)(len - 4))")
    A("        { free(copy); return FACE_TSS_RC_INVALID_PARAM; }")
    A("    vsize = rd_u16(buf + vtable);")
    A("    tsize = rd_u16(buf + vtable + 2);")
    A("    if (vsize < 4 || (vsize & 1u) || vsize > (uint32_t)(len - vtable) ||")
    A("        tsize < 4 || tsize > (uint32_t)(len - table))")
    A("        { free(copy); return FACE_TSS_RC_INVALID_PARAM; }")
    A(f"    rc = parse_{root_name}(buf, len, table, m);")
    A("    free(copy);")
    A("    return rc;")
    A("}")
    A("")
    # ---- descriptor ----
    A(f"const FACE_TSS_TYPE_SUPPORT {root_name}_type_support = {{")
    A(f"    {root_name.upper()}_MESSAGE_GUID,")
    A(f'    "{qname}",')
    A(f"    {root_name}_serialize,")
    A(f"    {root_name}_deserialize,")
    A(f"    {root_name}_fini,")
    A(f"    sizeof(struct {root_name}),")
    A(f"    65536, /* max_size */")
    A("};")
    return "\n".join(L) + "\n"


def main():
    ap = argparse.ArgumentParser(
        description="Generate FACE TSS typed C wrapper from .fbs")
    ap.add_argument("schema", help="Input .fbs file")
    ap.add_argument("--out-dir", required=True,
                    help="Output directory for generated files")
    args = ap.parse_args()
    schema = Path(args.schema)
    out_dir = Path(args.out_dir)
    namespace, root_name, qname, tables, enums, unions = parse_fbs(schema)
    guid = fnv1a64(qname) & 0x7FFFFFFFFFFFFFFF
    if guid == 0:
        guid = 1
    out_dir.mkdir(parents=True, exist_ok=True)
    stem = root_name.lower()
    (out_dir / f"{stem}_typed.h").write_text(
        gen_header(root_name, qname, tables, enums, unions, guid))
    (out_dir / f"{stem}_typed.c").write_text(
        gen_source(root_name, qname, tables, enums, unions, guid))
    print(f"generated {stem}_typed.h / {stem}_typed.c "
          f"for {qname} (guid {guid})")


if __name__ == "__main__":
    main()
