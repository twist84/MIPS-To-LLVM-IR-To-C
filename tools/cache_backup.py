"""Verified 7-Zip recovery backups and temporary object-cache access."""
import os
import hashlib
import json
import time
from pathlib import Path
import shutil
import subprocess
import tempfile
from contextlib import contextmanager, ExitStack


def seven_zip():
    for name in ("7z", "7zz", "7za"):
        executable = shutil.which(name)
        if executable:
            return executable
    if os.name == "nt":
        for key in ("ProgramW6432", "ProgramFiles", "ProgramFiles(x86)"):
            root = os.environ.get(key)
            if root:
                executable = Path(root) / "7-Zip" / "7z.exe"
                if executable.is_file():
                    return str(executable)
    return None


BACKUP_INVENTORY = "backup-inventory.json"
BACKUP_SCHEMA = "ee-recovery-ir-backup-v1"
PROFILE_NAMES = {"O1", "O2", "O3", "IR-O2", "O2-native"}


def recovery_file(relative):
    """Allow only IR and receipts in known canonical/legacy cache layouts."""
    parts = list(relative.parts)
    while parts and (parts[0] in PROFILE_NAMES or parts[0].startswith("legacy-")):
        parts.pop(0)
    if len(parts) == 1:
        return parts[0] in {"translation-cache.json", "shared-ir.json"}
    if len(parts) != 2:
        return False
    if parts[0] == "ir":
        return (parts[1].endswith(".ll") or
                parts[1] in {"ee_functions.json", ".shared-ir-provenance.json"})
    return parts == ["objects", "objects.json"]


def file_record(path):
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    return {"size": path.stat().st_size, "sha256": digest}


def backup_files(root):
    """Refuse links/reparse points rather than following external files."""
    files = []
    for current, dirs, names in os.walk(root, followlinks=False):
        for name in sorted(dirs + names):
            path = Path(current) / name
            stat = path.lstat()
            if path.is_symlink() or getattr(stat, "st_file_attributes", 0) & 0x400:
                raise ValueError(f"backup contains a link or reparse point: {path}")
            if path.is_file():
                files.append(path)
            elif not path.is_dir():
                raise ValueError(f"backup contains an unsupported file: {path}")
    return sorted(files)


def verify_ir_backup(root):
    inventory = json.loads((root / BACKUP_INVENTORY).read_text(encoding="utf-8"))
    if inventory.get("schema") != BACKUP_SCHEMA or not isinstance(inventory.get("files"), dict):
        raise ValueError("unsupported IR backup inventory")
    actual = {path.relative_to(root).as_posix(): file_record(path)
              for path in backup_files(root) if path != root / BACKUP_INVENTORY}
    if (actual != inventory["files"] or
            any(not recovery_file(Path(name)) for name in actual)):
        raise ValueError("IR backup inventory/hash mismatch")
    return inventory


def partition_backup(backup, preserved_profiles=()):
    """After successful promotion, separate recovery artifacts from old native data.

    Never discard displaced files; on failure the uncompressed old tree is
    retained. The old IR is never moved out of the backup during partition.
    """
    started = time.monotonic()
    retained = backup.with_name(backup.name.replace("backup-", "retained-", 1))
    ir_backup = backup.with_name(backup.name + "-ir")
    if retained.exists() or ir_backup.exists() or ir_backup.with_suffix(".7z").exists():
        raise ValueError("IR backup/retention destination already exists")
    original_files = backup_files(backup)
    if (backup / BACKUP_INVENTORY).exists():
        raise ValueError("backup inventory already exists")
    moved = []
    copied = []
    try:
        for path in original_files:
            relative = path.relative_to(backup)
            if not recovery_file(relative):
                target = retained / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                path.rename(target)
                moved.append((path, target))
        # Shared native profiles remain in the canonical store. Copy only
        # their small receipts; their DLL/object bytes are not backed up.
        for profile in preserved_profiles:
            for relative in (Path("translation-cache.json"), Path("shared-ir.json"),
                             Path("objects/objects.json")):
                source = profile / relative
                if not source.is_file():
                    continue
                if source.is_symlink() or getattr(source.lstat(), "st_file_attributes", 0) & 0x400:
                    raise ValueError(f"linked profile receipt: {source}")
                target = backup / profile.name / relative
                if target.exists():
                    raise ValueError(f"duplicate profile receipt: {target}")
                target.parent.mkdir(parents=True, exist_ok=True)
                copied.append(target)
                shutil.copyfile(source, target)
        files = {path.relative_to(backup).as_posix(): file_record(path)
                 for path in backup_files(backup)}
        inventory = {"schema": BACKUP_SCHEMA, "files": files}
        (backup / BACKUP_INVENTORY).write_bytes(
            (json.dumps(inventory, indent=2, sort_keys=True) + "\n").encode("utf-8"))
        verify_ir_backup(backup)
        for directory in sorted((p for p in backup.rglob("*") if p.is_dir()),
                                key=lambda p: len(p.parts), reverse=True):
            if not any(directory.iterdir()):
                directory.rmdir()
        backup.rename(ir_backup)
    except BaseException:
        (backup / BACKUP_INVENTORY).unlink(missing_ok=True)
        for target in reversed(copied):
            target.unlink(missing_ok=True)
        for source, target in reversed(moved):
            source.parent.mkdir(parents=True, exist_ok=True)
            target.rename(source)
        if retained.exists():
            for directory in sorted((p for p in retained.rglob("*") if p.is_dir()),
                                    key=lambda p: len(p.parts), reverse=True):
                if not any(directory.iterdir()):
                    directory.rmdir()
            if not any(retained.iterdir()):
                retained.rmdir()
        raise
    print(f"IR backup inventory: {len(files)} files; "
          f"{sum(item['size'] for item in files.values())} bytes; "
          f"partition/authentication={time.monotonic() - started:.3f}s", flush=True)
    if retained.exists():
        print(f"Displaced native/diagnostic files retained uncompressed: {retained}", flush=True)
    return ir_backup


def compress_backup(backup):
    """Archive only after promotion succeeds; preserve the directory on failure."""
    executable = seven_zip()
    archive = backup.with_suffix(".7z")
    if not executable:
        print(f"WARNING: 7-Zip not found; uncompressed backup retained: {backup}", flush=True)
        return backup
    if archive.exists():
        print(f"WARNING: Archive already exists; uncompressed backup retained: {backup}", flush=True)
        return backup
    total_bytes = sum(path.stat().st_size for path in backup_files(backup))
    # A temporary sibling avoids publishing a partial archive on interruption.
    with tempfile.TemporaryDirectory(prefix=".backup-archive-", dir=backup.parent) as temp:
        pending = Path(temp) / archive.name
        try:
            started = time.monotonic()
            subprocess.run([executable, "a", "-t7z", "-mx=9", "-m0=LZMA2",
                            "-ms=on", "-y", str(pending), "*"],
                           cwd=backup, check=True)
            compression_seconds = time.monotonic() - started
            started = time.monotonic()
            subprocess.run([executable, "t", str(pending)], check=True)
            verification_seconds = time.monotonic() - started
            if archive.exists():
                raise FileExistsError(archive)
            pending.rename(archive)
        except (OSError, subprocess.CalledProcessError) as error:
            print(f"WARNING: 7-Zip backup failed ({error}); retained: {backup}", flush=True)
            return backup
    print(f"Backup compression: level=9 input_bytes={total_bytes} "
          f"archive_bytes={archive.stat().st_size} "
          f"compression={compression_seconds:.3f}s "
          f"verification={verification_seconds:.3f}s", flush=True)
    try:
        shutil.rmtree(backup)
    except OSError as error:
        print(f"WARNING: Verified archive saved, directory cleanup failed: {error}", flush=True)
    return archive


@contextmanager
def backup_object_candidates(source_work, profile):
    """Keep extracted object trees alive through the existing hash-checked seed."""
    with ExitStack() as stack:
        candidates = []
        executable = None
        roots = [*source_work.glob("backup-*"), *source_work.glob("retained-*")]
        for backup in sorted(roots, reverse=True):
            # IR-only archives cannot contain native object payloads.
            if backup.name.endswith(("-ir", "-ir.7z")) or backup.is_symlink():
                continue
            if backup.is_file() and backup.suffix == ".7z":
                executable = executable or seven_zip()
                if not executable:
                    continue
                extracted = Path(stack.enter_context(tempfile.TemporaryDirectory(
                    prefix=".backup-objects-", dir=source_work.parent)))
                try:
                    subprocess.run([executable, "x", "-y", f"-o{extracted}", str(backup),
                                    f"-i!{profile}/objects/*",
                                    f"-i!legacy-{profile}/objects/*",
                                    f"-i!legacy-recovery-*-{profile}/objects/*"],
                                   check=True, stdout=subprocess.DEVNULL)
                except (OSError, subprocess.CalledProcessError) as error:
                    print(f"WARNING: Could not seed objects from {backup}: {error}", flush=True)
                    continue
                backup = extracted
            elif not backup.is_dir():
                continue
            candidates.extend((backup / profile, backup / ("legacy-" + profile)))
            candidates.extend(backup.glob("legacy-recovery-*-" + profile))
        yield candidates
