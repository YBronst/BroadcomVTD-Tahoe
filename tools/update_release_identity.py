#!/usr/bin/env python3
"""Helper script to update Release/identity.json, repack Release ZIP archive, and update Release/SHA256SUMS."""
import datetime, hashlib, json, pathlib, plistlib, struct, sys, uuid, zipfile

def find_repo_root():
    # Start searching from the directory containing this script, ascending until we find Release/BroadcomVTD.kext
    current = pathlib.Path(__file__).resolve().parent
    for _ in range(5):
        if (current / 'Release' / 'BroadcomVTD.kext').exists():
            return current
        if current.parent == current:
            break
        current = current.parent
    # Fallback to current working directory
    cwd = pathlib.Path.cwd()
    if (cwd / 'Release' / 'BroadcomVTD.kext').exists():
        return cwd
    return pathlib.Path(__file__).resolve().parent

def main():
    R = find_repo_root()
    kext_path = R / 'Release' / 'BroadcomVTD.kext'

    if not kext_path.exists():
        sys.exit(f"Error: {kext_path} does not exist. (Search root was: {R})")

    # 1. Read Info.plist
    info = plistlib.loads((kext_path / 'Contents/Info.plist').read_bytes())
    version = info['CFBundleShortVersionString']
    identifier = info['CFBundleIdentifier']

    # 2. Compute hashes of bundle files
    bundle_files = {}
    for file_path in sorted(kext_path.rglob('*')):
        if file_path.is_file():
            rel_path = file_path.relative_to(kext_path).as_posix()
            bundle_files[rel_path] = hashlib.sha256(file_path.read_bytes()).hexdigest()

    # 3. Analyze Mach-O binary for UUID and bytes
    binary_path = kext_path / 'Contents/MacOS/BroadcomVTD'
    binary_bytes = binary_path.read_bytes()
    executable_sha256 = hashlib.sha256(binary_bytes).hexdigest()
    executable_bytes = len(binary_bytes)

    offset = 32
    macho_uuid = None
    ncmds = struct.unpack_from('<I', binary_bytes, 16)[0]
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', binary_bytes, offset)
        if cmd == 0x1b:  # LC_UUID
            macho_uuid = str(uuid.UUID(bytes=binary_bytes[offset+8:offset+24])).upper()
        offset += size

    if not macho_uuid:
        sys.exit("Error: Could not extract LC_UUID from BroadcomVTD executable.")

    # Read existing identity.json if present to preserve non-computed fields if any
    identity_file = R / 'Release' / 'identity.json'
    existing_identity = {}
    if identity_file.exists():
        try:
            existing_identity = json.loads(identity_file.read_text())
        except Exception:
            pass

    identity = {
        "version": version,
        "project": existing_identity.get("project", "BroadcomVTD-Tahoe"),
        "tag": f"v{version}",
        "status": existing_identity.get("status", "CI_BUILD"),
        "physical_test": existing_identity.get("physical_test", False),
        "release_authority": existing_identity.get("release_authority", False),
        "kext": "BroadcomVTD.kext",
        "identifier": identifier,
        "executable_bytes": executable_bytes,
        "executable_sha256": executable_sha256,
        "uuid": macho_uuid,
        "bundle_files": bundle_files,
        "generated_by": existing_identity.get("generated_by", "BroadcomVTD-TahoeBuilder"),
        "generated_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat()
    }

    identity_file.write_text(json.dumps(identity, indent=2) + '\n')
    print(f"Updated {identity_file}")

    # 4. Repack Release ZIP
    archive_name = f"{identity['project']}-{identity['tag']}.zip"
    zip_path = R / 'Release' / archive_name

    packaged_files = {
        'INSTALL.md': 'Release/INSTALL.md',
        f'RELEASE_NOTES_v{version}.md': f'RELEASE_NOTES_v{version}.md',
        'README.md': 'README.md',
        'LICENSE': 'LICENSE',
        'THIRD_PARTY_NOTICES.md': 'THIRD_PARTY_NOTICES.md',
        'Licenses/Lilu-BSD-3-Clause.txt': 'Licenses/Lilu-BSD-3-Clause.txt',
        'Licenses/MacKernelSDK-APSL-2.0.txt': 'Licenses/MacKernelSDK-APSL-2.0.txt',
        'identity.json': 'Release/identity.json',
    }

    with zipfile.ZipFile(zip_path, 'w', compression=zipfile.ZIP_DEFLATED) as z:
        for arcname, relpath in packaged_files.items():
            z.write(R / relpath, arcname)
        for bfile in bundle_files:
            z.write(kext_path / bfile, f"BroadcomVTD.kext/{bfile}")

    print(f"Repackaged {zip_path}")

    # 5. Update SHA256SUMS
    digest = hashlib.sha256(zip_path.read_bytes()).hexdigest()
    sums_file = R / 'Release' / 'SHA256SUMS'
    sums_file.write_text(f"{digest}  {archive_name}\n")
    print(f"Updated {sums_file}: {digest}  {archive_name}")

if __name__ == '__main__':
    main()
