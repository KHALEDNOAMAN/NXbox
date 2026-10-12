import { writeFile } from "node:fs/promises";
import { join, posix } from "node:path";
import { gameExtension } from "./files";
import { packageIdentity } from "./manifest";
import type { Transfer } from "./optional";
import { safePath } from "./storage";
import { PackError } from "./i18n";
import type { DevicePortal } from "./portal";
export async function deployGame(options: {
  portal: DevicePortal;
  titleId: string;
  runtimeDirectory: string;
  runtimeFiles: string[];
  content: string[];
  keys: string;
  temporary: string;
  transfers?: Transfer[];
  /** false installs the library app: no game.txt/skip_library.txt. */
  directBoot?: boolean;
  onUploading?: () => void;
  onProgress?: (name: string, sent: number, total: number) => void;
}): Promise<string> {
  const { portal, titleId, runtimeDirectory, runtimeFiles, content, keys, temporary } = options;
  const library = options.directBoot === false;
  if (!content.length && !library) throw new Error("A base game is required");
  const identity = packageIdentity(titleId);
  const folder = `${identity}-${crypto.randomUUID()}`;
  for (const file of runtimeFiles)
    await portal.uploadRuntime(folder, join(runtimeDirectory, file), file);
  const pkg = await portal.register(folder, identity);
  options.onUploading?.();
  await portal.makeFolder(pkg.PackageFullName, "LocalState", "games");
  await portal.makeFolder(pkg.PackageFullName, "LocalState", "eden");
  await portal.makeFolder(pkg.PackageFullName, "LocalState/eden", "keys");
  await portal.pushFile(pkg.PackageFullName, keys, "LocalState/eden/keys", "prod.keys");
  for (const [index, path] of content.entries()) {
    // The library lists every file in games/, so keep the original names there.
    const remoteName = library
      ? posix.basename(path.replaceAll("\\", "/"))
      : `${index === 0 ? "base" : `addon-${index}`}${gameExtension(path)}`;
    await portal.pushFile(
      pkg.PackageFullName,
      path,
      "LocalState/games",
      remoteName,
      (sent, total) => options.onProgress?.(remoteName, sent, total),
    );
  }
  const folders = new Set([
    "LocalState",
    "LocalState/eden",
    "LocalState/eden/keys",
    "LocalState/games",
  ]);
  for (const file of options.transfers ?? []) {
    safePath(file.destination);
    if (!file.destination.startsWith("LocalState/")) throw new PackError("inputInvalid");
    const directory = posix.dirname(file.destination);
    let parent = "LocalState";
    for (const name of directory.split("/").slice(1)) {
      const next = `${parent}/${name}`;
      if (!folders.has(next)) {
        await portal.makeFolder(pkg.PackageFullName, parent, name);
        folders.add(next);
      }
      parent = next;
    }
    if (file.destination.endsWith("/profiles.dat")) {
      if (
        (await portal.listFiles(pkg.PackageFullName, directory)).some(
          (f) => f.Name === "profiles.dat",
        )
      )
        throw new PackError("saveExists");
    }
    await portal.pushFile(
      pkg.PackageFullName,
      file.path,
      directory,
      posix.basename(file.destination),
      (sent, total) => options.onProgress?.(posix.basename(file.destination), sent, total),
    );
  }
  // Activate direct boot only after all content transfers have been verified.
  if (library) return identity;
  for (const [file, value] of [
    ["game.txt", `games\\base${gameExtension(content[0]!)}\n`],
    ["skip_library.txt", "1\n"],
  ] as const) {
    const path = join(temporary, file);
    await writeFile(path, value, { mode: 0o600 });
    await portal.pushFile(pkg.PackageFullName, path, "LocalState");
  }
  return identity;
}
