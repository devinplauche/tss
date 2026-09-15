"""Bridge: IDL-defined descriptor types -> generated C codec files.

For each ``kind="idl"`` type, lowers its IDL file to ``.fbs`` (see
:mod:`scaffold.idl_to_fbs`) and runs the existing
``tools/face_tss_codegen.py`` pipeline, producing::

    <stem>.fbs        the lowered schema (transparency artifact)
    <stem>_typed.h    <T>_serialize / <T>_deserialize / <T>_fini
    <stem>_typed.c    the codec implementation

where ``<stem>`` is the lowercased descriptor type name and ``<T>`` the
C type name. Returns a ``{filename: content}`` dict for
:func:`scaffold.gen_uop.generate_tree` to merge into the output tree.
"""

import importlib.util
import tempfile
from pathlib import Path

from . import idl_parse, idl_to_fbs

__all__ = ["IdlError", "idl_type_files"]

IdlError = idl_parse.IdlError


def _load_codegen():
    """Import tools/face_tss_codegen.py by path.

    The scaffolder lives at <tss>/tools/uop-scaffold; the codegen at
    <tss>/tools/face_tss_codegen.py. If the tree was copied without it,
    IDL types cannot be generated: fail loudly.
    """
    cand = Path(__file__).resolve().parent.parent.parent / \
        "face_tss_codegen.py"
    if not cand.is_file():
        raise IdlError(
            "IDL types need tools/face_tss_codegen.py next to the "
            "uop-scaffold tree; not found at "
            f"{cand.parent} (copy the whole tools/ directory)")
    spec = importlib.util.spec_from_file_location("face_tss_codegen", cand)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def idl_type_files(tdef):
    """Generate the codec files for one IDL-defined type.

    ``tdef`` is a :class:`scaffold.model.TypeDef` with ``kind="idl"``.
    """
    defs = idl_parse.parse_idl(tdef.idl_path)
    # The loader resolved and validated this qualified name; lower_to_fbs
    # re-checks that it names a struct.
    qn = tuple(tdef.idl_qname.split("::"))
    fbs_text = idl_to_fbs.lower_to_fbs(defs, qn, tdef.idl_path)

    codegen = _load_codegen()
    stem = tdef.typed_stem
    with tempfile.TemporaryDirectory(prefix="uop_idl_") as td:
        schema_path = Path(td) / f"{tdef.name.lower()}.fbs"
        schema_path.write_text(fbs_text)
        _ns, root_name, qname, tables, enums, unions = \
            codegen.parse_fbs(schema_path)
        guid = codegen.fnv1a64(qname) & 0x7FFFFFFFFFFFFFFF
        if guid == 0:
            guid = 1
        header = codegen.gen_header(root_name, qname, tables, enums,
                                    unions, guid)
        source = codegen.gen_source(root_name, qname, tables, enums,
                                    unions, guid)
    return {
        f"{tdef.name.lower()}.fbs": fbs_text,
        f"{stem}.h": header,
        f"{stem}.c": source,
    }
