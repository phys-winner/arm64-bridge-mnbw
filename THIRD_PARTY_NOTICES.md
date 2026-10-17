# Third-party notices

The AGPL-3.0-only grant for the original project code does not replace any
upstream license below. Keep the upstream notices with redistributed sources.

| Component | Version or revision | License | Included license text |
| --- | --- | --- | --- |
| [Dynarmic](https://github.com/azahar-emu/dynarmic) | `e77b1ba0b7da7cbe93021b01a663acfe7c4dd516` | 0BSD | `arm64_bridge/patches/dynarmic-LICENSE.txt` |
| [Boost](https://www.boost.org/) | 1.83.0, header distribution | BSL-1.0 | `arm64_bridge/third_party/boost/LICENSE_1_0.txt` |
| [astcenc](https://github.com/ARM-software/astc-encoder) | 5.7.0, source subset | Apache-2.0 | `arm64_bridge/third_party/astcenc/LICENSE.txt` |
| [Texture2DDecoder](https://github.com/K0lb3/texture2ddecoder) | 1.0.6, ASTC/ETC subset | MIT | `arm64_bridge/third_party/texture2ddecoder/LICENSE` |

Dynarmic is fetched from its pinned upstream revision, rather than stored as a
Git submodule of this repository. The supplied patch preserves the local ARM64
changes; original additions in that patch are covered by the project's
AGPL-3.0-only grant, and its upstream context retains Dynarmic's license.
Dynarmic's bundled dependencies carry their own notices in the downloaded
`externals/` directories, including fmt, mcl, oaknut, robin-map, and the
architecture-specific libraries selected by its build.

The Texture2DDecoder subset includes fp16 headers with their own copyright
notices. Their notices are retained in the source files.

Names and contact addresses in upstream copyright/license notices identify
their respective authors; they are not local machine or device information.

Proprietary game libraries, assets, original machine-code fixtures, and SDK/NDK
binaries are not distributed by this repository.
