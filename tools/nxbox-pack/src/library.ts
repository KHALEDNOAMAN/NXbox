// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

import { basename } from "node:path";
import { draggedPath, validateFile } from "./files";
import { PackError, type strings } from "./i18n";
import { prepareTitleKeys } from "./optional";
import type { DevicePortal } from "./portal";
import type { Prompt } from "./prompt";

type Text = (typeof strings)["en"];
type Questions = Pick<Prompt, "ask">;

export async function chooseLibraryMode(
  library: boolean,
  prompt: Questions,
  text: Text,
): Promise<boolean> {
  if (library) return true;
  for (;;) {
    const answer = await prompt.ask(text.installMode, "1");
    if (answer === "1") return true;
    if (answer === "2") return false;
  }
}

/** Send original dumps to the installed library; no runtime, packaging or decompression. */
export async function sendToLibrary(
  portal: DevicePortal,
  prompt: Questions,
  text: Text,
  onProgress?: (name: string, sent: number, total: number) => void,
): Promise<void> {
  const pkg = (await portal.packages()).find((pkg) =>
    pkg.PackageFullName.startsWith("NSPX.NXbox_"),
  );
  if (!pkg) throw new PackError("libraryMissing");
  const content: string[] = [];
  for (;;) {
    const answer = await prompt.ask(text.libraryGame);
    if (!answer) {
      if (!content.length) throw new PackError("file");
      break;
    }
    const path = draggedPath(answer);
    await validateFile(path);
    content.push(path);
  }
  await portal.makeFolder(pkg.PackageFullName, "LocalState", "games");
  for (const path of content) {
    const name = basename(path);
    await portal.pushFile(pkg.PackageFullName, path, "LocalState/games", name, (sent, total) =>
      onProgress?.(name, sent, total),
    );
  }
  // Create and list the directory before asking; an unavailable listing must never mean
  // "keys are missing", since that could overwrite the console's existing keys.
  await portal.makeFolder(pkg.PackageFullName, "LocalState", "eden");
  await portal.makeFolder(pkg.PackageFullName, "LocalState/eden", "keys");
  const existing = new Set(
    (await portal.listFiles(pkg.PackageFullName, "LocalState/eden/keys")).map((file) =>
      file.Name.toLowerCase(),
    ),
  );
  for (const name of ["prod.keys", "title.keys"] as const) {
    if (existing.has(name)) continue;
    const answer = await prompt.ask(name === "prod.keys" ? text.libraryKeys : text.titleKeys);
    if (!answer) continue;
    const path = draggedPath(answer);
    if (name === "prod.keys") await validateFile(path, true);
    else await prepareTitleKeys(path);
    await portal.pushFile(pkg.PackageFullName, path, "LocalState/eden/keys", name);
  }
}
