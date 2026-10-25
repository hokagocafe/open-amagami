"""`python -m hokago` lists the tools; `python -m hokago NAME ARGS...` runs one of them as if it were run on its own.

The tools sit in groups (formats, audio, imaging, art, enterbrain); the name is the file's name, whatever its group.
"""
import ast
import os
import runpy
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def tools():
    """{name: (group, one-line description)}, read from the files, so a new tool needs no registration."""
    found = {}
    for group in sorted(os.listdir(HERE)):
        folder = os.path.join(HERE, group)
        if not os.path.isdir(folder) or group.startswith("_"):
            continue
        for fname in sorted(os.listdir(folder)):
            if not fname.endswith(".py") or fname.startswith("_"):
                continue
            with open(os.path.join(folder, fname), encoding="utf-8") as f:
                doc = ast.get_docstring(ast.parse(f.read())) or ""
            found[fname[:-3]] = (group, doc.split("\n")[0])
    return found


def main():
    available = tools()
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help", "list"):
        print(__doc__)
        for group in sorted({g for g, _ in available.values()}):
            print(f"{group}:")
            for name, (g, text) in available.items():
                if g == group:
                    print(f"  {name:<18} {text}")
        return
    name = sys.argv[1]
    if name not in available:
        raise SystemExit(f"no tool called {name!r}; `python -m hokago list` shows them")
    sys.argv = [name] + sys.argv[2:]
    runpy.run_module(f"hokago.{available[name][0]}.{name}", run_name="__main__", alter_sys=True)


if __name__ == "__main__":
    main()
