// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

import { expect, test } from "bun:test";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { parseArgs } from "../src/cli";
import { strings } from "../src/i18n";
import en from "../src/i18n/en";
import pt from "../src/i18n/pt_br";
import { chooseLibraryMode, sendToLibrary } from "../src/library";
import { DevicePortal, PART_SIZE, uploadPlan } from "../src/portal";

const mainPackage = "NSPX.NXbox_1.0.0.0_x64__test";

function consolePortal(keys: string[] = [], installed = true) {
  const uploads: { pkg: string; directory: string; name: string; data: string }[] = [];
  const calls: URL[] = [];
  const portal = new DevicePortal(
    "https://xbox.test:11443",
    { user: "u", password: "p" },
    true,
    (async (input: string | URL | Request, init?: RequestInit) => {
      const url = new URL(String(input));
      calls.push(url);
      if (url.pathname.endsWith("/packages"))
        return Response.json({
          InstalledPackages: [
            { Name: "Separate game", PackageFullName: "NSPX.NXbox.0100000000000000_1_x64__test" },
            ...(installed ? [{ Name: "NXbox", PackageFullName: mainPackage }] : []),
          ],
        });
      const directory = url.searchParams.get("path") ?? "";
      if (url.pathname.endsWith("/file") && init?.method === "POST") {
        const body = await new Response(init.body, { headers: init.headers }).formData();
        const file = body.get("file") as File;
        uploads.push({
          pkg: url.searchParams.get("packagefullname")!,
          directory,
          name: file.name,
          data: await file.text(),
        });
      } else if (url.pathname.endsWith("/files")) {
        return Response.json({
          Items: [
            ...(directory === "\\LocalState\\eden\\keys" ? keys.map((Name) => ({ Name })) : []),
            ...uploads
              .filter((file) => file.directory === directory)
              .map((file) => ({
                Name: file.name,
                FileSize: Buffer.byteLength(file.data),
              })),
          ],
        });
      } else {
        expect(["/api/os/machinename", "/api/filesystem/apps/folder"]).toContain(url.pathname);
      }
      return Response.json({}, { headers: { "Set-Cookie": "CSRF-Token=test" } });
    }) as typeof fetch,
  );
  return { portal, uploads, calls };
}

function questions(answers: string[]) {
  const asked: string[] = [];
  return {
    asked,
    async ask(message: string, fallback = "") {
      asked.push(message);
      if (!answers.length) throw new Error("Unexpected prompt");
      return answers.shift()! || fallback;
    },
  };
}

test("--library selects library mode without asking; interactive default is library first", async () => {
  const prompt = questions([]);
  expect(await chooseLibraryMode(parseArgs(["--library"]).library, prompt, strings.en)).toBe(true);
  expect(prompt.asked).toEqual([]);
  expect(parseArgs([]).library).toBe(false);
  expect(await chooseLibraryMode(false, questions([""]), strings.en)).toBe(true);
  expect(await chooseLibraryMode(false, questions(["invalid", "2"]), strings.en)).toBe(false);
});

test("3.5 GB and 3.5 GiB plans have four contiguous parts and the manifest last", () => {
  for (const size of [3_500_000_000, 3.5 * PART_SIZE]) {
    const plan = uploadPlan("game.nsz", size);
    expect(plan.files.map((file) => file.name)).toEqual([
      "game.nsz.nxpart000",
      "game.nsz.nxpart001",
      "game.nsz.nxpart002",
      "game.nsz.nxpart003",
    ]);
    expect(plan.files.map((file) => [file.start, file.end])).toEqual([
      [0, PART_SIZE - 1],
      [PART_SIZE, 2 * PART_SIZE - 1],
      [2 * PART_SIZE, 3 * PART_SIZE - 1],
      [3 * PART_SIZE, size - 1],
    ]);
    expect(plan.manifest).toEqual({ name: "game.nsz.nxparts", text: `parts=4\nsize=${size}\n` });
  }
});

test("500 MB and exactly 1 GiB plans upload the original file without a manifest", () => {
  for (const size of [500_000_000, PART_SIZE]) {
    expect(uploadPlan("game.xci", size)).toEqual({
      files: [{ name: "game.xci", start: 0, end: size - 1 }],
      manifest: undefined,
    });
  }
});

test("library uploads original compressed files to the main package and skips existing keys", async () => {
  const temporary = await mkdtemp(join(tmpdir(), "nxbox-library-test-"));
  try {
    const paths = [join(temporary, "base.nsz"), join(temporary, "update.xcz")];
    for (const path of paths) await writeFile(path, "original compressed bytes");
    const { portal, uploads } = consolePortal(["prod.keys", "TITLE.KEYS"]);
    const prompt = questions([...paths, ""]);
    const progress: [string, number, number][] = [];
    await sendToLibrary(portal, prompt, strings.en, (...args) => progress.push(args));
    expect(uploads).toEqual(
      ["base.nsz", "update.xcz"].map((name) => ({
        pkg: mainPackage,
        directory: "\\LocalState\\games",
        name,
        data: "original compressed bytes",
      })),
    );
    expect(prompt.asked).toEqual(Array(3).fill(strings.en.libraryGame));
    expect(progress).toEqual(["base.nsz", "update.xcz"].map((name) => [name, 25, 25]));
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
});

test("only missing keys are offered and uploaded to the main package's keys directory", async () => {
  const temporary = await mkdtemp(join(tmpdir(), "nxbox-library-test-"));
  try {
    const game = join(temporary, "game.nsp"),
      keys = join(temporary, "title.keys");
    await writeFile(game, "game");
    await writeFile(keys, "title keys");
    const { portal, uploads } = consolePortal(["prod.keys"]);
    const prompt = questions([game, "", keys]);
    await sendToLibrary(portal, prompt, strings.en);
    expect(prompt.asked).toEqual([
      strings.en.libraryGame,
      strings.en.libraryGame,
      strings.en.titleKeys,
    ]);
    expect(uploads[1]).toEqual({
      pkg: mainPackage,
      directory: "\\LocalState\\eden\\keys",
      name: "title.keys",
      data: "title keys",
    });
    const skipped = consolePortal();
    await sendToLibrary(skipped.portal, questions([game, "", "", ""]), strings.en);
    expect(skipped.uploads).toHaveLength(1);
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
});

test("missing main package fails before requesting files or changing the console", async () => {
  const { portal, uploads, calls } = consolePortal([], false);
  const prompt = questions([]);
  await expect(sendToLibrary(portal, prompt, strings.en)).rejects.toThrow("libraryMissing");
  expect(prompt.asked).toEqual([]);
  expect(uploads).toEqual([]);
  expect(calls).toHaveLength(1);
});

test("both languages contain every message", () => {
  expect(Object.keys(pt).sort()).toEqual(Object.keys(en).sort());
});
