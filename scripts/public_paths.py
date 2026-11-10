"""Remove machine locations from public evidence without changing measurements."""
from pathlib import PurePosixPath, PureWindowsPath
import re

DRIVE = re.compile(r"(?<![A-Za-z0-9_])[A-Za-z]:[\\/]")
PRIVATE = re.compile(r"/(?:home|Users|private|tmp|workspace|__w|mnt|opt|usr|var)/")
EMBEDDED = re.compile(r'(?<![A-Za-z0-9_])[A-Za-z]:[\\/][^"\r\n]*?\.(?:exe|dll|cmake|lib|json|db|png|jpg|py|cpp|cu|h)(?=$|[\s";)|])', re.I)
POSIX_EMBEDDED = re.compile(r'''/(?:home|Users|private|tmp|workspace|__w|mnt|opt|usr|var)/[^\s|;"')]+''')


def logical(value):
    path = value.replace("\\", "/")
    parts = [part for part in path.split("/") if part]
    for anchor in ("selections", "inputs", "comparisons", "synthetic", "runs"):
        if anchor in parts:
            return "/".join(parts[parts.index(anchor):])
    name = parts[-1] if parts else "root"
    if name.endswith(".db"):
        return "library/" + name
    if name.endswith((".exe", ".dll")):
        return "tools/" + name
    if name.endswith(".cmake"):
        return "toolchain/" + name
    if name.lower() == "mosaicraft":
        return "workspace"
    return "resources/" + name


def redact(value):
    if isinstance(value, dict):
        return {key: redact(item) for key, item in value.items()}
    if isinstance(value, list):
        return [redact(item) for item in value]
    if not isinstance(value, str):
        return value
    if (DRIVE.match(value) and PureWindowsPath(value).is_absolute()) or PRIVATE.match(value):
        return logical(value)
    value = EMBEDDED.sub(lambda match: logical(match.group()), value)
    value = POSIX_EMBEDDED.sub(lambda match: logical(match.group()), value)
    if DRIVE.search(value) or PRIVATE.search(value):
        raise ValueError(f"Unclassified machine location in public evidence: {value[:120]}")
    return value


def public_report(value):
    value = redact(value)
    if isinstance(value, dict):
        value["paths_redacted"] = True
        value["path_contract"] = "Resource names relative to the evidence root; not a runtime checkpoint."
    return value
