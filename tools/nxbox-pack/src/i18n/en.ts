// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

export default {
  installMode:
    "1. Send games to the NXbox library\n2. Install this game as its own app\nChoose a mode",
  libraryGame:
    "Drag a game, update or DLC (.nsp .nsz .xci .xcz), one file at a time; Enter finishes",
  libraryKeys: "Drag prod.keys, or press Enter to skip",
  libraryMissing:
    "Main NXbox app not found on this console. Install NXbox first, then run --library again.",
  libraryDone:
    "Ready. Open NXbox: games appear in the library after it joins the parts on start. Big games can take a minute.",
  profileUsage:
    "Usage: nxbox-pack profile multi on|off, or nxbox-pack profile avatar <name> <image> | --character <id> --color <name> [--insecure-portal]",
  profilePackage: "Package full name (choose the app whose accounts you want to edit)",
  profileMissing:
    "Profile not found, duplicate name, or invalid profile data. Open NXbox once to create its profiles.",
  profileClose:
    "Close the selected NXbox app on the console before continuing. Press Enter when closed.",
  profileDone: "Profile updated. Reopen the selected NXbox app.",
  avatarInvalid:
    "Choose a valid PNG, JPEG, WebP, GIF or BMP image (up to 20 MB). The browser must finish within two minutes.",
  avatarBrowser: "Converting in your local browser. If it did not open, open this URL on this PC:",
  avatarConverting: "Preparing your NXbox avatar…",
  avatarConverted: "Image prepared. Return to NXbox Pack; you can close this tab.",

  guide: "Drag the game file into this window and press Enter (supported: .nsp .xci .nsz .xcz)",
  legal: "You must own the game and dump your own keys. NXbox Pack never downloads games or keys.",
  keys: "Drag your prod.keys into this window and press Enter",
  extra: "Drag an update/DLC file, or press Enter to continue (one file at a time)",
  host: "Xbox address (Dev Home > Remote Access)",
  found: "Xbox candidates found",
  user: "Device Portal username",
  password: "Device Portal password (hidden)",
  saved: "Use credentials from the OS secure store? [Y/n]",
  remember: "Save credentials in the OS secure store? [y/N]",
  secureUnavailable: "Secure storage is unavailable; credentials are kept only for this session.",
  fallback:
    "Control NCA metadata extraction is not implemented. Using the filename title ID/name and a generic NXbox icon.",
  title: "Base game title ID (16 hexadecimal digits starting with 0100)",
  name: "Game display name",
  runtime: "Downloading and verifying the NXbox runtime…",
  install: "Registering your independent game app…",
  upload: "Uploading to this app's private LocalState…",
  done: "Ready. Open this game's tile in Xbox Dev Home; set its app type to Game for the full memory budget.",
  failed: "NXbox Pack failed",
  file: "Choose an existing, nonempty .nsp, .xci, .nsz or .xcz file.",
  keyFile: "Choose an existing, nonempty file named prod.keys.",
  titleInvalid:
    "A base game title ID must contain 16 hex digits, begin with 0100 and end with 000.",
  nameInvalid: "Enter a game name of 1–64 characters without control characters.",
  compressed:
    "NSZ/XCZ needs the optional nsz decompressor on PATH (nsz -D). Install it or provide an NSP/XCI dump.",
  runtimeMissing:
    "Runtime unavailable; CI downloads may require NXBOX_PACK_GITHUB_TOKEN. Supply --runtime-url and --runtime-sha256 (CI artifact ZIP or game APPX).",
  runtimeInvalid: "Runtime download, checksum or payload validation failed.",
  portal: "Device Portal request failed. Check the address, credentials and Dev Mode.",
  deploy:
    "Xbox registration failed or timed out. Check VCLibs and the Device Portal deployment log.",
  interrupted: "Cancelled.",
  secret: "A terminal is required to enter a hidden password.",
  args: "Invalid arguments. Run --help.",
  progress: "Sending",
  itemGame: "Game",
  itemKeys: "Keys (prod.keys)",
  itemUpdate: "Update",
  itemDlc: "DLC",
  itemFirmware: "Firmware",
  itemSaves: "Save data",
  itemMods: "Mods",
  itemTitleKeys: "Keys (title.keys)",
  fileCount: "files",
  firmware:
    "Drag a firmware NCA folder, ZIP or hekate rawnand backup, or Enter to skip (recommended: without firmware, Mii screens and some system applets stay empty)",
  saves: "Drag a JKSV/Checkpoint backup leaf folder or ZIP for this title, or Enter to skip",
  mods: "Drag romfs/exefs folders or a mod folder/ZIP for this title, or Enter to skip",
  titleKeys: "Drag title.keys, or Enter to skip",
  inputInvalid:
    "Invalid folder/ZIP: unsafe paths, unsupported archive, empty data or corrupt filesystem.",
  firmwareInvalid:
    "Firmware rejected: NCA filename SHA-256, encrypted NCA3 header or SystemVersion data is invalid.",
  firmwareIncomplete:
    "Firmware incomplete: requires SystemVersion (0100000000000809, Data) and Mii model (0100000000000802, Data/PublicData).",
  firmwareKeys:
    "Firmware requires header_key and, for NAND, bis_key_02 (32 bytes, crypt+tweak). Encrypted SystemVersion also requires the matching key_area_key in prod.keys.",
  nandInvalid:
    "Invalid hekate NAND: missing/ambiguous SYSTEM GPT partition, missing split parts or unreadable FAT32 (check bis_key_02).",
  optionalMismatch:
    "Rejected: backup/mod title ID does not match this game. Drop one backup leaf for this title.",
  modsInvalid:
    "Rejected: mods must contain romfs/exefs inside one mod folder or load/<titleid>/<modname>/.",
  titleKeysInvalid: "Choose an existing, nonempty file named title.keys.",
  saveExists:
    "Save import stopped: this app already has a profile. Use a fresh game app to avoid replacing its user or saves.",
  none: "-",
  rejectNoId:
    "Rejected: no single [title ID] token in the file name, so it cannot be matched to the game.",
  rejectMismatch:
    "Rejected: this title ID does not belong to the game (expected its update ID or DLC range).",
  rejectDuplicate: "Rejected: this file or title ID was already added.",
  help: "NXbox Pack\nUsage: nxbox-pack [--library] [--runtime-url HTTPS_URL --runtime-sha256 HEX] [--insecure-portal]\n--insecure-portal accepts the Xbox self-signed TLS certificate for this connection only.\nDefault runtime: latest NspxMiguel/NXbox release with a SHA-256 asset digest.\nNo games or keys are included in the staged runtime. Own-app mode requires nsz on PATH for NSZ/XCZ; --library sends them directly.\n--library sends games to the installed main NXbox app without building a package.",
};
