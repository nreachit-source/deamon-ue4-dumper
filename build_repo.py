from __future__ import annotations

import gzip
import hashlib
import pathlib
import shutil


HERE = pathlib.Path(__file__).resolve().parent
SOURCE = HERE.parent / "com.local.ue4loadmonitor_1.2.2_iphoneos-arm64.deb"
POOL = HERE / "debs"
DEB = POOL / SOURCE.name


def digest(path: pathlib.Path, algorithm: str) -> str:
    hasher = hashlib.new(algorithm)
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


def checksum_lines(path: pathlib.Path) -> tuple[str, str, str]:
    size = path.stat().st_size
    return (
        f" {digest(path, 'md5')} {size:16d} {path.name}",
        f" {digest(path, 'sha1')} {size:16d} {path.name}",
        f" {digest(path, 'sha256')} {size:16d} {path.name}",
    )


def main() -> None:
    POOL.mkdir(exist_ok=True)
    shutil.copy2(SOURCE, DEB)

    package = f"""Package: com.local.ue4loadmonitor
Name: UE4 Load Monitor
Version: 1.2.2
Architecture: iphoneos-arm64
Description: Detects UE4 app process and exports reflected type schema.
Maintainer: Local Development
Author: Local Development
Section: Development
Depends: firmware (>= 13.0)
Filename: debs/{DEB.name}
Size: {DEB.stat().st_size}
MD5sum: {digest(DEB, 'md5')}
SHA1: {digest(DEB, 'sha1')}
SHA256: {digest(DEB, 'sha256')}

"""
    packages = HERE / "Packages"
    packages.write_text(package, encoding="utf-8", newline="\n")
    packages_gz = HERE / "Packages.gz"
    with packages.open("rb") as source, packages_gz.open("wb") as compressed:
        with gzip.GzipFile(filename="", mode="wb", fileobj=compressed, mtime=0) as output:
            shutil.copyfileobj(source, output)

    md5_packages, sha1_packages, sha256_packages = checksum_lines(packages)
    md5_gz, sha1_gz, sha256_gz = checksum_lines(packages_gz)
    release = f"""Origin: UE4 Load Monitor
Label: UE4 Load Monitor
Suite: stable
Codename: ios
Architectures: iphoneos-arm64
Components: main
Description: Private UE4 monitor development repository
MD5Sum:
{md5_packages}
{md5_gz}
SHA1:
{sha1_packages}
{sha1_gz}
SHA256:
{sha256_packages}
{sha256_gz}
"""
    (HERE / "Release").write_text(release, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
