"""Shared codegen helpers for the uop-scaffold generators."""

__all__ = ["member_stems", "render_region"]


def member_stems(model):
    """Map each connection name to its lowercase C member stem.

    Raises ValueError on case-insensitive collisions.
    """
    seen = {}
    out = {}
    for c in model.connections:
        stem = c.name.lower()
        if stem in seen:
            raise ValueError(
                f"connections '{seen[stem]}' and '{c.name}' collide "
                f"as C identifiers")
        seen[stem] = c.name
        out[c.name] = stem
    return out


def render_region(name, body, indent=""):
    """Render a USER CODE region block, indenting only the marker lines.

    The body is the user's text verbatim: never re-indented, so
    regeneration is byte-stable (no indentation creep).
    """
    if not body.endswith("\n"):
        body += "\n"
    return (f"{indent}/* USER CODE BEGIN: {name} */\n"
            f"{body}"
            f"{indent}/* USER CODE END: {name} */")
