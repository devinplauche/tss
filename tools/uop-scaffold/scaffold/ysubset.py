"""Restricted YAML-subset parser for uop-scaffold descriptors.

Supports exactly:
  - mappings:   key: value      /   key:      (nested block follows)
  - lists:      - item          /   - key: value   (list of mappings)
  - scalars:    bare words, integers, "double-quoted", 'single-quoted'
  - comments:   # ...  (outside quotes, after whitespace or at line start)
  - indentation: spaces only, consistent

Anything else (tabs, flow syntax ``{a: b}`` / ``[1, 2]``, anchors, tags,
multi-line scalars, escapes inside quotes) is a hard error carrying a
line number. This is deliberately NOT YAML: it parses a tiny, predictable
subset and rejects the rest, so descriptor authors get errors instead of
surprises.
"""

import re

__all__ = ["parse", "YSubError"]


class YSubError(Exception):
    """Parse error. str() is ``line N: message``."""

    def __init__(self, lineno, msg):
        super().__init__(f"line {lineno}: {msg}")
        self.lineno = lineno
        self.msg = msg


_KEY_RE = re.compile(r"[A-Za-z0-9_]+")
_MAPLINE_RE = re.compile(r"[A-Za-z0-9_]+:( |$)")
_INT_RE = re.compile(r"[+-]?\d+")


def _strip_comment(text):
    """Cut a ``#`` comment, respecting single/double quotes."""
    out = []
    quote = None
    prev_space = True
    for ch in text:
        if quote is not None:
            out.append(ch)
            if ch == quote:
                quote = None
        elif ch in "\"'":
            quote = ch
            out.append(ch)
        elif ch == "#" and prev_space:
            break
        else:
            out.append(ch)
        prev_space = ch in " \t"
    return "".join(out).rstrip()


def _desugar(raw):
    """Rewrite ``- item`` lines into a ``-`` marker plus a virtual block.

    Turns ``- key: value`` at indent N into a ``-`` marker at N followed by
    ``key: value`` at N+2, so list items reuse the normal block parser.
    """
    out = []
    for lineno, indent, text in raw:
        if text == "-" or text.startswith("- "):
            body = text[1:].strip()
            out.append((lineno, indent, "-"))
            if body:
                out.append((lineno, indent + 2, body))
        else:
            out.append((lineno, indent, text))
    return out


def _parse_scalar(text, lineno):
    if len(text) >= 2 and text[0] == text[-1] and text[0] in "\"'":
        return text[1:-1]
    if text[:1] in "{[":
        raise YSubError(lineno, "flow syntax ({...}/[...]) is not supported")
    if _INT_RE.fullmatch(text):
        return int(text)
    if text == "":
        raise YSubError(lineno, "expected a value")
    return text


def _split_key_value(text, lineno):
    """Split ``key: value``. Returns (key, value_text, has_value)."""
    if ":" not in text:
        raise YSubError(lineno, f"expected 'key: value', got {text!r}")
    key, _, value = text.partition(":")
    key = key.strip()
    if not _KEY_RE.fullmatch(key or ""):
        raise YSubError(lineno, f"invalid key {key!r}")
    value = value.strip()
    return key, value, value != ""


def _parse_block(raw, i, indent):
    lineno, _, text = raw[i]
    if text == "-":
        return _parse_list(raw, i, indent)
    # A bare line is a mapping entry only when it looks like ``key:`` /
    # ``key: value`` (colon-terminated or colon+space); otherwise it is a
    # scalar (so ``- tcp://host:5555`` list items stay scalars).
    if _MAPLINE_RE.match(text):
        return _parse_map(raw, i, indent)
    return _parse_scalar(text, lineno), i + 1


def _parse_map(raw, i, indent):
    mapping = {}
    while i < len(raw):
        lineno, ind, text = raw[i]
        if ind < indent:
            break
        if ind > indent:
            raise YSubError(lineno, "unexpected indentation")
        if text == "-":
            raise YSubError(lineno, "list item inside a mapping")
        key, value, has_value = _split_key_value(text, lineno)
        if key in mapping:
            raise YSubError(lineno, f"duplicate key {key!r}")
        if has_value:
            mapping[key] = _parse_scalar(value, lineno)
            i += 1
        else:
            # ``key:`` with an empty value: nested block or null.
            if i + 1 < len(raw) and raw[i + 1][1] > indent:
                nested, i = _parse_block(raw, i + 1, raw[i + 1][1])
                mapping[key] = nested
            else:
                mapping[key] = None
                i += 1
    return mapping, i


def _parse_list(raw, i, indent):
    items = []
    while i < len(raw):
        lineno, ind, text = raw[i]
        if ind != indent or text != "-":
            break
        i += 1
        if i < len(raw) and raw[i][1] > indent:
            item, i = _parse_block(raw, i, raw[i][1])
            items.append(item)
        else:
            items.append(None)
    return items, i


def parse(text):
    """Parse a descriptor document. Returns nested dicts/lists/scalars."""
    raw = []
    for lineno, line in enumerate(text.splitlines(), 1):
        stripped = line.lstrip(" \t")
        leading = line[: len(line) - len(stripped)]
        if "\t" in leading:
            raise YSubError(lineno, "tabs are not allowed; use spaces")
        body = _strip_comment(stripped).strip()
        if not body:
            continue
        raw.append((lineno, len(leading), body))
    if not raw:
        raise YSubError(1, "empty document")
    raw = _desugar(raw)
    obj, next_i = _parse_block(raw, 0, raw[0][1])
    if next_i != len(raw):
        raise YSubError(raw[next_i][0], "unexpected content")
    if not isinstance(obj, dict):
        raise YSubError(raw[0][0], "top level must be a mapping")
    return obj
