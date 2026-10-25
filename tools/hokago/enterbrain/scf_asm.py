"""Disassemble SCF classes to text and assemble them back.

Usage:
  python -m hokago scf_asm dis    <file.scf> [out.txt]
  python -m hokago scf_asm asm    <in.txt> <out.scf>
  python -m hokago scf_asm verify <SCENARIO.ARC | AMAGAMI.IMG>   (text round trip of every class)

The instruction set is documented in docs/technical/scf-bytecode.md. Text files
are UTF-8; the game's Shift-JIS strings are converted on the way in and out.
"""
import os
import re
import struct
import sys

from hokago.enterbrain import scf    # noqa: E402

# Operand kinds: u8, s8 (signed), u16, j8 / j16 (jump relative to the opcode's own offset).
INSTRUCTIONS = {
    0x00: ("nop",),
    0x01: ("nop1", "u8"),
    0x02: ("ret",),
    0x03: ("ret_top",),
    0x04: ("dup",),
    0x05: ("pop",),
    0x06: ("ne_ref",),
    0x07: ("eq_ref",),
    0x08: ("push_cvar", "u8", "u8"),
    0x09: ("push_cvar16", "u16", "u8"),
    0x0A: ("pop_cvar", "u8", "u8"),
    0x0B: ("pop_cvar16", "u16", "u8"),
    0x0C: ("and",),
    0x0D: ("or",),
    0x10: ("add",), 0x11: ("sub",), 0x12: ("mul",), 0x13: ("div",), 0x14: ("mod",),
    0x15: ("bit_and",), 0x16: ("bit_or",), 0x17: ("bit_xor",),
    0x18: ("eq",), 0x19: ("gt",), 0x1A: ("lt",), 0x1B: ("ge",), 0x1C: ("le",), 0x1D: ("ne",),
    0x1E: ("neg",),
    0x1F: ("bit_not",),
    0x20: ("push_ivar", "u8"),
    0x21: ("pop_ivar", "u8"),
    0x22: ("push_local", "u8"),
    0x23: ("pop_local", "u8"),
    0x24: ("push_int", "s8"),
    0x25: ("push_nil",),
    0x26: ("push_self",),
    0x28: ("push_true",),
    0x29: ("push_false",),
    0x2A: ("push_caught",),
    0x2B: ("not",),
    0x30: ("send", "u8", "u8"),
    0x31: ("send_super", "u8", "u8"),
    0x32: ("send16", "u8", "u16"),
    0x33: ("send_super16", "u8", "u16"),
    0x34: ("primitive", "u8"),
    0x40: ("jump", "j8"),
    0x41: ("jump16", "j16"),
    0x44: ("jump_false", "j8"),
    0x45: ("jump_false16", "j16"),
    0x50: ("push_const", "u8"),
    0x51: ("push_const16", "u16"),
    0x64: ("index_get",),
    0x65: ("index_set",),
    0x68: ("clear_caught",),
    0x69: ("try", "j16"),
    0x6A: ("end_try",),
    0x6B: ("rethrow",),
    0x6C: ("locals", "u8"),
    0x6D: ("line", "u16"),
}
OPCODES = {entry[0]: (opcode, entry[1:]) for opcode, entry in INSTRUCTIONS.items()}
SIZES = {"u8": 1, "s8": 1, "u16": 2, "j8": 1, "j16": 2}
FORMATS = {"u8": "<B", "s8": "<b", "u16": "<H", "j8": "<b", "j16": "<h"}
CONSTANT_OPS = {"push_const": 0, "push_const16": 0, "send": 1, "send_super": 1, "send16": 1, "send_super16": 1,
                "push_cvar": 0, "push_cvar16": 0, "pop_cvar": 0, "pop_cvar16": 0}


def disassemble(code):
    """Return [(offset, mnemonic, [operands])]. Jump operands are absolute offsets."""
    out, pos = [], 0
    while pos < len(code):
        start = pos
        entry = INSTRUCTIONS.get(code[pos])
        if entry is None:
            raise ValueError(f"unknown opcode {code[pos]:#04x} at {pos:#x}")
        pos += 1
        operands = []
        for kind in entry[1:]:
            if pos + SIZES[kind] > len(code):
                raise ValueError(f"truncated instruction at {start:#x}")
            value = struct.unpack_from(FORMATS[kind], code, pos)[0]
            pos += SIZES[kind]
            operands.append(start + value if kind[0] == "j" else value)
        out.append((start, entry[0], operands))
    return out


def assemble(instructions):
    """Inverse of disassemble. instructions: [(mnemonic, [operands])]."""
    out = bytearray()
    for mnemonic, operands in instructions:
        opcode, kinds = OPCODES[mnemonic]
        start = len(out)
        out.append(opcode)
        for kind, value in zip(kinds, operands):
            out += struct.pack(FORMATS[kind], value - start if kind[0] == "j" else value)
    return bytes(out)


# --- text form ---

def quote(raw):
    """Bytes to a quoted UTF-8 literal that converts back to the same bytes."""
    try:
        value = raw.decode(scf.ENCODING)
        if value.encode(scf.ENCODING) != raw or any(ord(c) < 0x20 and c not in "\n\t\r" for c in value):
            raise UnicodeError
    except UnicodeError:
        return 'x"' + raw.hex() + '"'
    value = value.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t")
    return '"' + value + '"'


STRING = re.compile(r'(x?)"((?:[^"\\]|\\.)*)"')
ESCAPES = {"n": "\n", "r": "\r", "t": "\t", "\\": "\\", '"': '"'}


def unquote(token):
    match = STRING.fullmatch(token)
    if not match:
        raise ValueError(f"bad string literal: {token}")
    if match.group(1):
        return bytes.fromhex(match.group(2))
    return re.sub(r"\\(.)", lambda m: ESCAPES[m.group(1)], match.group(2)).encode(scf.ENCODING)


def constant_text(tag, value):
    if tag in scf.TAG_BARE:
        return scf.TAG_BARE[tag]
    if tag == scf.TAG_INT:
        return f"int {value}"
    if tag == scf.TAG_FLOAT:
        return f"float 0x{value[::-1].hex()} ; {struct.unpack('<f', value)[0]!r}"
    if tag == scf.TAG_ARRAY:
        return "array [ " + " , ".join(constant_text(*item).split(" ; ")[0] for item in value) + " ]"
    return f"{scf.TAG_NAMES[tag]} {quote(value)}"


TOKEN = re.compile(r'x?"(?:[^"\\]|\\.)*"|\[|\]|,|[^\s\[\],]+')


def parse_constant(tokens):
    """Consume one constant from a token list (reversed, used as a stack)."""
    kind = tokens.pop()
    for tag, name in scf.TAG_BARE.items():
        if kind == name:
            return tag, None
    if kind == "int":
        return scf.TAG_INT, int(tokens.pop())
    if kind == "float":
        return scf.TAG_FLOAT, bytes.fromhex(tokens.pop()[2:])[::-1]
    if kind == "array":
        items = []
        assert tokens.pop() == "["
        while tokens[-1] != "]":
            items.append(parse_constant(tokens))
            if tokens[-1] == ",":
                tokens.pop()
        tokens.pop()
        return scf.TAG_ARRAY, items
    tag = {name: tag for tag, name in scf.TAG_NAMES.items()}[kind]
    return tag, unquote(tokens.pop())


def to_text(cls):
    lines = [f"class {quote(cls['name'])} : {quote(cls['parent'])} flags={cls['flags']}"]
    for key, word in (("instance_vars", "ivar"), ("class_vars", "cvar")):
        for slot, name in cls[key]:
            lines.append(f"{word} {slot} {quote(name)}")
    for i, (tag, value) in enumerate(cls["constants"]):
        lines.append(f"const {constant_text(tag, value)}    ; #{i}")
    for key, word in (("methods", "method"), ("class_methods", "classmethod")):
        for method in cls[key]:
            lines.append("")
            lines.append(f"{word} {quote(method['name'])} args={method['args']}")
            for offset, mnemonic, operands in disassemble(method["code"]):
                text = f"  {offset:04x}: {mnemonic}"
                if operands:
                    kinds = OPCODES[mnemonic][1]
                    text += " " + " ".join(f"@{v:04x}" if k[0] == "j" else str(v) for k, v in zip(kinds, operands))
                if mnemonic in CONSTANT_OPS:
                    index = operands[CONSTANT_OPS[mnemonic]]
                    if index < len(cls["constants"]):
                        text += "    ; " + constant_text(*cls["constants"][index]).split(" ; ")[0]
                lines.append(text)
            lines.append("end")
    return "\n".join(lines) + "\n"


def strip_comment(line):
    """Remove a trailing '; comment' that is not inside a string literal."""
    in_string = escaped = False
    for i, c in enumerate(line):
        if in_string:
            if escaped:
                escaped = False
            elif c == "\\":
                escaped = True
            elif c == '"':
                in_string = False
        elif c == '"':
            in_string = True
        elif c == ";":
            return line[:i]
    return line


def from_text(text):
    cls = {"instance_vars": [], "class_vars": [], "methods": [], "class_methods": [], "constants": []}
    current = None
    for number, line in enumerate(text.split("\n"), 1):
        line = strip_comment(line).strip()
        if not line:
            continue
        tokens = TOKEN.findall(line)
        word = tokens[0]
        try:
            if current is not None and word != "end":
                mnemonic = tokens[1]
                kinds = OPCODES[mnemonic][1]
                operands = [int(t[1:], 16) if k[0] == "j" else int(t) for k, t in zip(kinds, tokens[2:])]
                current.append((mnemonic, operands))
            elif word == "class":
                cls["name"], cls["parent"] = unquote(tokens[1]), unquote(tokens[3])
                cls["flags"] = int(tokens[4].split("=")[1])
            elif word in ("ivar", "cvar"):
                cls["instance_vars" if word == "ivar" else "class_vars"].append((int(tokens[1]), unquote(tokens[2])))
            elif word == "const":
                cls["constants"].append(parse_constant(tokens[:0:-1]))
            elif word in ("method", "classmethod"):
                current = []
                method = {"name": unquote(tokens[1]), "args": int(tokens[2].split("=")[1]), "code": current}
                cls["methods" if word == "method" else "class_methods"].append(method)
            elif word == "end":
                method["code"] = assemble(current)
                current = None
            else:
                raise ValueError(f"unexpected '{word}'")
        except (IndexError, KeyError, ValueError) as error:
            raise ValueError(f"line {number}: {error}") from None
    return cls


def all_classes(path):
    """Yield (label, raw class bytes) from an ARC archive or the system image."""
    if path.upper().endswith(".IMG"):
        for cls in scf.read_image(scf.load_image(path)):
            yield scf.text(cls["name"]), scf.build(cls)
    elif path.upper().endswith(".CPK"):  # the PS Vita release's scenario pack: one named .scf per class
        from hokago.formats import cpk
        with open(path, "rb") as f:
            for offset, size, _, _, name in cpk.read_index(path):
                f.seek(offset)
                raw = f.read(size)
                yield name, cpk.decompress_crilayla(raw) if raw[:8] == b"CRILAYLA" else raw
    else:
        from hokago.enterbrain import arc
        with open(path, "rb") as f:
            for _, h, blob in arc.read_arc(f.read()):
                yield f"{h:08X}", blob


def main():
    cmd, path = sys.argv[1], sys.argv[2]
    if cmd == "dis":
        with open(path, "rb") as f:
            cls, _ = scf.parse(f.read())
        text = to_text(cls)
        if len(sys.argv) > 3:
            with open(sys.argv[3], "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
        else:
            sys.stdout.reconfigure(encoding="utf-8")
            sys.stdout.write(text)
    elif cmd == "asm":
        with open(path, encoding="utf-8") as f:
            cls = from_text(f.read())
        with open(sys.argv[3], "wb") as f:
            f.write(scf.build(cls))
    elif cmd == "verify":
        total = bad = methods = instructions = 0
        for label, blob in all_classes(path):
            total += 1
            try:
                cls, _ = scf.parse(blob)
                for key in ("methods", "class_methods"):
                    for method in cls[key]:
                        listing = disassemble(method["code"])
                        starts = {offset for offset, _, _ in listing} | {len(method["code"])}
                        for offset, mnemonic, operands in listing:
                            for kind, value in zip(OPCODES[mnemonic][1], operands):
                                if kind[0] == "j" and value not in starts:
                                    raise ValueError(f"jump into the middle of an instruction at {offset:#x}")
                        methods += 1
                        instructions += len(listing)
                if scf.build(from_text(to_text(cls))) != blob:
                    raise ValueError("text round trip differs")
            except ValueError as error:
                bad += 1
                if bad <= 10:
                    print(f"{label}: {error}")
        status = "FAILED" if bad else "OK"
        print(f"{status}: {total - bad} of {total} classes ({methods} methods, {instructions} instructions) "
              f"disassemble and reassemble identically")
        if bad:
            raise SystemExit(1)
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
