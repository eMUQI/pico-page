#!/usr/bin/env python3
"""Build a pinned, host-only CREngine reference and compare actual text positions."""
import os
import argparse
from pathlib import Path
import subprocess
import sys
import tarfile

from setup_fonts import fetch

ROOT = Path(__file__).resolve().parents[1]
LOCAL = ROOT / ".local"
REVISION = "f4a8be93255a439db4777d00f0bc53316acabebd"


def run(*args, env=None):
    subprocess.run(args, cwd=ROOT, check=True, env=env)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--book", action="append", type=Path, default=[],
                        help="Also verify this unchanged EPUB with the actual reference engine")
    parser.add_argument("--sample-step", type=int, default=1, help="Compare every Nth scalar plus each part end; default checks all scalars")
    args = parser.parse_args()
    if not 1 <= args.sample_step <= 1000000:
        parser.error("--sample-step must be between 1 and 1000000")
    archive = LOCAL / "downloads/crengine.tar.gz"
    fetch(archive, f"https://codeload.github.com/koreader/crengine/tar.gz/{REVISION}",
          "0551a3daef8d08203f35aefaf00af8227aa1925effaef6d74839b8bb77012583")
    source = LOCAL / f"crengine-{REVISION}"
    if not source.exists():
        with tarfile.open(archive) as bundle:
            for member in bundle.getmembers():
                path = Path(member.name)
                if path.is_absolute() or ".." in path.parts or member.issym() or member.islnk():
                    raise ValueError("Unsafe reference source archive")
            bundle.extractall(LOCAL)
    deps = LOCAL / "koreader-deps"
    fetch(deps / "include/xxhash.h", "https://raw.githubusercontent.com/Cyan4973/xxHash/v0.8.3/xxhash.h",
          "17973c0dc49d9854ca26caa191f0e12f7a424b68858d9a78de3860d959d85e4b")
    fetch(deps / "xxhash.c", "https://raw.githubusercontent.com/Cyan4973/xxHash/v0.8.3/xxhash.c",
          "5c3591fe6e6c86a619eb26760e9520e37a6fd5152882ab5ad93f912e2a855966")
    (deps / "lib/pkgconfig").mkdir(parents=True, exist_ok=True)
    run("cc", "-O2", "-fPIC", "-I" + str(deps / "include"), "-c", str(deps / "xxhash.c"), "-o", str(deps / "xxhash.o"))
    run("ar", "rcs", str(deps / "lib/libxxhash.a"), str(deps / "xxhash.o"))
    (deps / "lib/pkgconfig/libxxhash.pc").write_text(
        f"prefix={deps}\nlibdir=${{prefix}}/lib\nincludedir=${{prefix}}/include\n"
        "Name: xxHash\nDescription: Local CREngine reference dependency\nVersion: 0.8.3\n"
        "Libs: -L${libdir} -lxxhash\nCflags: -I${includedir}\n")
    env = os.environ.copy()
    env["PKG_CONFIG_PATH"] = str(deps / "lib/pkgconfig") + os.pathsep + env.get("PKG_CONFIG_PATH", "")
    engine_build = LOCAL / "crengine-build"
    run("cmake", "-S", str(source), "-B", str(engine_build), "-DCMAKE_BUILD_TYPE=Release", "-DAUTO=OFF", "-DFREETYPE=ON",
        "-DZLIB=ON", "-DHARFBUZZ=ON", "-DLIBPNG=ON", "-DLIBJPEG=ON", env=env)
    run("cmake", "--build", str(engine_build), "-j", "4", env=env)
    probe = LOCAL / "koreader-reference"
    probe.mkdir(exist_ok=True)
    (probe / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.20)
project(koreader_reference LANGUAGES C CXX)
add_subdirectory("{ROOT}/firmware/components/reader_core" core)
add_subdirectory("{ROOT}/firmware/components/reader_store" store)
add_subdirectory("{ROOT}/firmware/components/reader_document" document)
add_subdirectory("{ROOT}/firmware/components/reader_sync" sync)
find_package(PkgConfig REQUIRED)
pkg_check_modules(CRE_FONT REQUIRED IMPORTED_TARGET freetype2 harfbuzz)
add_library(reference_crengine SHARED IMPORTED)
set_target_properties(reference_crengine PROPERTIES
    IMPORTED_LOCATION "{engine_build}/libcrengine${{CMAKE_SHARED_LIBRARY_SUFFIX}}"
    INTERFACE_INCLUDE_DIRECTORIES "{engine_build};{source}/crengine/include")
add_executable(koreader_mapping_probe "{ROOT}/tools/koreader_mapping_probe.cpp")
target_link_libraries(koreader_mapping_probe PRIVATE reader_sync reference_crengine PkgConfig::CRE_FONT)
''')
    run("cmake", "-S", str(probe), "-B", str(probe / "build"), "-DCMAKE_BUILD_TYPE=Release", env=env)
    run("cmake", "--build", str(probe / "build"), "-j", "4", env=env)
    fixture = LOCAL / "koreader-probe.epub"
    run(sys.executable, str(ROOT / "tests/make_epub_fixture.py"), str(fixture))
    fixtures = LOCAL / "koreader-fixtures"
    run(sys.executable, str(ROOT / "tests/make_document_fixtures.py"), str(fixtures))
    for book in [fixture, fixtures / "image-mapping.epub",
                 *sorted(fixtures.glob("mapping-node-*.epub")), *args.book]:
        print(f"Reference comparison: {book}", flush=True)
        run(str(probe / "build/koreader_mapping_probe"), str(book.resolve()),
            str(LOCAL / "card/Fonts/LXGWWenKai-Regular.ttf"), str(args.sample_step))


if __name__ == "__main__":
    main()
