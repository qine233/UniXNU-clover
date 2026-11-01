#!/opt/homebrew/bin/python3.13
"""Run the real kernel compile flags over one file and fail on the whole stream.

usage: check-clover-xnu-syntax.py osfmk/kern/task.c ...

The argument names which translation unit to check; the file compiled is the one in the build tree at
that path, because the flags, header search paths and defines only make sense against it. Round the
patch onto the tree first -- a syntax check of a file that is not what will be compiled is decoration.

Round269 shipped with five errors in task.c that my own syntax check had called clean. The check was
reading the same compile_commands.json entry and the same clang arguments, then printing only the
first forty diagnostics -- and XNU's task.c carries a thousand pre-existing
-Wcast-function-type-strict warnings, so the first forty lines were all warnings from line 241 onward
and nothing from line 12044 was ever shown. "EXIT=0 and no diagnostics after 11940" was a reading of a
truncated list, not of the file.

So this tool has one job the ad-hoc version failed: errors are never truncated, and the exit code comes
from the compiler, not from what happened to fit on screen. Warnings are summarised by line so a round
can see whether it added any, and an implicit function declaration is treated as an error even when the
compiler lets it through as a warning, because the kernel's BSD helpers (proc_find, proc_task, proc_rele)
are not declared in any header task.c includes -- every earlier census declares them inside the function
that calls them, and the round that forgot lost proc_find's pointer return into an int.
"""
import json, os, re, subprocess, sys

TREE = os.environ.get("CLOVER_XNU_TREE",
                      "/Users/zhouzhou/UniXNU/out/clover/tree/.cache/xnu-work")
CC = os.path.join(os.environ.get("CLOVER_XNU_BUILD",
                                 "/Users/zhouzhou/UniXNU/out/clover/tree/build/xnu/ios-obj/"
                                 "DEVELOPMENT_ARM64_BCM2837"),
                  "compile_commands.json")

# The defect classes that mean "this will not link, or will run wrong", regardless of whether the
# compiler chose to warn about them.
HARD = ("error:", "implicit function declaration", "undeclared",
        "incomplete type", "out-of-scope", "incompatible function pointer",
        "conflicting types", "expects arguments", "control reaches end")


def entry_for(path):
    with open(CC) as fh:
        entries = json.load(fh)
    base = os.path.basename(path)
    for e in entries:
        if e.get("file", "").endswith("/" + base) or os.path.realpath(e.get("file", "")) == \
                os.path.realpath(path):
            if e.get("arguments"):
                return e
    sys.exit("no compile_commands entry for %s: the tree was not configured" % base)


def run(path):
    if not os.path.isfile(CC):
        sys.exit("no %s: run a kernel build once before syntax-checking" % CC)
    real = os.path.join(TREE, os.path.relpath(path, TREE)) if path.startswith(TREE) else path
    e = entry_for(real)
    clean, skip = [], False
    for a in e["arguments"]:
        if skip:
            skip = False
            continue
        if a == "-o":
            skip = True
            continue
        if a == "-c":
            continue
        clean.append(a)
    clean.insert(1, "-fsyntax-only")
    r = subprocess.run(clean, cwd=e["directory"], capture_output=True, text=True)
    out = (r.stdout + r.stderr).splitlines()
    name = os.path.basename(e["file"])
    diag = []
    for ln in out:
        m = re.match(r"^(.*?):(\d+):\d*: (error|warning|note|fatal error): (.*)$", ln)
        if m and os.path.basename(m.group(1)) == name:
            diag.append((int(m.group(2)), m.group(3), m.group(4)))
    hard = [d for d in diag if d[1] in ("error", "fatal error")
            or any(k in d[2] for k in HARD)]
    print("%s: exit=%d diagnostics=%d hard=%d" % (name, r.returncode, len(diag), len(hard)))
    for ln, kind, msg in hard:
        print("  HARD %s:%d %s" % (name, ln, msg[:220]))
    if hard:
        print("  (all of these stop the build or silently truncate a pointer)")
    return 1 if (hard or r.returncode != 0) else 0


rc = 0
for p in sys.argv[1:]:
    rc |= run(p)
sys.exit(rc)
