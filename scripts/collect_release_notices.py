#!/usr/bin/env python3
"""Copy dependency notices from the exact source trees used for a native build."""
from __future__ import annotations

import argparse
import shutil
from pathlib import Path


def collect(juce: Path, clap: Path, output: Path) -> None:
    repo = Path(__file__).resolve().parents[1]
    for root in (juce, clap):
        if not root.is_dir():
            raise FileNotFoundError(f"Dependency source tree is missing: {root}")
    output.mkdir(parents=True, exist_ok=False)
    shutil.copy2(repo / "docs/DISTRIBUTION.md", output / "StageSignal-distribution.md")
    roots = ((juce, "JUCE"), (clap, "clap-juce-extensions"), (repo / "third_party", "core-dependencies"))
    count = 0
    for root, name in roots:
        files = [p for p in root.rglob("*") if p.is_file() and ".git" not in p.relative_to(root).parts
                 and any(word in p.name.lower() for word in ("license", "licence", "copying", "notice"))]
        if not files:
            raise RuntimeError(f"No dependency notices found under {root}")
        for source in sorted(files):
            dest = output / name / source.relative_to(root)
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, dest)
            count += 1
    # These upstream notices are in README/header files, not named LICENSE.
    for relative in ("modules/juce_core/zip/zlib/README", "modules/juce_graphics/image_formats/jpglib/README"):
        source = juce / relative
        dest = output / "JUCE" / relative
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, dest)
    gl = (juce / "modules/juce_opengl/opengl/juce_gl.h").read_text(encoding="utf-8")
    begin = gl.index("// BEGIN_GLEW_LICENSE\n")
    end = gl.index("// END_GLEW_LICENSE", begin)
    (output / "JUCE/OpenGL-third-party-notices.txt").write_text(gl[begin:end], encoding="utf-8")
    print(f"Collected {count} dependency notice files, JPEG/zlib READMEs and OpenGL notices")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--juce-root", required=True, type=Path)
    parser.add_argument("--clap-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    collect(args.juce_root, args.clap_root, args.output)
