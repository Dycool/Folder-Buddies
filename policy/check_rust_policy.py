#!/usr/bin/env python3
"""Fail closed when repository-authored Rust tries to weaken safety policy."""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
CRATES = ROOT / "crates"

errors: list[str] = []

cargo = (ROOT / "Cargo.toml").read_text(encoding="utf-8")
required_cargo = (
    '[workspace.lints.rust]\nunsafe_code = "forbid"',
    '[workspace.lints.clippy]\ntransmute_ptr_to_ptr = "forbid"\nundocumented_unsafe_blocks = "forbid"',
)
for marker in required_cargo:
    if marker not in cargo:
        errors.append(f"Cargo.toml is missing required safety policy: {marker!r}")

crate_roots = list(CRATES.glob("*/src/lib.rs")) + list(CRATES.glob("*/src/main.rs"))
for root in crate_roots:
    first = root.read_text(encoding="utf-8").splitlines()[:1]
    if first != ["#![forbid(unsafe_code)]"]:
        errors.append(f"{root.relative_to(ROOT)} must begin with #![forbid(unsafe_code)]")

attribute_override = re.compile(r"#\s*!?\s*\[\s*(allow|warn)\s*\(")
unsafe_construct = re.compile(r"\bunsafe\s*(?:\{|fn\b|impl\b|trait\b|extern\b)")
raw_pointer = re.compile(r"\*\s*(?:const|mut)\b")

for path in CRATES.rglob("*.rs"):
    text = path.read_text(encoding="utf-8")
    rel = path.relative_to(ROOT)
    for line_no, line in enumerate(text.splitlines(), 1):
        if attribute_override.search(line):
            errors.append(f"{rel}:{line_no}: allow/warn lint overrides are forbidden")
        if unsafe_construct.search(line):
            errors.append(f"{rel}:{line_no}: unsafe Rust construct is forbidden")
        if raw_pointer.search(line):
            errors.append(f"{rel}:{line_no}: raw pointer type is forbidden")
        if "std::mem::transmute" in line or "core::mem::transmute" in line:
            errors.append(f"{rel}:{line_no}: transmute is forbidden")

for build_script in ROOT.rglob("build.rs"):
    if ".git" not in build_script.parts and "target" not in build_script.parts:
        errors.append(f"{build_script.relative_to(ROOT)}: repository build scripts are forbidden")

if errors:
    print("Rust safety policy violations:", file=sys.stderr)
    for error in errors:
        print(f"  - {error}", file=sys.stderr)
    raise SystemExit(1)

print(f"Rust safety policy OK ({len(crate_roots)} crate roots checked)")
