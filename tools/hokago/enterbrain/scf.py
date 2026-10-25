"""Read and write SCF files: compiled classes of the game's scripting language.

Usage:
  python -m hokago scf dump         <file.scf>
  python -m hokago scf verify       <SCENARIO.ARC>   (parse and rebuild every class, compare)
  python -m hokago scf verify-image <AMAGAMI.IMG>    (same for the system image)
  python -m hokago scf list-image   <AMAGAMI.IMG>

The container format is documented in docs/technical/scf-format.md. Bytecode is
kept as raw bytes here; decoding it is a separate step.
"""
import os
import struct
import sys

MAGIC = b"SCF\x1a"
VERSION = 4
FLAG_NATIVE = 0x08    # set on the 14 built-in classes of the system image
ENCODING = "cp932"

# Constant pool tags.
TAG_INT, TAG_FLOAT, TAG_STRING, TAG_CLASS, TAG_SYMBOL, TAG_ARRAY = 1, 3, 5, 6, 7, 8
# Tags 0, 2 and 4 carry no data. Their meaning is not confirmed (probably nil and the booleans).
TAG_BARE = {0: "tag0", 2: "tag2", 4: "tag4"}
TAG_NAMES = {**TAG_BARE, TAG_INT: "int", TAG_FLOAT: "float", TAG_STRING: "string", TAG_CLASS: "class",
             TAG_SYMBOL: "symbol", TAG_ARRAY: "array"}


class Reader:
    def __init__(self, data, pos=0):
        self.data, self.pos = data, pos

    def u8(self):
        self.pos += 1
        return self.data[self.pos - 1]

    def u16(self):
        self.pos += 2
        return struct.unpack_from("<H", self.data, self.pos - 2)[0]

    def raw(self, size):
        self.pos += size
        if self.pos > len(self.data):
            raise ValueError("read past the end of the file")
        return self.data[self.pos - size:self.pos]

    def string(self):
        return self.raw(self.u16())


def read_constant(r):
    """Return (tag, value). Floats stay as their 4 raw bytes so rebuilding is exact."""
    tag = r.u8()
    if tag in TAG_BARE:
        return tag, None
    if tag == TAG_INT:
        return tag, struct.unpack("<i", r.raw(4))[0]
    if tag == TAG_FLOAT:
        return tag, r.raw(4)
    if tag in (TAG_STRING, TAG_CLASS, TAG_SYMBOL):
        return tag, r.string()
    if tag == TAG_ARRAY:
        return tag, [read_constant(r) for _ in range(r.u16())]
    raise ValueError(f"unknown constant tag {tag} at {r.pos - 1:#x}")


def write_constant(tag, value):
    if tag in TAG_BARE:
        return bytes((tag,))
    if tag == TAG_INT:
        return bytes((tag,)) + struct.pack("<i", value)
    if tag == TAG_FLOAT:
        return bytes((tag,)) + value
    if tag == TAG_ARRAY:
        return bytes((tag,)) + struct.pack("<H", len(value)) + b"".join(write_constant(*item) for item in value)
    return bytes((tag,)) + struct.pack("<H", len(value)) + value


def show_constant(tag, value):
    if tag in TAG_BARE:
        return TAG_BARE[tag]
    if tag == TAG_INT:
        return str(value)
    if tag == TAG_FLOAT:
        return repr(struct.unpack("<f", value)[0])
    if tag == TAG_ARRAY:
        return "[" + ", ".join(show_constant(*item) for item in value) + "]"
    return f"{TAG_NAMES[tag]} {text(value)!r}"


def parse(data, pos=0):
    """Parse one class starting at pos. Returns (class dict, end position).

    Names and strings are kept as bytes (Shift-JIS) so rebuilding is exact.
    """
    r = Reader(data, pos)
    if r.raw(4) != MAGIC:
        raise ValueError("not an SCF file")
    flags, version = r.u8(), r.u8()
    if version != VERSION or flags not in (0, FLAG_NATIVE):
        raise ValueError(f"unexpected SCF version {version} / flags {flags:#x}")
    cls = {"flags": flags, "name": r.string(), "parent": r.string()}
    for key in ("instance_vars", "class_vars"):
        cls[key] = [(r.u8(), r.string()) for _ in range(r.u16())]
    for key in ("methods", "class_methods"):
        methods = []
        for _ in range(r.u16()):
            name, args = r.string(), r.u16()
            methods.append({"name": name, "args": args, "code": r.raw(r.u16())})
        cls[key] = methods
    cls["constants"] = [read_constant(r) for _ in range(r.u16())]
    return cls, r.pos


def build(cls):
    def string(value):
        return struct.pack("<H", len(value)) + value

    out = bytearray(MAGIC + bytes((cls["flags"], VERSION)) + string(cls["name"]) + string(cls["parent"]))
    for key in ("instance_vars", "class_vars"):
        out += struct.pack("<H", len(cls[key]))
        for slot, name in cls[key]:
            out += bytes((slot,)) + string(name)
    for key in ("methods", "class_methods"):
        out += struct.pack("<H", len(cls[key]))
        for method in cls[key]:
            out += string(method["name"]) + struct.pack("<HH", method["args"], len(method["code"])) + method["code"]
    out += struct.pack("<H", len(cls["constants"]))
    for tag, value in cls["constants"]:
        out += write_constant(tag, value)
    return bytes(out)


def read_image(image):
    """Split a decompressed system image into its classes, in stored order."""
    size, count = struct.unpack_from("<II", image, 0)
    if size != len(image):
        raise ValueError("image size field does not match")
    name_offsets = struct.unpack_from(f"<{count}I", image, 8)
    data_offsets = struct.unpack_from(f"<{count}I", image, 8 + 4 * count)
    classes = []
    for i in range(count):
        end = data_offsets[i + 1] if i + 1 < count else size
        cls, stop = parse(image, data_offsets[i])
        name = image[name_offsets[i]:image.index(b"\0", name_offsets[i])]
        if stop != end or cls["name"] != name:
            raise ValueError(f"image entry {i} is inconsistent")
        classes.append(cls)
    return classes


def build_image(classes):
    count = len(classes)
    names = [cls["name"] + b"\0" for cls in classes]
    blobs = [build(cls) for cls in classes]
    position = 8 + 8 * count
    name_offsets = []
    for name in names:
        name_offsets.append(position)
        position += len(name)
    data_offsets = []
    for blob in blobs:
        data_offsets.append(position)
        position += len(blob)
    return (struct.pack("<II", position, count) + struct.pack(f"<{count}I", *name_offsets)
            + struct.pack(f"<{count}I", *data_offsets) + b"".join(names) + b"".join(blobs))


def load_image(path):
    from hokago.enterbrain import lzss
    with open(path, "rb") as f:
        return lzss.unpack(f.read())


def text(value):
    return value.decode(ENCODING, "replace")


def dump(cls):
    native = " (native)" if cls["flags"] & FLAG_NATIVE else ""
    print(f"class {text(cls['name'])} : {text(cls['parent'])}{native}")
    for key, label in (("instance_vars", "instance variable"), ("class_vars", "class variable")):
        for slot, name in cls[key]:
            print(f"  {label} {slot}: {text(name)}")
    for key, label in (("methods", "method"), ("class_methods", "class method")):
        for method in cls[key]:
            print(f"  {label} {text(method['name'])} (args field {method['args']}), {len(method['code'])} bytes")
            print(f"    {method['code'].hex(' ')}")
    for i, (tag, value) in enumerate(cls["constants"]):
        print(f"  constant {i}: {show_constant(tag, value)}")


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    cmd, path = sys.argv[1], sys.argv[2]
    if cmd == "dump":
        with open(path, "rb") as f:
            data = f.read()
        cls, end = parse(data)
        dump(cls)
        if end != len(data):
            print(f"warning: {len(data) - end} bytes left after the class")
    elif cmd == "verify":
        from hokago.enterbrain import arc
        with open(path, "rb") as f:
            entries = arc.read_arc(f.read())
        bad = 0
        for _, h, blob in entries:
            try:
                cls, end = parse(blob)
                if end != len(blob) or build(cls) != blob:
                    raise ValueError("does not round-trip")
            except ValueError as error:
                bad += 1
                if bad <= 10:
                    print(f"{h:08X}: {error}")
        status = "FAILED" if bad else "OK"
        print(f"{status}: {len(entries) - bad} of {len(entries)} classes parse and rebuild identically")
        if bad:
            raise SystemExit(1)
    elif cmd in ("verify-image", "list-image"):
        image = load_image(path)
        classes = read_image(image)
        if cmd == "list-image":
            for cls in classes:
                native = "  native" if cls["flags"] & FLAG_NATIVE else ""
                print(f"{text(cls['name']):<32} : {text(cls['parent']):<24} "
                      f"{len(cls['methods']):>3} methods, {len(cls['class_methods']):>3} class methods{native}")
        elif build_image(classes) != image:
            raise SystemExit("MISMATCH: the rebuilt image differs from the original")
        else:
            print(f"OK: {len(classes)} classes; the system image parses and rebuilds identically")
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
