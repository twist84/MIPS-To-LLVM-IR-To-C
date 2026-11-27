"""Read-only discovery log parser and persistent host-side evidence ledger."""
from collections import defaultdict
import hashlib
import json
import re

EVIDENCE_NAME = "ee-discovery-events.json"
MISS = re.compile(r"^(?:live cache-miss: PC=|auto-expand: compiling reachable PC |auto-expand: guest-reached PC )(0x[0-9a-fA-F]+)", re.MULTILINE)
SIBLING = re.compile(r"^auto-expand: compiling table sibling PC (0x[0-9a-fA-F]+)", re.MULTILINE)


def collect(text):
    result = defaultdict(set)
    for pattern, label in ((MISS, "runtime_cache_miss"), (SIBLING, "runtime_table_sibling")):
        for match in pattern.finditer(text):
            result[int(match[1], 16)].add(label)
    return result


def elf_digest(elf):
    digest = hashlib.sha256()
    with elf.open("rb") as source:
        for data in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(data)
    return digest.hexdigest()


def load(path, elf):
    if not path.is_file():
        return {}
    data = json.loads(path.read_text(encoding="utf-8"))
    if data["elf_sha256"] != elf_digest(elf):
        raise ValueError("runtime discovery evidence belongs to a different ELF")
    return {int(pc, 0): set(sources) for pc, sources in data["entries"].items()}


def record(work, elf, output):
    found = collect(output)
    if not found:
        return
    path = work / EVIDENCE_NAME
    existing = load(path, elf)
    for pc, sources in found.items():
        existing.setdefault(pc, set()).update(sources)
    data = {"elf_sha256": elf_digest(elf),
            "entries": {hex(pc): sorted(sources) for pc, sources in sorted(existing.items())}}
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)
