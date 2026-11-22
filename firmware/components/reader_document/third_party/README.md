# Native EPUB dependencies

These files are copied from CrossPoint commit
`50823ffa125b437b4c9fe5882f326458780198da`; no CrossPoint application,
Arduino, rendering, or KOReader code is included. `manifest.json` records
SHA-256 for every included source/header. The source commit, not the version
text in a file, is the reproducibility identifier.

| Directory | Source | License evidence | Local changes |
|---|---|---|---|
| `miniz` | `lib/miniz/third_party/miniz.c` and `.h`; header identifies 3.1.2 | `.c` contains the full RAD/Valve/Rich Geldreich MIT notice; `.h` describes the library as public domain and refers to an Unlicense statement, but this selected header does not contain a separate complete Unlicense text. The explicit MIT grant and all existing headers are retained | Source unchanged. Build disables deflate, writing, stdio and the zlib API; ESP builds prefix the exported inflate/checksum symbols to avoid ROM substitution |
| `expat` | `lib/expat/`; `xmlparse.c` identifies 2.7.3+ | Each Expat source/header retains its MIT copyright and permission notice; `siphash.h` separately identifies CC0 Public Domain Dedication | `expat_config.h` is populated for little-endian host/ESP32-S3, namespaces, UTF-8 output and general entities; external DTD parsing is disabled. `xmlparse.c` restores its platform entropy preprocessor guard from the selected source's commented-out form |

Read before copying: the miniz ZIP reader's file-callback initialization,
central-directory allocation, file statistics and callback extraction; the
Expat parser setup, XML handlers, `XML_SetHashSalt`, DTD handling, UTF-8 token
code, and the notices in all included files. The parser receives a nonzero
random hash salt from OpenSSL `RAND_bytes` or `esp_fill_random` before parsing.
`XML_POOR_ENTROPY` only permits the portable build; the application does not
use Expat's fallback salt. The pinned CrossPoint source comments out Expat's platform entropy
compile-time guard using backslash-continued `//` comments. Pico Page restores
the original guard to avoid Xtensa `-Werror=comment`; the configured explicit
salt path satisfies that guard through `XML_POOR_ENTROPY`.

ESP32-S3's ROM linker script defines `tinfl_decompress` as an absolute symbol.
Its 10,992-byte decompressor layout differs from this source's 8,364-byte Xtensa
layout. Calling ROM with the local structure corrupts memory. Component compile
definitions rename the inflate and checksum entrypoints with `pico_`; dependent
PNG decoding receives the same definitions. The linked implementation must be
`pico_tinfl_decompress` in application text, not ROM address `0x40000828`.

The host uses the installed OpenSSL Crypto library through `find_package`;
this machine reports OpenSSL 3.6.5, with Apache-2.0 license. No OpenSSL source
is copied. Firmware uses the ESP-IDF 6.1 `mbedtls` component's PSA hash API.
The local IDF Mbed TLS submodule is `a2b32072ea898afc1ed5b6caf6931e36028c91d6`;
its `LICENSE` is Apache-2.0. MD5 and SHA-256 are enabled in the current IDF
configuration. MD5 is used only for the KOReader document identifier, never
as the strong book identity or a credential hash.

Source distribution must retain the included copyright and license notices.
Binary packaging must include the relevant notices and satisfy the platform
libraries' licenses. This component does not select the repository's overall
license or complete the full firmware dependency review.
