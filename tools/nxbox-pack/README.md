# NXbox Pack

Send locally owned games to the main NXbox library, or install a separate NXbox UWP app for each game.
A separate game app has its own identity, name, tile and private saves/keys/content. Once configured,
its tile boots directly into the game, without opening the NXbox library. Own-app mode packages an
emulator runtime; it does not recompile the game.

Download a binary from the **Build NXbox Pack** workflow artifacts:

| File                         | Host                 |
| ---------------------------- | -------------------- |
| `nxbox-pack-windows-x64.exe` | Windows x64          |
| `nxbox-pack-macos-arm64`     | Apple Silicon macOS  |
| `nxbox-pack-linux-x64`       | Linux x64 with glibc |

On macOS/Linux, run `chmod +x nxbox-pack-*` before launching. Run the binary in a terminal. No Bun,
Windows SDK, MakeAppx, SignTool, OpenSSL, signing certificate or emulator build is required for
uncompressed NSP/XCI files. An Intel Mac binary can be compiled with `bun-darwin-x64`; CI currently
ships the three targets above.

## Use

Enable Xbox Developer Mode and Device Portal authentication in **Dev Home > Remote Access**. The
console must already have `Microsoft.VCLibs.140.00` (x64, version 14.0.30704.0 or newer).

```sh
./nxbox-pack-macos-arm64 --insecure-portal
```

The self-signed Device Portal TLS certificate normally requires `--insecure-portal`. This flag
applies only to the selected portal connection. Omit it if the console certificate is trusted. Use a
trusted local network when accepting an unverified certificate.

Choose **Install this game as its own app** (option 2) for the steps below. The default, option 1,
sends games to the installed NXbox library.

1. Drag one game file and press Enter.
2. Drag `prod.keys` and press Enter.
3. Optionally drag updates/DLC, one per prompt, then press Enter without a file.
4. Confirm the filename-derived game name and ID. If there is no unambiguous bracketed base ID such
   as `[01007EF00011E000]`, enter it. Control NCA metadata extraction is not implemented; the tool
   explicitly announces the fallback and uses the generic NXbox tile images.
5. Confirm the discovered Xbox address or enter the address shown in Dev Home.
6. Enter Device Portal credentials; the password is hidden. You can opt into the OS secure store.
7. After installation/upload completes, set this game's app type to **Game** in Dev Home and open
   its tile. Registration and boot still need validation on an actual Xbox Series console.

Only Portuguese and English are used, selected automatically from the system locale. `LC_ALL`,
`LC_MESSAGES` and `LANG` take precedence over `Intl`; on Windows, `Get-Culture` supplies the locale
when no environment override is present. `NXBOX_PACK_LANG=pt` or `en` forces the language for tests.
There is no language selector.

Credentials stay in memory unless you choose to save them. macOS uses Keychain via `security`; Linux
uses Secret Service via `secret-tool`; Windows tries PasswordVault and falls back to a CurrentUser
DPAPI-encrypted record. If secure storage fails, the session continues without saving. Passwords are
never passed in process arguments or written as plaintext configuration.

## Send games to the NXbox library (big games)

Install the main NXbox app on the console first, then run:

```sh
./nxbox-pack-macos-arm64 --library --insecure-portal
```

Without `--library`, choose **Send games to the NXbox library** (option 1, the default).
Connect using the discovered Xbox address and Device Portal credentials. Drag the base game,
updates and DLC (`.nsp`, `.nsz`, `.xci`, `.xcz`), one file per prompt, then press empty Enter to finish.
NXbox Pack sends the original files to the installed `NSPX.NXbox_` package's `LocalState\games`.
NSZ/XCZ stay compressed; this mode needs no local decompressor and never downloads a runtime or
builds/signs a package.

Files up to 1 GiB use one upload; larger files use numbered 1 GiB parts and a final manifest,
with size verification and progress for the whole file. This avoids Device Portal's multi-GB upload
failure. Missing `prod.keys` and `title.keys` can be supplied optionally; existing keys are skipped
based on the console's file listing. Open NXbox afterward: it joins the parts on start before the
games appear in the library, which can take a minute for big games. Keep enough free space for
one extra part during joining.

## Runtime source

The default source is the latest `NspxMiguel/NXbox` release's `NXbox_game_*.appx` with a GitHub
SHA-256 digest. If unavailable, the tool tries the newest successful main-branch packaging workflow
runs and their unexpired `nxbox-release` / `nxbox-diagnostic` artifacts. CPU-only artifacts fail
payload validation. For authenticated CI downloads, set `NXBOX_PACK_GITHUB_TOKEN` to a token with
Actions read access for this repository. The token is not saved. A public release is the preferred
distribution path for users without GitHub credentials.

Alternatively, supply both a direct HTTPS URL and a trusted checksum:

```sh
./nxbox-pack-macos-arm64 --insecure-portal \
  --runtime-url https://example.org/NXbox_game_0.3.1.0_x64.appx \
  --runtime-sha256 YOUR_64_HEX_DIGIT_SHA256
```

A URL may point to a game APPX, a packaging CI artifact ZIP containing that APPX, or a ZIP of the
staged runtime (`nxbox.exe`, required DLLs, `Assets/`, `Notices/`). Do not use an unprepared raw
`nxbox-uwp-build` artifact: it lacks the complete game payload. ZIPs are handled in TypeScript; no
external unzip command is needed. Downloads are limited to 256 MiB compressed and ZIP contents to
768 MiB expanded per archive. SHA-256 is checked before extraction. Runtime staging retains licenses
and strips signed APPX bookkeeping, homebrew samples, game dumps, keys and stale boot configuration.

## Compressed games

In own-app mode, NSZ/XCZ files are accepted, but need the optional
[nsz decompressor](https://github.com/nicoboss/nsz) on PATH. NXbox Pack runs
`nsz -D --keys <your-prod.keys> -o <private-temp-directory> <dump>` locally before deployment. It
uploads the resulting NSP/XCI and deletes temporary output afterward. The original dump is
preserved. Allow space for both the compressed input and decompressed output; updates/DLC follow the
same process. Without `nsz`, use an NSP/XCI dump. The CLI reports the missing requirement instead of
uploading compressed files that cannot boot through `skip_library.txt`.

## Big games

The Device Portal file API fails on a single multi-GB upload (measured at ~3.6 GB), so any file
above 1 GiB is sent in 1 GiB parts (`base.nsp.nxpart000`, `.nxpart001`, ... then a
`base.nsp.nxparts` manifest, last). NXbox joins the parts into the original file on its next start;
leave the console with enough free space for one more part while it does.

## Development

Use Bun **1.3.14** (`d1632b29`; local full revision `d1632b291`):

```sh
cd tools/nxbox-pack
bun install --frozen-lockfile
bun run check
bun test
bun run build:windows
bun run build:macos
bun run build:linux
```

Every build command includes `src/cli.ts`, fixing the "Missing entrypoints" error from invoking
`bun build` alone. The CI matrix tests and builds on all three OSes, runs each compiled executable
with English/Portuguese `--help`, and uploads the binary with its SHA-256 sidecar. Generated
binaries and dependencies are ignored by Git.

See [the design and console verification plan](../../docs/nxbox-pack.md).

## Ownership

You must own the game and dump your own keys. NXbox Pack never downloads games or keys. They are
uploaded separately into the installed app's private LocalState, never included in the staged
runtime or a distributable package. Do not share game dumps, keys, or console LocalState backups.

Profile characters share NXbox's 120 original portraits and 40 named backgrounds, including
`sunrise`, `lagoon`, `dusk`, and `ember` gradients. The stable `fox-pilot` ID now represents a fox
ceramic artisan. The console's Settings > Account > Profile picture works in single-account mode;
Multiple accounts is off by default and no first-launch account question appears.
