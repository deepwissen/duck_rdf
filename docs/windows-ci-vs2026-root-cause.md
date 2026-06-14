# Windows CI failure — root cause: GitHub `windows-latest` moved to Visual Studio 2026

## TL;DR

The Windows extension build started failing in June 2026 **without any change to
this repository**. The *exact same commit* that built green on **2026-05-26**
failed on **2026-06-13**. The only difference was the GitHub-hosted runner: the
`windows-latest` label was migrated from a **Visual Studio 2022** image to a
**Visual Studio 2026** image. VS 2026 broke the MSVC build in two independent
ways, and the second one (DuckDB's vendored `fmt`) is not fixable from this repo.

The working fix is to build the Windows target with **MinGW**
(`windows_amd64_mingw`) instead of MSVC (`windows_amd64`).

## Before / after — same code, 18 days apart

| | **2026-05-26 — ✅ SUCCESS** | **2026-06-13 — ❌ FAILURE** |
|---|---|---|
| Commit | `14b2fa1` ("Bump to duck 1.5.3") + main | same config (PR #38, only test files added) |
| Runner image | `win25/20260518.141` (Windows2025) | `win25-vs2026/20260608.135` (Windows2025-VS2026) |
| Visual Studio | **2022** (v17.14) | **2026** (v18.6.2) |
| MSVC toolset | `MSVC 14.44.35207` (`cl` 19.44) | `MSVC 14.51.36231` |
| `vcvars64.bat` path | `…\Visual Studio\2022\…` ✅ exists | `…\Visual Studio\2022\…` ❌ gone (now `…\18\…`) |
| `stdext::checked_array_iterator` | ✅ present | ❌ removed by the new STL |
| CMake compiler detection | `compiler identification is MSVC` | falls back to MinGW / errors |
| Result | builds + tests green | link / compile failure |

PR #38 itself only adds test files (`test/sql/pivot_rdf_edge.test`, etc.) and a
couple of source files — nothing build- or Windows-related. Any PR opened in this
window is red on Windows.

## Timeline

| Date | Event | Actor |
|---|---|---|
| 2026-05-07 | VS 2026 GA; GitHub announces `windows-latest` will move to it "in June" | GitHub (`actions/runner-images#14017`, `#14016`) |
| 2026-05-25/26 | `duck 1.5.3` config builds **green** on the VS 2022 image | — |
| ~2026-06-08 | GitHub flips `windows-latest` → VS 2026 image `20260608` | GitHub (`actions/runner-images#14202`) |
| 2026-06-13 | PR #38 builds **red** on the new image | — |

## Who changed it

GitHub's `actions/runner-images` team, via a pre-announced migration of the
**floating `windows-latest` label**. The matrix runs on `runs-on: windows-latest`
(defined in the `extension-ci-tools` submodule's `config/distribution_matrix.json`),
so the new image was inherited automatically. No change in this repo or upstream
selected it.

References:
- `actions/runner-images#14017` — `windows-latest`/`windows-2025` move to VS 2026 in June 2026
- `actions/runner-images#14016` — VS 2026 GA on GitHub Actions
- `actions/runner-images#14202` — the `20260608` image update that ran the failing build

## Root cause (two independent breakages from VS 2026)

**Layer A — `vcvars` path moved (MSVC never activates).**
The pinned reusable workflow `_extension_distribution.yml@v1.5.0` hard-codes
`call "…\Microsoft Visual Studio\2022\Enterprise\…\vcvars64.bat"`. On the VS 2026
image that path does not exist (`The system cannot find the path specified.`), so
MSVC is never activated. With no `CC`/`CXX` set and `-G Ninja`, CMake then picks
MinGW gcc from `PATH`, while vcpkg still builds the deps (curl/zlib/libxml2) with
MSVC (via `vswhere`). The result is a **mixed toolchain link**: MinGW objects vs
MSVC libraries, ~2,900 unresolved symbols (`__security_cookie`,
`__GSHandlerCheck`, `__chkstk`, `fprintf`, `snprintf`, …). This is the original
PR #38 link failure.

`extension-ci-tools@v1.5.3` fixes Layer A: it probes the `…\18\…` (VS 2026) path
first and falls back to `…\2022\…`.

**Layer B — `stdext::checked_array_iterator` removed (DuckDB `fmt` won't compile).**
Once MSVC is actually activated (v1.5.3 + forcing `CC/CXX=cl`), the build gets
further and then fails compiling DuckDB's vendored `fmt`:

```
duckdb/third_party/fmt/include/fmt/format.h(326):
  error C2653: 'stdext': is not a class or namespace name
  error C2061: syntax error: identifier 'checked_array_iterator'
```

The offending guard:

```cpp
#ifdef _SECURE_SCL   // checks if DEFINED, not its value
template <typename T> using checked_ptr = stdext::checked_array_iterator<T*>;
#else
template <typename T> using checked_ptr = T*;
#endif
```

VS 2026's STL still **defines** `_SECURE_SCL` (as 0 in release) but **removed**
`stdext::checked_array_iterator`. Because `fmt` uses `#ifdef` (presence) instead
of checking the value, it takes the dead branch and fails. This guard is identical
in **every** DuckDB stable release checked (`v1.4.1`, `v1.5.0`, `v1.5.3`), so the
MSVC build cannot be fixed by changing the DuckDB version, and DuckDB is
re-checked-out fresh in CI so a local patch does not persist.

## Why MSVC is not fixable from this repo (today)

- Layer B lives in the `duckdb` submodule (vendored `fmt`), re-checked-out at the
  pinned version on every run.
- It is present in all DuckDB stable releases, so a version bump does not help.
- The runner will not roll back to VS 2022; both `windows-latest` and
  `windows-2025` now resolve to VS 2026, and `windows-2022` is itself deprecated.

## The fix

Build the Windows target with **MinGW** instead of MSVC. The MinGW build is
self-consistent — GCC objects linked against GCC-built deps (`x64-mingw-static`)
— and does not involve Visual Studio at all, so VS 2026 is irrelevant to it.

Changes applied on branch `fix/windows-msvc-ci` (validated green):

1. **`.github/workflows/MainDistributionPipeline.yml`** — `exclude_archs` builds
   `windows_amd64_mingw` and excludes the MSVC `windows_amd64`.
2. **`.github/workflows/MainDistributionPipeline.yml`** — bump the reusable
   workflow `@v1.5.0 → @v1.5.3` (fixes Layer A for any future MSVC re-enable).
3. **`Makefile`** — gated `CC/CXX=cl` for `windows_amd64`, left dormant (only
   triggers if the MSVC target is re-enabled).

### Caveat (distribution)

The MinGW build produces a `windows_amd64_mingw` artifact — a *separate* platform
from MSVC `windows_amd64`. It loads into MinGW-built DuckDB; it is not a drop-in
for the official MSVC-built DuckDB on Windows. For unblocking CI this is
sufficient; for distributing an MSVC-ABI binary, the MSVC target must be
re-enabled once DuckDB supports VS 2026.

## Re-enabling MSVC later

When DuckDB ships a `fmt` that compiles under VS 2026 (or an equivalent fix):

1. In `exclude_archs`, stop excluding `windows_amd64` (and exclude
   `windows_amd64_mingw` if you want MSVC-only).
2. Keep `@v1.5.3` (or newer) and the `Makefile` `CC/CXX=cl` block — both are
   already correct for an MSVC build.

## Options summary

| Option | MSVC artifact? | CI green now? | Notes |
|---|---|---|---|
| MinGW (`windows_amd64_mingw`) | No | Yes | Current fix; MinGW ABI |
| Force MSVC via `/U_SECURE_SCL` | Maybe | Maybe | Uncertain; may expose further VS 2026 issues |
| Wait for upstream DuckDB | Yes (later) | Only if paired with MinGW now | No in-repo action for MSVC |
| Pin VS 2022 runner | — | — | Not available; VS 2022 image retired from `windows-latest`/`windows-2025` |
