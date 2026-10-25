"""Decompile SCF classes to readable source and compile that source back.

Usage:
  python -m hokago scf_src dec    <file.scf> [out.scfs]
  python -m hokago scf_src enc    <in.scfs> <out.scf>
  python -m hokago scf_src dec-all <out_dir>        (every class, into <out_dir>/image and /scenario)
  python -m hokago scf_src verify <SCENARIO.ARC | AMAGAMI.IMG>   (round trip of every class)

A method that cannot be expressed as structured code (or whose structured form
does not compile back to the same bytes) is written as an `asm { }` block in the
instruction syntax of scf_asm.py, so every class always round-trips exactly.
The language is described in docs/technical/scf-source.md.
"""
import os
import re
import struct
import sys

from hokago.enterbrain import scf       # noqa: E402
from hokago.enterbrain import scf_asm   # noqa: E402

KEYWORDS = {"if", "else", "while", "loop", "break", "continue", "return", "self", "nil", "true", "false",
            "caught", "super", "asm", "primitive", "class", "method", "classmethod", "ivar", "cvar", "const",
            "end", "locals", "args", "flags", "and", "or", "line", "nop1", "do", "until", "throw", "try", "catch"}

BINARY = {  # mnemonic -> (symbol, precedence)
    "or": ("or", 1), "and": ("and", 2),
    "eq_ref": ("==", 3), "ne_ref": ("!=", 3), "eq": ("eq", 3), "ne": ("ne", 3),
    "gt": (">", 3), "lt": ("<", 3), "ge": (">=", 3), "le": ("<=", 3),
    "bit_or": ("|", 4), "bit_xor": ("^", 5), "bit_and": ("&", 6),
    "add": ("+", 7), "sub": ("-", 7), "mul": ("*", 8), "div": ("/", 8), "mod": ("%", 8),
}
BINARY_BY_SYMBOL = {symbol: (mnemonic, prec) for mnemonic, (symbol, prec) in BINARY.items()}
UNARY = {"neg": "-", "bit_not": "~", "not": "!"}
UNARY_BY_SYMBOL = {symbol: mnemonic for mnemonic, symbol in UNARY.items()}
UNARY_PREC = 9


class Unstructured(Exception):
    pass


# ---------------------------------------------------------------------------
# Names
# ---------------------------------------------------------------------------

def decode(raw):
    value = raw.decode(scf.ENCODING)
    if value.encode(scf.ENCODING) != raw:
        raise UnicodeError
    return value


def is_identifier(text):
    return bool(re.fullmatch(r"[^\W\d]\w*", text)) and text not in KEYWORDS


def quote_with(raw, quote_char):
    try:
        value = decode(raw)
        if any(ord(c) < 0x20 and c not in "\n\t\r" for c in value):
            raise UnicodeError
    except UnicodeError:
        return "x" + quote_char + raw.hex() + quote_char
    value = (value.replace("\\", "\\\\").replace(quote_char, "\\" + quote_char)
             .replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t"))
    return quote_char + value + quote_char


def name_token(raw):
    """A name as it appears in source: bare if it is a plain identifier, quoted otherwise."""
    try:
        text = decode(raw)
        if is_identifier(text):
            return text
    except UnicodeError:
        pass
    return quote_with(raw, "'")


def ivar_table(cls, classes):
    """Return (slot -> name, name -> slot) over the class and its parents; ambiguous names are dropped."""
    names = {}
    chain, seen = cls, set()
    while chain is not None and id(chain) not in seen:
        seen.add(id(chain))
        for slot, name in chain["instance_vars"]:
            names.setdefault(slot, name)
        chain = classes.get(chain["parent"])
    counts = {}
    for name in names.values():
        counts[name] = counts.get(name, 0) + 1
    by_slot = {slot: name for slot, name in names.items() if counts[name] == 1 and is_name_clean(name)}
    return by_slot, {name: slot for slot, name in by_slot.items()}


def is_name_clean(raw):
    try:
        return is_identifier(decode(raw)) and not re.fullmatch(r"ivar\d+", decode(raw))
    except UnicodeError:
        return False


# ---------------------------------------------------------------------------
# Decompiler: bytecode to a tree
# ---------------------------------------------------------------------------

JUMP_SIZE = {"jump": 2, "jump16": 3, "jump_false": 2, "jump_false16": 3}


class Decompiler:
    def __init__(self, cls, method):
        self.cls = cls
        self.pool = cls["constants"]
        self.args = method["args"]
        code = method["code"]
        self.ins = scf_asm.disassemble(code)
        self.size = len(code)
        if not self.ins or self.ins[0][1] != "locals":
            raise Unstructured("no locals instruction")
        if self.ins[-1][1] != "ret":
            raise Unstructured("does not end in ret")
        self.locals = self.ins[0][2][0]
        self.index = {offset: i for i, (offset, _, _) in enumerate(self.ins)}
        self.index[self.size] = len(self.ins)
        # Loops: head offset -> index of the last backward unconditional jump to it.
        self.loops = {}
        for i, (offset, mnemonic, operands) in enumerate(self.ins):
            if mnemonic in ("jump", "jump16") and operands[0] <= offset:
                head = operands[0]
                if head not in self.index:
                    raise Unstructured("jump into an instruction")
                self.loops[head] = max(self.loops.get(head, -1), i)
        # Loops closed by a backward conditional jump (do ... until): head offset -> jump index.
        self.untils = {}
        for i, (offset, mnemonic, operands) in enumerate(self.ins):
            if mnemonic in ("jump_false", "jump_false16") and operands[0] <= offset:
                head = operands[0]
                if head not in self.index:
                    raise Unstructured("jump into an instruction")
                self.untils[head] = max(self.untils.get(head, -1), i)

    def end_offset(self, i):
        return self.ins[i][0] if i < len(self.ins) else self.size

    def constant(self, k):
        if k >= len(self.pool):
            raise Unstructured("constant index out of range")
        return self.pool[k]

    def run(self):
        return self.region(1, len(self.ins) - 1, None)

    # An expression is a tuple; see print_expr for the shapes.
    def expression(self, i, j):
        """Simulate straight-line instructions from i up to (excluding) the first jump or
        return at or after i, bounded by j. Returns (stack, index of the stopping instruction)."""
        stack = []
        while i < j:
            offset, mnemonic, ops = self.ins[i]
            if mnemonic in ("jump", "jump16", "jump_false", "jump_false16", "ret", "ret_top", "pop", "pop_ivar",
                            "pop_local", "pop_cvar", "pop_cvar16", "try", "end_try", "primitive", "rethrow"):
                break
            if mnemonic != "nop":      # padding in front of send16; the compiler puts it back
                self.step(stack, mnemonic, ops)
            i += 1
        return stack, i

    def step(self, stack, mnemonic, ops):
        def pop():
            if not stack:
                raise Unstructured("stack underflow")
            return stack.pop()

        if mnemonic == "push_int":
            stack.append(("int", ops[0]))
        elif mnemonic in ("push_const", "push_const16"):
            tag, value = self.constant(ops[0])
            # An integer constant prints as a literal; the compiler decides how to encode it.
            stack.append(("int", value) if tag == scf.TAG_INT else ("const", tag, value))
        elif mnemonic == "push_local":
            stack.append(("local", ops[0]))
        elif mnemonic == "push_ivar":
            stack.append(("ivar", ops[0]))
        elif mnemonic in ("push_cvar", "push_cvar16"):
            tag, value = self.constant(ops[0])
            if tag != scf.TAG_CLASS:
                raise Unstructured("class variable of a non-class constant")
            stack.append(("cvar", value, ops[1]))
        elif mnemonic in ("push_self", "push_nil", "push_true", "push_false", "push_caught"):
            stack.append((mnemonic[5:],))
        elif mnemonic in ("send", "send16", "send_super", "send_super16"):
            argc, k = ops
            tag, selector = self.constant(k)
            if tag != scf.TAG_SYMBOL:
                raise Unstructured("selector is not a symbol")
            args = [pop() for _ in range(argc)][::-1]
            receiver = pop()
            stack.append(("send", receiver, selector, args, "super" in mnemonic))
        elif mnemonic in BINARY:
            b = pop()
            a = pop()
            stack.append(("bin", mnemonic, a, b))
        elif mnemonic in UNARY:
            stack.append(("un", mnemonic, pop()))
        elif mnemonic == "index_get":
            b = pop()
            a = pop()
            stack.append(("index", a, b))
        elif mnemonic == "index_set":
            c = pop()
            b = pop()
            a = pop()
            stack.append(("indexset", a, b, c))
        elif mnemonic == "dup":
            top = pop()
            stack += [top, top]
        else:
            raise Unstructured(f"unsupported instruction {mnemonic}")

    def region(self, i, j, loop, want_condition=False):
        """Statements for instructions [i, j). loop = (head offset, exit offset) or None.

        With want_condition the block must end in a bare expression (the condition of a
        do ... until loop); the result is then (statements, condition)."""
        out = []
        while i < j:
            offset, mnemonic, ops = self.ins[i]
            head = loop[0] if loop else None
            closing = self.loops.get(offset, -1)
            until = self.untils.get(offset, -1)
            if offset != head and until >= 0 and until < j and until > closing:
                i = self.until_statement(out, i)
                continue
            if offset != head and closing >= 0 and closing < j:
                i = self.loop_statement(out, i, j)
                continue
            stack, k = self.expression(i, j)
            if k >= j:
                if want_condition and len(stack) == 1:
                    return out, stack[0]
                raise Unstructured("expression runs past the end of its block")
            offset, mnemonic, ops = self.ins[k]
            if mnemonic == "pop":
                if len(stack) != 1:
                    raise Unstructured("pop with a surprising stack")
                out.append(("expr", stack[0]))
                i = k + 1
            elif mnemonic in ("pop_ivar", "pop_local", "pop_cvar", "pop_cvar16"):
                if len(stack) != 1:
                    raise Unstructured("store with a surprising stack")
                if mnemonic == "pop_ivar":
                    out.append(("set_ivar", ops[0], stack[0]))
                elif mnemonic == "pop_local":
                    out.append(("set_local", ops[0], stack[0]))
                else:
                    tag, value = self.constant(ops[0])
                    if tag != scf.TAG_CLASS:
                        raise Unstructured("class variable of a non-class constant")
                    out.append(("set_cvar", value, ops[1], stack[0]))
                i = k + 1
            elif mnemonic in ("ret", "ret_top"):
                if mnemonic == "ret_top":
                    if len(stack) != 1:
                        raise Unstructured("return with a surprising stack")
                    out.append(("return", stack[0]))
                else:
                    if stack:
                        raise Unstructured("stack not empty at return")
                    out.append(("return", None))
                i = k + 1
            elif mnemonic == "try":
                if stack:
                    raise Unstructured("try with values on the stack")
                i = self.try_statement(out, k, j, loop)
            elif mnemonic == "rethrow":
                if len(stack) != 1:
                    raise Unstructured("throw with a surprising stack")
                out.append(("throw", stack[0]))
                i = k + 1
            elif mnemonic == "primitive":
                if stack:
                    raise Unstructured("primitive with values on the stack")
                out.append(("primitive", ops[0]))
                i = k + 1
            elif mnemonic in ("jump_false16",):
                if len(stack) != 1:
                    raise Unstructured("conditional with a surprising stack")
                i = self.conditional(out, stack[0], k, j, loop)
            elif mnemonic in ("jump", "jump16"):
                if stack:
                    raise Unstructured("jump with values on the stack")
                target = ops[0]
                if target > offset:
                    if loop and target == loop[1]:
                        out.append(("break",))
                    else:
                        raise Unstructured("forward jump that is not a break")
                elif loop and target == loop[0]:
                    out.append(("continue",))
                else:
                    raise Unstructured("backward jump that is not a continue")
                i = k + 1
            else:
                raise Unstructured(f"unsupported control instruction {mnemonic}")
        if want_condition:
            raise Unstructured("do-until body has no condition")
        return out

    def try_statement(self, out, k, j, loop):
        """`try @H  body  end_try  jump @END  H: handler  clear_caught  END:` as ("try", body, handler)."""
        handler_offset = self.ins[k][2][0]
        depth, e = 0, None
        for q in range(k + 1, j):
            if self.ins[q][1] == "try":
                depth += 1
            elif self.ins[q][1] == "end_try":
                if depth == 0:
                    e = q
                    break
                depth -= 1
        if e is None or e + 1 >= j or self.ins[e + 1][1] not in ("jump16",):
            raise Unstructured("try without the usual end")
        end_offset = self.ins[e + 1][2][0]
        if handler_offset not in self.index or end_offset not in self.index:
            raise Unstructured("handler outside the instructions")
        hi, ie = self.index[handler_offset], self.index[end_offset]
        if hi != e + 2 or ie > j or ie - 1 < hi or self.ins[ie - 1][1] != "clear_caught":
            raise Unstructured("handler of an unusual shape")
        body = self.region(k + 1, e, loop)
        handler = self.region(hi, ie - 1, loop)
        out.append(("try", body, handler))
        return ie

    def until_statement(self, out, i):
        head = self.ins[i][0]
        k = self.untils[head]
        inner = (head, self.end_offset(k + 1))
        body, cond = self.region(i, k, inner, want_condition=True)
        out.append(("until", cond, body))
        return k + 1

    def loop_statement(self, out, i, j):
        head = self.ins[i][0]
        b = self.loops[head]
        exit_offset = self.end_offset(b + 1)
        stack, k = self.expression(i, b)
        inner = (head, exit_offset)
        if (k < b and self.ins[k][1] == "jump_false16" and self.ins[k][2][0] == exit_offset and len(stack) == 1):
            body = self.region(k + 1, b, inner)
            out.append(("while", stack[0], body))
        else:
            out.append(("loop", self.region(i, b, inner)))
        return b + 1

    def conditional(self, out, cond, k, j, loop):
        offset, _, ops = self.ins[k]
        target = ops[0]
        if target <= offset or target not in self.index:
            raise Unstructured("backward or invalid conditional jump")
        kt = self.index[target]
        if kt > j:
            raise Unstructured("conditional jump leaves its block")
        then_end, else_part = kt, None
        last = self.ins[kt - 1] if kt - 1 > k else None
        if last and last[1] == "jump16" and last[2][0] > last[0]:
            m = last[2][0]
            if m >= target and m in self.index and self.index[m] <= j and not (loop and m == loop[1]):
                then_end = kt - 1
                else_part = self.region(kt, self.index[m], loop)
                next_i = self.index[m]
            else:
                next_i = kt
        else:
            next_i = kt
        body = self.region(k + 1, then_end, loop)
        out.append(("if", cond, body, else_part))
        return next_i


def decompile(cls, method):
    return Decompiler(cls, method).run()


# ---------------------------------------------------------------------------
# Printer
# ---------------------------------------------------------------------------

class Printer:
    def __init__(self, cls, classes):
        self.cls = cls
        self.by_slot, _ = ivar_table(cls, classes)

    def literal(self, tag, value):
        if tag == scf.TAG_INT:
            return str(value)
        if tag == scf.TAG_STRING:
            return quote_with(value, '"')
        if tag == scf.TAG_CLASS:
            return name_token(value)
        if tag == scf.TAG_SYMBOL:
            return "#" + name_token(value)
        if tag == scf.TAG_FLOAT:
            number = struct.unpack("<f", value)[0]
            text = repr(number)
            if struct.pack("<f", float(text)) == value and re.fullmatch(r"-?\d+\.\d+(e-?\d+)?", text):
                return text + "f"
            return f"float(x{struct.unpack('<I', value)[0]:08x})"
        if tag == scf.TAG_ARRAY:
            return "[" + ", ".join(self.literal(*item) for item in value) + "]"
        return f"tag{tag}()"

    def args(self, method):
        return method["args"]

    def expr(self, node, parent_prec=0, right=False, ctx=None):
        kind = node[0]
        if kind == "int":
            return str(node[1])
        if kind == "const":
            return self.literal(node[1], node[2])
        if kind == "local":
            n = node[1]
            return f"arg{n}" if n < ctx["args"] else f"loc{n - ctx['args']}"
        if kind == "ivar":
            name = self.by_slot.get(node[1])
            return "self." + (name_token(name) if name else f"ivar{node[1]}")
        if kind == "cvar":
            return f"{name_token(node[1])}.cvar{node[2]}"
        if kind in ("self", "nil", "true", "false", "caught"):
            return kind
        if kind == "send":
            receiver = self.expr(node[1], 10, ctx=ctx)
            if node[4]:
                receiver = "super" if node[1] == ("self",) else receiver + ".super"
            selector = name_token(node[2])
            return f"{receiver}.{selector}(" + ", ".join(self.expr(a, 0, ctx=ctx) for a in node[3]) + ")"
        if kind == "index":
            return f"{self.expr(node[1], 10, ctx=ctx)}[{self.expr(node[2], 0, ctx=ctx)}]"
        if kind == "indexset":
            text = (f"{self.expr(node[1], 10, ctx=ctx)}[{self.expr(node[2], 0, ctx=ctx)}] = "
                    f"{self.expr(node[3], 0, ctx=ctx)}")
            return f"({text})" if parent_prec else text
        if kind == "un":
            operand = node[2]
            text = UNARY[node[1]]
            if node[1] == "neg" and operand[0] == "int":
                return f"-({operand[1]})"
            return text + self.expr(operand, UNARY_PREC, ctx=ctx)
        if kind == "bin":
            symbol, prec = BINARY[node[1]]
            text = f"{self.expr(node[2], prec, ctx=ctx)} {symbol} {self.expr(node[3], prec, True, ctx=ctx)}"
            if prec < parent_prec or (right and prec == parent_prec):
                return f"({text})"
            return text
        raise ValueError(kind)

    def statements(self, stmts, depth, ctx):
        pad = "    " * depth
        lines = []
        for s in stmts:
            kind = s[0]
            if kind == "expr":
                lines.append(f"{pad}{self.expr(s[1], ctx=ctx)};")
            elif kind == "set_local":
                n = s[1]
                name = f"arg{n}" if n < ctx["args"] else f"loc{n - ctx['args']}"
                lines.append(f"{pad}{name} = {self.expr(s[2], ctx=ctx)};")
            elif kind == "set_ivar":
                name = self.by_slot.get(s[1])
                lines.append(f"{pad}self.{name_token(name) if name else f'ivar{s[1]}'} = {self.expr(s[2], ctx=ctx)};")
            elif kind == "set_cvar":
                lines.append(f"{pad}{name_token(s[1])}.cvar{s[2]} = {self.expr(s[3], ctx=ctx)};")
            elif kind == "return":
                lines.append(f"{pad}return;" if s[1] is None else f"{pad}return {self.expr(s[1], ctx=ctx)};")
            elif kind == "throw":
                lines.append(f"{pad}throw {self.expr(s[1], ctx=ctx)};")
            elif kind == "primitive":
                lines.append(f"{pad}primitive({s[1]});")
            elif kind == "break":
                lines.append(f"{pad}break;")
            elif kind == "continue":
                lines.append(f"{pad}continue;")
            elif kind == "if":
                lines.append(f"{pad}if ({self.expr(s[1], ctx=ctx)}) {{")
                lines += self.statements(s[2], depth + 1, ctx)
                if s[3] is not None:
                    lines.append(f"{pad}}} else {{")
                    lines += self.statements(s[3], depth + 1, ctx)
                lines.append(f"{pad}}}")
            elif kind == "while":
                lines.append(f"{pad}while ({self.expr(s[1], ctx=ctx)}) {{")
                lines += self.statements(s[2], depth + 1, ctx)
                lines.append(f"{pad}}}")
            elif kind == "until":
                lines.append(f"{pad}do {{")
                lines += self.statements(s[2], depth + 1, ctx)
                lines.append(f"{pad}}} until ({self.expr(s[1], ctx=ctx)});")
            elif kind == "loop":
                lines.append(f"{pad}loop {{")
                lines += self.statements(s[1], depth + 1, ctx)
                lines.append(f"{pad}}}")
            elif kind == "try":
                lines.append(f"{pad}try {{")
                lines += self.statements(s[1], depth + 1, ctx)
                lines.append(f"{pad}}} catch {{")
                lines += self.statements(s[2], depth + 1, ctx)
                lines.append(f"{pad}}}")
            else:
                raise ValueError(kind)
        return lines


# ---------------------------------------------------------------------------
# Compiler: tree to bytecode
# ---------------------------------------------------------------------------

class Compiler:
    def __init__(self, cls, args, locals_count):
        self.cls = cls
        self.pool = cls["constants"]
        self.args = args
        self.code = bytearray()
        self.emit("locals", locals_count)
        self.loops = []   # [head offset, [fixups for breaks]]

    def intern(self, tag, value):
        key = (tag, value)
        for i, item in enumerate(self.pool):
            if item == key:
                return i
        self.pool.append(key)
        return len(self.pool) - 1

    def emit(self, mnemonic, *operands):
        opcode, kinds = scf_asm.OPCODES[mnemonic]
        if mnemonic in ("send16", "send_super16") and len(self.code) % 2:
            self.code.append(scf_asm.OPCODES["nop"][0])
        start = len(self.code)
        self.code.append(opcode)
        for kind, value in zip(kinds, operands):
            self.code += struct.pack(scf_asm.FORMATS[kind], value - start if kind[0] == "j" else value)
        return start

    def forward(self, mnemonic):
        """Emit a forward jump with a placeholder; return the position to patch."""
        return self.emit(mnemonic, len(self.code))

    def patch(self, position, target=None):
        target = len(self.code) if target is None else target
        struct.pack_into("<h", self.code, position + 1, target - position)

    def backward(self, target):
        distance = target - len(self.code)
        self.emit("jump" if distance >= -128 else "jump16", target)

    def push_constant(self, tag, value):
        k = self.intern(tag, value)
        self.emit("push_const" if k < 256 else "push_const16", k)

    def expr(self, node):
        kind = node[0]
        if kind == "int":
            if -128 <= node[1] <= 127:
                self.emit("push_int", node[1])
            else:
                self.push_constant(scf.TAG_INT, node[1])
        elif kind == "const":
            self.push_constant(node[1], node[2])
        elif kind == "local":
            self.emit("push_local", node[1])
        elif kind == "ivar":
            self.emit("push_ivar", node[1])
        elif kind == "cvar":
            k = self.intern(scf.TAG_CLASS, node[1])
            self.emit("push_cvar" if k < 256 else "push_cvar16", k, node[2])
        elif kind in ("self", "nil", "true", "false", "caught"):
            self.emit("push_" + kind)
        elif kind == "send":
            self.expr(node[1])
            for argument in node[3]:
                self.expr(argument)
            k = self.intern(scf.TAG_SYMBOL, node[2])
            base = "send_super" if node[4] else "send"
            self.emit(base if k < 128 else base + "16", len(node[3]), k)   # a signed byte
        elif kind == "index":
            self.expr(node[1])
            self.expr(node[2])
            self.emit("index_get")
        elif kind == "indexset":
            for part in node[1:]:
                self.expr(part)
            self.emit("index_set")
        elif kind == "un":
            self.expr(node[2])
            self.emit(node[1])
        elif kind == "bin":
            self.expr(node[2])
            self.expr(node[3])
            self.emit(node[1])
        else:
            raise ValueError(kind)

    def statements(self, stmts):
        for s in stmts:
            kind = s[0]
            if kind == "expr":
                self.expr(s[1])
                self.emit("pop")
            elif kind == "set_local":
                self.expr(s[2])
                self.emit("pop_local", s[1])
            elif kind == "set_ivar":
                self.expr(s[2])
                self.emit("pop_ivar", s[1])
            elif kind == "set_cvar":
                self.expr(s[3])
                k = self.intern(scf.TAG_CLASS, s[1])
                self.emit("pop_cvar" if k < 256 else "pop_cvar16", k, s[2])
            elif kind == "return":
                if s[1] is None:
                    self.emit("ret")
                else:
                    self.expr(s[1])
                    self.emit("ret_top")
            elif kind == "throw":
                self.expr(s[1])
                self.emit("rethrow")
            elif kind == "primitive":
                self.emit("primitive", s[1])
            elif kind == "break":
                self.loops[-1][1].append(self.forward("jump16"))
            elif kind == "continue":
                self.backward(self.loops[-1][0])
            elif kind == "if":
                self.expr(s[1])
                jf = self.forward("jump_false16")
                self.statements(s[2])
                if s[3] is None:
                    self.patch(jf)
                else:
                    jump = self.forward("jump16")
                    self.patch(jf)
                    self.statements(s[3])
                    self.patch(jump)
            elif kind == "while":
                head = len(self.code)
                self.expr(s[1])
                jf = self.forward("jump_false16")
                self.loops.append([head, []])
                self.statements(s[2])
                self.backward(head)
                self.patch(jf)
                for position in self.loops.pop()[1]:
                    self.patch(position)
            elif kind == "until":
                head = len(self.code)
                self.loops.append([head, []])
                self.statements(s[2])
                self.expr(s[1])
                distance = head - len(self.code)
                self.emit("jump_false" if distance >= -128 else "jump_false16", head)
                for position in self.loops.pop()[1]:
                    self.patch(position)
            elif kind == "loop":
                head = len(self.code)
                self.loops.append([head, []])
                self.statements(s[1])
                self.backward(head)
                for position in self.loops.pop()[1]:
                    self.patch(position)
            elif kind == "try":
                handler = self.forward("try")
                self.statements(s[1])
                self.emit("end_try")
                over = self.forward("jump16")
                self.patch(handler)
                self.statements(s[2])
                self.emit("clear_caught")
                self.patch(over)
            else:
                raise ValueError(kind)

    def finish(self):
        self.emit("ret")
        return bytes(self.code)


def compile_tree(cls, args, locals_count, tree):
    compiler = Compiler(cls, args, locals_count)
    compiler.statements(tree)
    return compiler.finish()


# ---------------------------------------------------------------------------
# Parser: source text to a tree
# ---------------------------------------------------------------------------

TOKEN = re.compile(r"""
    (?P<space>\s+|//[^\n]*)
  | (?P<hex>x"[0-9a-fA-F]*")
  | (?P<hexq>x'[0-9a-fA-F]*')
  | (?P<string>"(?:[^"\\]|\\.)*")
  | (?P<quoted>'(?:[^'\\]|\\.)*')
  | (?P<float>-?\d+\.\d+(?:e-?\d+)?f)
  | (?P<number>\d+)
  | (?P<id>[^\W\d]\w*)
  | (?P<op><=|>=|==|!=|[-+*/%&|^~!<>=(){}\[\].,;#:@])
""", re.VERBOSE)
ESCAPES = {"n": "\n", "r": "\r", "t": "\t", "\\": "\\", '"': '"', "'": "'"}


def unescape(body):
    return re.sub(r"\\(.)", lambda m: ESCAPES[m.group(1)], body)


def tokenize(text):
    tokens, pos = [], 0
    while pos < len(text):
        match = TOKEN.match(text, pos)
        if not match:
            raise ValueError(f"unexpected character {text[pos]!r} at offset {pos}")
        pos = match.end()
        kind = match.lastgroup
        value = match.group()
        if kind == "space":
            continue
        if kind in ("hex", "hexq"):
            tokens.append(("name" if kind == "hexq" else "string", bytes.fromhex(value[2:-1]), True))
        elif kind == "string":
            tokens.append(("string", unescape(value[1:-1]).encode(scf.ENCODING), True))
        elif kind == "quoted":
            tokens.append(("name", unescape(value[1:-1]).encode(scf.ENCODING), True))
        elif kind == "id":
            tokens.append(("name", value.encode(scf.ENCODING), False))
        elif kind == "number":
            tokens.append(("number", int(value), False))
        elif kind == "float":
            tokens.append(("float", value, False))
        else:
            tokens.append(("op", value, False))
    return tokens


class Parser:
    def __init__(self, tokens, cls, name_to_slot, args):
        self.tokens, self.pos = tokens, 0
        self.cls, self.name_to_slot, self.args = cls, name_to_slot, args

    def peek(self, ahead=0):
        i = self.pos + ahead
        return self.tokens[i] if i < len(self.tokens) else ("eof", None, False)

    def next(self):
        token = self.peek()
        self.pos += 1
        return token

    def is_op(self, text, ahead=0):
        token = self.peek(ahead)
        return token[0] == "op" and token[1] == text

    def is_word(self, text, ahead=0):
        token = self.peek(ahead)
        return token[0] == "name" and not token[2] and token[1] == text.encode()

    def expect_op(self, text):
        token = self.next()
        if token[0] != "op" or token[1] != text:
            raise ValueError(f"expected '{text}' but found {token[1]!r}")

    def block(self):
        self.expect_op("{")
        out = []
        while not self.is_op("}"):
            out.append(self.statement())
        self.expect_op("}")
        return out

    def statement(self):
        if self.is_word("if"):
            self.next()
            self.expect_op("(")
            cond = self.expression()
            self.expect_op(")")
            then = self.block()
            other = None
            if self.is_word("else"):
                self.next()
                other = self.block()
            return ("if", cond, then, other)
        if self.is_word("while"):
            self.next()
            self.expect_op("(")
            cond = self.expression()
            self.expect_op(")")
            return ("while", cond, self.block())
        if self.is_word("loop"):
            self.next()
            return ("loop", self.block())
        if self.is_word("try"):
            self.next()
            body = self.block()
            if not self.is_word("catch"):
                raise ValueError("expected 'catch'")
            self.next()
            return ("try", body, self.block())
        if self.is_word("do"):
            self.next()
            body = self.block()
            if not self.is_word("until"):
                raise ValueError("expected 'until'")
            self.next()
            self.expect_op("(")
            cond = self.expression()
            self.expect_op(")")
            self.expect_op(";")
            return ("until", cond, body)
        if self.is_word("throw"):
            self.next()
            value = self.expression()
            self.expect_op(";")
            return ("throw", value)
        if self.is_word("primitive"):
            self.next()
            self.expect_op("(")
            number = self.next()[1]
            self.expect_op(")")
            self.expect_op(";")
            return ("primitive", number)
        if self.is_word("break"):
            self.next()
            self.expect_op(";")
            return ("break",)
        if self.is_word("continue"):
            self.next()
            self.expect_op(";")
            return ("continue",)
        if self.is_word("return"):
            self.next()
            if self.is_op(";"):
                self.next()
                return ("return", None)
            value = self.expression()
            self.expect_op(";")
            return ("return", value)
        target = self.expression()
        if self.is_op("="):
            self.next()
            value = self.expression()
            self.expect_op(";")
            return self.assignment(target, value)
        self.expect_op(";")
        return ("expr", target)

    def assignment(self, target, value):
        kind = target[0]
        if kind == "local":
            return ("set_local", target[1], value)
        if kind == "ivar":
            return ("set_ivar", target[1], value)
        if kind == "cvar":
            return ("set_cvar", target[1], target[2], value)
        if kind == "index":
            return ("expr", ("indexset", target[1], target[2], value))
        raise ValueError("invalid assignment target")

    def expression(self, min_prec=1):
        left = self.unary()
        while True:
            token = self.peek()
            symbol = token[1].decode() if token[0] == "name" and not token[2] else token[1]
            if token[0] not in ("op", "name") or symbol not in BINARY_BY_SYMBOL:
                return left
            mnemonic, prec = BINARY_BY_SYMBOL[symbol]
            if prec < min_prec:
                return left
            self.next()
            right = self.expression(prec + 1)
            left = ("bin", mnemonic, left, right)

    def unary(self):
        token = self.peek()
        if token[0] == "op" and token[1] in UNARY_BY_SYMBOL:
            self.next()
            if token[1] == "-" and self.peek()[0] == "number":
                return ("int", -self.next()[1])
            return ("un", UNARY_BY_SYMBOL[token[1]], self.unary_operand())
        return self.postfix(self.primary())

    def unary_operand(self):
        return self.unary()

    def primary(self):
        token = self.next()
        kind, value, quoted = token
        if kind == "number":
            return ("int", value)
        if kind == "float":
            return ("const", scf.TAG_FLOAT, struct.pack("<f", float(value[:-1])))
        if kind == "string":
            return ("const", scf.TAG_STRING, value)
        if kind == "op":
            if value == "(":
                inner = self.expression()
                self.expect_op(")")
                if inner[0] == "int":
                    return ("paren", inner)
                return inner
            if value == "[":
                items = []
                while not self.is_op("]"):
                    node = self.expression()
                    items.append(self.constant_of(node))
                    if self.is_op(","):
                        self.next()
                self.expect_op("]")
                return ("const", scf.TAG_ARRAY, items)
            if value == "#":
                name = self.next()
                return ("const", scf.TAG_SYMBOL, name[1])
            raise ValueError(f"unexpected '{value}'")
        if kind == "name" and not quoted:
            word = value.decode()
            if word in ("self", "nil", "true", "false", "caught"):
                return (word,)
            if word == "super":
                return ("super",)
            if word == "float" and self.is_op("("):
                self.next()
                digits = self.next()[1]
                self.expect_op(")")
                return ("const", scf.TAG_FLOAT, struct.pack("<I", int(digits[1:], 16)))
            match = re.fullmatch(r"(arg|loc)(\d+)", word)
            if match:
                n = int(match.group(2))
                return ("local", n if match.group(1) == "arg" else n + self.args)
            match = re.fullmatch(r"tag(\d)", word)
            if match and self.is_op("("):
                self.next()
                self.expect_op(")")
                return ("const", int(match.group(1)), None)
        if kind == "name":
            return ("const", scf.TAG_CLASS, value)
        raise ValueError(f"unexpected token {value!r}")

    def constant_of(self, node):
        if node[0] == "int":
            return (scf.TAG_INT, node[1])
        if node[0] == "const":
            return (node[1], node[2])
        raise ValueError("array literals can only hold constants")

    def postfix(self, node):
        while True:
            if self.is_op("."):
                self.next()
                token = self.next()
                if token[0] != "name":
                    raise ValueError("expected a name after '.'")
                name = token[1]
                if self.is_op("("):
                    self.next()
                    args = []
                    while not self.is_op(")"):
                        args.append(self.expression())
                        if self.is_op(","):
                            self.next()
                    self.expect_op(")")
                    if node == ("super",):
                        node = ("send", ("self",), name, args, True)
                    else:
                        node = ("send", node, name, args, False)
                elif node == ("self",):
                    match = re.fullmatch(rb"ivar(\d+)", name)
                    if match and not token[2]:
                        node = ("ivar", int(match.group(1)))
                    elif name in self.name_to_slot:
                        node = ("ivar", self.name_to_slot[name])
                    else:
                        raise ValueError(f"unknown instance variable {name!r}")
                elif node[0] == "const" and node[1] == scf.TAG_CLASS:
                    match = re.fullmatch(rb"cvar(\d+)", name)
                    if not match:
                        raise ValueError("expected cvarN")
                    node = ("cvar", node[2], int(match.group(1)))
                else:
                    raise ValueError("invalid member access")
            elif self.is_op("["):
                self.next()
                index = self.expression()
                self.expect_op("]")
                node = ("index", node, index)
            else:
                return node


# ---------------------------------------------------------------------------
# Whole classes
# ---------------------------------------------------------------------------

def unparen(node):
    """Parentheses around an integer literal exist only to defeat negative-number folding."""
    if isinstance(node, tuple):
        if node and node[0] == "paren":
            return ("int", node[1][1])
        return tuple(unparen(part) for part in node)
    if isinstance(node, list):
        return [unparen(part) for part in node]
    return node


def method_to_source(cls, method, printer, classes):
    """Return the lines of one method, structured when possible, else as an asm block."""
    header = f'{{kind}} {quote_with(method["name"], chr(34))} args={method["args"]}'
    try:
        tree = decompile(cls, method)
        probe = {"constants": list(cls["constants"])}
        locals_count = Decompiler(cls, method).locals
        rebuilt = compile_tree(probe, method["args"], locals_count, tree)
        if rebuilt != method["code"] or probe["constants"] != cls["constants"]:
            raise Unstructured("does not compile back to the same bytes")
        ctx = {"args": method["args"]}
        body = printer.statements(tree, 1, ctx)
        _, name_to_slot = ivar_table(cls, classes)
        # The text has to parse back to the same tree.
        parser = Parser(tokenize("{\n" + "\n".join(body) + "\n}"), cls, name_to_slot, method["args"])
        if unparen(parser.block()) != tree:
            raise Unstructured("source does not parse back to the same tree")
        return [header + f" locals={locals_count} {{"] + body + ["}"], True
    except (Unstructured, ValueError, UnicodeError, KeyError, IndexError, struct.error):
        listing = scf_asm.disassemble(method["code"])
        lines = [header + " asm {"]
        for offset, mnemonic, operands in listing:
            text = f"    {offset:04x}: {mnemonic}"
            if operands:
                kinds = scf_asm.OPCODES[mnemonic][1]
                text += " " + " ".join(f"@{v:04x}" if k[0] == "j" else str(v) for k, v in zip(kinds, operands))
            lines.append(text)
        lines.append("}")
        return lines, False


def class_to_source(cls, classes):
    printer = Printer(cls, classes)
    lines = [f"class {quote_with(cls['name'], chr(34))} : {quote_with(cls['parent'], chr(34))} flags={cls['flags']}"]
    for key, word in (("instance_vars", "ivar"), ("class_vars", "cvar")):
        for slot, name in cls[key]:
            lines.append(f"{word} {slot} {quote_with(name, chr(34))}")
    for tag, value in cls["constants"]:
        lines.append(f"const {scf_asm.constant_text(tag, value)}")
    structured = total = 0
    for key, kind in (("methods", "method"), ("class_methods", "classmethod")):
        for method in cls[key]:
            body, ok = method_to_source(cls, method, printer, classes)
            body[0] = body[0].replace("{kind}", kind, 1)
            lines.append("")
            lines += body
            structured += ok
            total += 1
    return "\n".join(lines) + "\n", structured, total


def build_classes(paths):
    classes = {}
    for path in paths:
        for _label, blob in scf_asm.all_classes(path):
            cls, _ = scf.parse(blob)
            classes.setdefault(cls["name"], cls)
    return classes


def class_from_source(text, classes):
    """Parse source text back into a class dict."""
    cls = {"instance_vars": [], "class_vars": [], "methods": [], "class_methods": [], "constants": []}
    lines = text.split("\n")
    i = 0
    pending = []
    while i < len(lines):
        line = scf_asm.strip_comment(lines[i]).strip()
        i += 1
        if not line:
            continue
        tokens = scf_asm.TOKEN.findall(line)
        word = tokens[0]
        if word == "class":
            cls["name"], cls["parent"] = scf_asm.unquote(tokens[1]), scf_asm.unquote(tokens[3])
            cls["flags"] = int(tokens[4].split("=")[1])
        elif word in ("ivar", "cvar"):
            cls["instance_vars" if word == "ivar" else "class_vars"].append((int(tokens[1]), scf_asm.unquote(tokens[2])))
        elif word == "const":
            cls["constants"].append(scf_asm.parse_constant(tokens[:0:-1]))
        elif word in ("method", "classmethod"):
            body = []
            while lines[i] != "}":
                body.append(lines[i])
                i += 1
            i += 1
            pending.append((word, line, body))
    _, name_to_slot = ivar_table(cls, classes)
    for word, header, body in pending:
        name = scf_asm.unquote(re.search(r'x?"(?:[^"\\]|\\.)*"', header).group())
        args = int(re.search(r"args=(\d+)", header).group(1))
        entry = {"name": name, "args": args}
        if header.rstrip().endswith("asm {"):
            ins = []
            for part in body:
                part = scf_asm.strip_comment(part).strip()
                if not part:
                    continue
                tokens = scf_asm.TOKEN.findall(part)
                mnemonic = tokens[1]
                kinds = scf_asm.OPCODES[mnemonic][1]
                ins.append((mnemonic, [int(t[1:], 16) if k[0] == "j" else int(t) for k, t in zip(kinds, tokens[2:])]))
            entry["code"] = scf_asm.assemble(ins)
        else:
            locals_count = int(re.search(r"locals=(\d+)", header).group(1))
            parser = Parser(tokenize("{\n" + "\n".join(body) + "\n}"), cls, name_to_slot, args)
            tree = unparen(parser.block())
            entry["code"] = compile_tree(cls, args, locals_count, tree)
        cls["methods" if word == "method" else "class_methods"].append(entry)
    return cls


def main():
    cmd, path = sys.argv[1], sys.argv[2]
    if cmd == "dec":
        classes = build_classes(["extracted/AMAGAMI.IMG", "extracted/SCENARIO.ARC"])
        with open(path, "rb") as f:
            cls, _ = scf.parse(f.read())
        text, ok, total = class_to_source(cls, classes)
        if len(sys.argv) > 3:
            with open(sys.argv[3], "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
        else:
            sys.stdout.reconfigure(encoding="utf-8")
            sys.stdout.write(text)
        print(f"// {ok} of {total} methods structured", file=sys.stderr)
    elif cmd == "dec-all":
        # Decompile every class of the game into <out>/image and <out>/scenario.
        sources = {"image": "extracted/AMAGAMI.IMG", "scenario": "extracted/SCENARIO.ARC"}
        classes = build_classes(list(sources.values()))
        for folder, source in sources.items():
            os.makedirs(os.path.join(path, folder), exist_ok=True)
            count = 0
            for _label, blob in scf_asm.all_classes(source):
                cls, _ = scf.parse(blob)
                text, _ok, _total = class_to_source(cls, classes)
                name = re.sub(r'[\/:*?"<>|]', "_", scf.text(cls["name"]))
                with open(os.path.join(path, folder, name + ".scfs"), "w", encoding="utf-8", newline=chr(10)) as f:
                    f.write(text)
                count += 1
            print(f"{folder}: {count} classes")
    elif cmd == "enc":
        classes = build_classes(["extracted/AMAGAMI.IMG", "extracted/SCENARIO.ARC"])
        with open(path, encoding="utf-8") as f:
            cls = class_from_source(f.read(), classes)
        with open(sys.argv[3], "wb") as f:
            f.write(scf.build(cls))
    elif cmd == "verify":
        classes = build_classes(["extracted/AMAGAMI.IMG", "extracted/SCENARIO.ARC"])
        total = bad = ok_methods = all_methods = 0
        reasons = {}
        for label, blob in scf_asm.all_classes(path):
            total += 1
            cls, _ = scf.parse(blob)
            text, ok, count = class_to_source(cls, classes)
            ok_methods += ok
            all_methods += count
            try:
                again = scf.build(class_from_source(text, classes))
                if again != blob:
                    raise ValueError("bytes differ")
            except Exception as error:      # noqa: BLE001 - report every kind of failure
                bad += 1
                if bad <= 8:
                    print(f"{label}: {type(error).__name__}: {error}")
        status = "FAILED" if bad else "OK"
        print(f"{status}: {total - bad} of {total} classes round-trip; "
              f"{ok_methods} of {all_methods} methods ({100 * ok_methods / max(all_methods, 1):.1f}%) are structured")
        if bad:
            raise SystemExit(1)
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
