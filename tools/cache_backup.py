"""Verified 7-Zip recovery backups and temporary object-cache access."""
import os
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
    # A temporary sibling avoids publishing a partial archive on interruption.
    with tempfile.TemporaryDirectory(prefix=".backup-archive-", dir=backup.parent) as temp:
        pending = Path(temp) / archive.name
        try:
            subprocess.run([executable, "a", "-t7z", "-mx=9", "-m0=LZMA2",
                            "-ms=on", "-y", str(pending), "*"],
                           cwd=backup, check=True)
            subprocess.run([executable, "t", str(pending)], check=True)
            if archive.exists():
                raise FileExistsError(archive)
            pending.rename(archive)
        except (OSError, subprocess.CalledProcessError) as error:
            print(f"WARNING: 7-Zip backup failed ({error}); retained: {backup}", flush=True)
            return backup
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
        for backup in sorted(source_work.glob("backup-*"), reverse=True):
            if backup.is_symlink():
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
