"""Shared EE source IR and append-only provenance, independent of native profiles."""
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path

MARKER = "shared-ir.json"
PROVENANCE = ".shared-ir-provenance.json"
SCHEMA = "ee-shared-ir-v1"
MARKER_SCHEMA = "ee-shared-ir-v2"
_held = set()


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def write_json(path, value):
    temporary = path.with_name(path.name + ".tmp")
    try:
        temporary.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def relative_path(path, directory):
    """Manifest-relative path, retaining an absolute path only across volumes."""
    try:
        return Path(os.path.relpath(Path(path).resolve(), Path(directory).resolve())).as_posix()
    except ValueError:
        return str(Path(path).resolve())


def resolve_path(value, directory, *, legacy_cwd=False):
    path = Path(value)
    if path.is_absolute():
        return path.resolve()
    if legacy_cwd and len(path.parts) > 1 and path.exists():
        return path.resolve()
    return (Path(directory) / path).resolve()


def module_path(value, directory):
    path = Path(value)
    return resolve_path(value, directory, legacy_cwd=len(path.parts) > 1)


def elf_path(metadata, directory):
    return resolve_path(metadata["elf"], directory,
                        legacy_cwd=metadata.get("path_format") != "manifest-relative-v1")


def normalize_metadata(metadata_path, elf):
    """Convert freshly generated or independently verified metadata only."""
    metadata = read_json(metadata_path)
    directory = metadata_path.parent.resolve()
    for function in metadata["functions"]:
        module = module_path(function["ir_path"], directory)
        if module.parent != directory or module.suffix != ".ll" or not module.is_file():
            raise ValueError("function references an unexpected IR path")
        function["ir_path"] = module.name
    metadata["elf"] = relative_path(elf, directory)
    metadata["path_format"] = "manifest-relative-v1"
    write_json(metadata_path, metadata)


def ir_directory(work):
    work = Path(work).resolve()
    marker = work / MARKER
    if not marker.is_file():
        return work / "ir"
    try:
        value = read_json(marker)
        path = Path(value["ir_dir"])
    except (OSError, ValueError, KeyError, TypeError) as exc:
        raise ValueError("invalid shared EE IR directory marker") from exc
    schema = value.get("schema")
    if schema == MARKER_SCHEMA and not path.is_absolute():
        path = work / path
    elif schema != SCHEMA or not path.is_absolute():
        raise ValueError("invalid shared EE IR directory marker")
    path = path.resolve()
    if (path.name != "ir" or path == work / "ir" or
            not (path / PROVENANCE).is_file()):
        raise ValueError("invalid shared EE IR directory marker")
    return path


@contextmanager
def ir_lock(work):
    """Exclusive, OS-released lock for the lifetime of a translation/replay."""
    directory = ir_directory(work)
    root = directory.parent
    key = root.parent / (root.name + ".ee-ir.lock")
    if key in _held:
        yield
        return
    key.parent.mkdir(parents=True, exist_ok=True)
    with key.open("a+b") as handle:
        if handle.seek(0, os.SEEK_END) == 0:
            handle.write(b"\0")
            handle.flush()
        handle.seek(0)
        try:
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            raise ValueError(f"shared EE IR is in use: {root}") from exc
        _held.add(key)
        try:
            if ir_directory(work) != directory:
                raise ValueError("shared EE IR location changed; retry")
            yield
        finally:
            _held.remove(key)
            handle.seek(0)
            if os.name == "nt":
                msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(handle.fileno(), fcntl.LOCK_UN)


def map_digest(metadata, count=None):
    functions = metadata["functions"]
    count = len(functions) if count is None else count
    normalized = dict(metadata)
    normalized["function_count"] = count
    normalized["functions"] = [
        dict(fn, ir_path=Path(fn["ir_path"]).name)
        for fn in functions[:count]
    ]
    data = json.dumps(normalized, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(data.encode("utf-8")).hexdigest()


def prefix_hashes(metadata, hashes, count):
    return {Path(fn["ir_path"]).name: hashes[Path(fn["ir_path"]).name]
            for fn in metadata["functions"][:count]}


def active(metadata_path):
    return (metadata_path.parent / PROVENANCE).is_file()


def current_state(metadata_path, ir_sources, hashes=None):
    path = metadata_path.parent / PROVENANCE
    if not path.is_file():
        return None
    state = read_json(path)
    prefix = "shared EE IR provenance is stale or incompatible"
    if state.get("schema") != SCHEMA:
        raise ValueError(f"{prefix}: unsupported schema")
    saved_sources = state.get("ir_sources")
    if saved_sources != ir_sources:
        if not isinstance(saved_sources, dict) or not isinstance(ir_sources, dict):
            raise ValueError(f"{prefix}: missing lowering fingerprints")
        changed = sorted(name for name in set(saved_sources) | set(ir_sources)
                         if saved_sources.get(name) != ir_sources.get(name))
        raise ValueError(f"{prefix}: lowering inputs changed: " +
                         ", ".join(changed[:8]))
    if state.get("metadata_sha256") != digest(metadata_path):
        raise ValueError(f"{prefix}: function metadata hash changed")
    if hashes is not None and state.get("ir_hashes") != hashes:
        raise ValueError(f"{prefix}: guest module hashes changed")
    return state


def publish(metadata_path, ir_sources, hashes, alias=None):
    """Publish verified IR only; previously recorded maps must be prefixes."""
    metadata = read_json(metadata_path)
    count = len(metadata["functions"])
    if count != len(hashes) or metadata.get("function_count") != count:
        raise ValueError("shared EE IR map/hash count mismatch")
    if prefix_hashes(metadata, hashes, count) != hashes:
        raise ValueError("shared EE IR map/hash names mismatch")
    path = metadata_path.parent / PROVENANCE
    sha = digest(metadata_path)
    history = {}
    if path.is_file():
        old = read_json(path)
        previous_count = len(old["ir_hashes"])
        previous = old["history"][old["metadata_sha256"]]
        if (old.get("schema") != SCHEMA or old.get("ir_sources") != ir_sources or
                previous_count > count or previous["count"] != previous_count or
                previous["map"] != map_digest(metadata, previous_count) or
                prefix_hashes(metadata, hashes, previous_count) != old["ir_hashes"]):
            raise ValueError("shared EE IR may only extend a verified function map")
        history = dict(old["history"])
    history[sha] = {"count": count, "map": map_digest(metadata)}
    if alias is not None:
        old_sha, old_count, old_map = alias
        if old_count > count or old_map != map_digest(metadata, old_count):
            raise ValueError("legacy EE map is not a shared map prefix")
        history[old_sha] = {"count": old_count, "map": old_map}
    value = {"schema": SCHEMA, "ir_sources": ir_sources,
             "metadata_sha256": sha, "ir_hashes": hashes, "history": history}
    if not path.is_file() or read_json(path) != value:
        write_json(path, value)


def verified_extension(snapshot, metadata_path, ir_sources, hashes):
    """Recognize even multi-batch updates made by another native profile."""
    if not active(metadata_path):
        return False
    try:
        state = current_state(metadata_path, ir_sources, hashes)
        previous = state["history"][snapshot["metadata_sha256"]]
        metadata = read_json(metadata_path)
        old_count = previous["count"]
        return (snapshot.get("ir_sources") == ir_sources and
                0 < old_count <= len(metadata["functions"]) and
                previous["map"] == map_digest(metadata, old_count) and
                prefix_hashes(metadata, hashes, old_count) == snapshot["ir_hashes"])
    except (OSError, ValueError, KeyError, TypeError):
        return False


def attach(work, directory):
    relative = relative_path(directory, work)
    if Path(relative).is_absolute():
        raise ValueError("shared EE IR must reside on the same volume")
    write_json(work / MARKER, {"schema": MARKER_SCHEMA, "ir_dir": relative})
