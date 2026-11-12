#!/usr/bin/env python3
"""Replay selected XNU file diffs in a disposable Git repository.

No XNU checkout or download is needed: each selected file must be introduced by
an earlier new-file diff. This checks patch application, not compilation or
cross-file API dependencies. Exit status: 0 success, Git's status on apply
failure, 2 invalid input/missing single-file baseline. Byte-oriented extraction
preserves final newlines, including Git's no-newline marker.
"""

import argparse
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import tempfile

DEFAULT_FILES = ("bsd/dev/sweet_spmi.c", "iokit/Kernel/arm/SweetFramebuffer.cpp")
HUNK = re.compile(rb"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")


@dataclass
class FileDiff:
    old: str
    new: str
    data: bytes
    line: int


def header_path(line):
    value = line[4:].rstrip(b"\r\n").split(b"\t", 1)[0]
    if value == b"/dev/null":
        return "/dev/null"
    if not value.startswith((b"a/", b"b/")):
        raise ValueError("expected an unquoted a/ or b/ path")
    path = value[2:].decode("utf-8")
    if PurePosixPath(path).is_absolute() or ".." in PurePosixPath(path).parts:
        raise ValueError("unsafe diff path: " + path)
    return path


def file_diffs(data):
    """Extract exact unified file diffs, accepting Git and plain unified headers."""
    lines = data.splitlines(keepends=True)
    starts = [i for i in range(len(lines) - 1)
              if lines[i].startswith((b"--- a/", b"--- /dev/null"))
              and lines[i + 1].startswith((b"+++ b/", b"+++ /dev/null"))]
    for index, start in enumerate(starts):
        end = starts[index + 1] if index + 1 < len(starts) else len(lines)
        # A Git header belongs to the next file, never the preceding file.
        for pos in range(start + 2, end):
            if lines[pos].startswith(b"diff --git "):
                end = pos
                break
        yield FileDiff(header_path(lines[start]), header_path(lines[start + 1]),
                       b"".join(lines[start:end]), start + 1)


def check_hunks(diff):
    """Validate declared old/new counts (omitted counts mean one, not zero)."""
    lines = diff.data.splitlines(keepends=True)
    count = 0
    pos = 2
    while pos < len(lines):
        match = HUNK.match(lines[pos])
        if not match:
            if lines[pos].startswith(b"@@"):
                raise ValueError(f"line {diff.line + pos}: invalid hunk header")
            pos += 1
            continue
        count += 1
        expected = (int(match[2]) if match[2] is not None else 1,
                    int(match[4]) if match[4] is not None else 1)
        seen = [0, 0]
        header_line = diff.line + pos
        pos += 1
        while pos < len(lines):
            line = lines[pos]
            if line.startswith(b"@@"):
                break
            if line.startswith(b"\\ No newline at end of file"):
                pos += 1
                continue
            if line.startswith(b" "):
                seen[0] += 1
                seen[1] += 1
            elif line.startswith(b"-") and line.rstrip(b"\r\n") != b"-- ":
                seen[0] += 1
            elif line.startswith(b"+"):
                seen[1] += 1
            else:
                break  # prose/mail footer after a complete hunk
            pos += 1
        if tuple(seen) != expected:
            raise ValueError(f"line {header_line}: hunk count mismatch: "
                             f"declared old/new {expected}, actual {tuple(seen)}")
    if not count:
        raise ValueError(f"line {diff.line}: no unified hunks (binary/rename-only diffs unsupported)")


def run_git(repo, *args, data=None):
    return subprocess.run(["git", "-C", str(repo), *args], input=data,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def replay(args, selected, repo):
    initialized = set()
    applied = {path: 0 for path in selected}
    patches = sorted(args.patch_dir.glob("*.patch"))
    if not patches:
        raise ValueError(f"no .patch files in {args.patch_dir}")
    for patch in patches:
        if args.through and patch.name[:4].isdigit() and int(patch.name[:4]) > args.through:
            continue
        for diff in file_diffs(patch.read_bytes()):
            touched = {diff.old, diff.new} & selected
            if not touched:
                continue
            try:
                check_hunks(diff)
            except ValueError as exc:
                raise ValueError(f"{patch.name}: {exc}") from exc
            if args.check_hunks_only:
                for path in touched:
                    applied[path] += 1
                continue
            if diff.old != "/dev/null" and diff.old not in initialized:
                raise ValueError(f"{patch.name}: {diff.old}: no earlier new-file baseline; "
                                 "single-file replay needs the original source or additional dependencies")
            if diff.old != "/dev/null" and diff.new != "/dev/null" and diff.old != diff.new:
                raise ValueError(f"{patch.name}: rename needs both paths selected; unsupported in single-file replay")
            result = run_git(repo, "apply", "--whitespace=nowarn", "-", data=diff.data)
            if result.returncode:
                print(f"FAIL {patch.name}: {', '.join(sorted(touched))}", file=sys.stderr)
                sys.stderr.buffer.write(result.stderr)
                print("Selected-file replay stopped; dependencies outside these files were not checked.",
                      file=sys.stderr)
                return result.returncode
            if diff.new != "/dev/null":
                initialized.add(diff.new)
            else:
                initialized.discard(diff.old)
            for path in touched:
                applied[path] += 1
            print(f"OK {patch.name}: {', '.join(sorted(touched))}", flush=True)
    for path, number in sorted(applied.items()):
        if not number:
            raise ValueError(f"no diffs found for {path}")
        print(f"PASS {path}: {number} file diffs" + (" (hunk counts only)" if args.check_hunks_only else ""))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--patch-dir", type=Path,
                        default=Path(__file__).resolve().parents[2] / "xnu/patches-11215")
    parser.add_argument("--only", action="append", metavar="PATH",
                        help="exact relative file path; repeat to select multiple files")
    parser.add_argument("--through", type=int, metavar="NUMBER", help="stop after this patch number")
    parser.add_argument("--check-hunks-only", action="store_true", help="check counts without applying")
    args = parser.parse_args(argv)
    selected = set(args.only or DEFAULT_FILES)
    for path in selected:
        if PurePosixPath(path).is_absolute() or ".." in PurePosixPath(path).parts:
            parser.error("--only must be a safe relative path")
    try:
        with tempfile.TemporaryDirectory(prefix="raphael-patch-check-") as directory:
            repo = Path(directory)
            result = run_git(repo, "init", "--quiet")
            if result.returncode:
                sys.stderr.buffer.write(result.stderr)
                return result.returncode
            return replay(args, selected, repo)
    except (ValueError, OSError) as exc:
        print(f"ERROR {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
