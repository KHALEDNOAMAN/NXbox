// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

import { createReadStream } from "node:fs";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { basename, join, posix } from "node:path";
import { Readable } from "node:stream";
import { PackError } from "./i18n";

/**
 * The console answers 500 after the transfer of a single multi-GB file (measured at ~3.6 GB), so
 * bigger files go up in slices that NXbox joins on its next start (src/frontend_common/split_upload.h).
 */
export const PART_SIZE = 1024 ** 3;
/** Pure upload plan shared by both modes; the manifest is always the last upload. */
export function uploadPlan(name: string, size: number, partSize = PART_SIZE) {
  if (size <= partSize) return { files: [{ name, start: 0, end: size - 1 }], manifest: undefined };
  const parts = Math.ceil(size / partSize);
  return {
    files: Array.from({ length: parts }, (_, index) => ({
      name: `${name}.nxpart${String(index).padStart(3, "0")}`,
      start: index * partSize,
      end: Math.min((index + 1) * partSize, size) - 1,
    })),
    manifest: { name: `${name}.nxparts`, text: `parts=${parts}\nsize=${size}\n` },
  };
}
export type Credentials = { user: string; password: string };
export type InstalledPackage = {
  Name: string;
  PackageFullName: string;
  PackageRelativeId?: string;
};
export function portalUrl(host: string): string {
  const url = new URL(
    host.includes("://") ? host : `https://${host}${host.includes(":") ? "" : ":11443"}`,
  );
  if (
    url.protocol !== "https:" ||
    url.username ||
    url.password ||
    url.pathname !== "/" ||
    url.search ||
    url.hash
  )
    throw new PackError("portal");
  return url.origin;
}
export class DevicePortal {
  private csrf = "";
  constructor(
    readonly base: string,
    private readonly credentials: Credentials,
    private readonly insecure = false,
    private readonly transport: typeof fetch = fetch,
  ) {}
  private async raw(method: string, path: string, init: RequestInit = {}): Promise<Response> {
    const headers = new Headers(init.headers);
    // Device Portal can run without authentication; then no header is sent.
    if (this.credentials.user)
      headers.set(
        "Authorization",
        `Basic ${Buffer.from(`${this.credentials.user}:${this.credentials.password}`).toString("base64")}`,
      );
    if (this.csrf) {
      headers.set("Cookie", `CSRF-Token=${this.csrf}`);
      headers.set("X-CSRF-Token", this.csrf);
    }
    const response = await this.transport(`${this.base}${path}`, {
      ...init,
      method,
      headers,
      redirect: "manual",
      signal: init.signal ?? AbortSignal.timeout(60_000),
      tls: { rejectUnauthorized: !this.insecure },
    } as RequestInit);
    const cookie = response.headers.get("set-cookie")?.match(/CSRF-Token=([^;,\s]+)/i)?.[1];
    if (cookie) this.csrf = cookie;
    if (!response.ok)
      throw new PackError("portal", {
        cause: new Error(`${method} ${path.split("?")[0]}: HTTP ${response.status}`),
      });
    return response;
  }
  async request(method: string, path: string, init: RequestInit = {}): Promise<Response> {
    if (method !== "GET" && !this.csrf) await this.raw("GET", "/api/os/machinename");
    return this.raw(method, path, init);
  }
  /**
   * Dev Home installs UWP packages as Apps, capped at 1 GB, unless this console setting is on;
   * games then crash with bad_alloc. Turns it on and reports whether it had to.
   */
  async installAsGames(): Promise<boolean> {
    const data = (await (await this.request("GET", "/ext/settings")).json()) as {
      Settings?: { Name?: string; Value?: string; Type?: string }[];
    };
    // Measured on Series X OS 10.0.26100; older builds called it DefaultAppMode.
    const setting = data.Settings?.find(
      (s) => s.Name === "DefaultUWPContentTypeToGame" || s.Name === "DefaultAppMode",
    );
    if (!setting?.Name) return false;
    const value = setting.Type === "Bool" || setting.Name.endsWith("ToGame") ? "true" : "Game";
    if (String(setting.Value).toLowerCase() === value.toLowerCase()) return false;
    const response = await this.request(
      "PUT",
      `/ext/settings/${encodeURIComponent(setting.Name)}`,
      {
        body: JSON.stringify({ Value: value }),
        headers: { "Content-Type": "application/json" },
      },
    );
    if (!response.ok) throw new Error(`Device Portal refused ${setting.Name}: ${response.status}`);
    return true;
  }
  async packages(): Promise<InstalledPackage[]> {
    const data = (await (await this.request("GET", "/api/app/packagemanager/packages")).json()) as {
      InstalledPackages?: InstalledPackage[];
    };
    return data.InstalledPackages ?? [];
  }
  async register(
    folder: string,
    identity: string,
    timeout = 15 * 60_000,
  ): Promise<InstalledPackage> {
    // Device Portal reports the display name in Name, so match the identity prefix of the
    // full name, and skip the previous version that stays listed until the upgrade lands.
    const before = new Set((await this.packages()).map((p) => p.PackageFullName));
    await this.request(
      "POST",
      `/api/app/packagemanager/register?${new URLSearchParams({ folder: Buffer.from(folder).toString("base64") })}`,
    );
    const deadline = Date.now() + timeout;
    while (Date.now() < deadline) {
      await Bun.sleep(1000);
      const state = await this.request("GET", "/api/app/packagemanager/state");
      if (state.status === 204) continue;
      const text = await state.text();
      if (text.trim()) {
        const result = JSON.parse(text) as { Code?: number; Success?: boolean };
        if (result.Success === false || (result.Code !== undefined && Number(result.Code) !== 0))
          throw new PackError("deploy", { cause: new Error(String(text)) });
      }
      const installed = (await this.packages()).find(
        (p) => p.PackageFullName.startsWith(`${identity}_`) && !before.has(p.PackageFullName),
      );
      if (installed) return installed;
    }
    throw new PackError("deploy");
  }
  private async sendFiles(
    endpoint: string,
    files: { path: string; name: string; start?: number; end?: number }[],
    onProgress?: (bytes: number, total: number) => void,
  ): Promise<void> {
    // Stream multipart bodies with backpressure; never buffer multi-GB dumps in FormData.
    for (const { path, name } of files)
      if (!(await Bun.file(path).exists()) || /["\r\n\x00]/.test(name)) throw new PackError("file");
    const boundary = `nxbox-${crypto.randomUUID()}`;
    const head = (name: string) =>
      Buffer.from(
        `--${boundary}\r\nContent-Disposition: form-data; name="file"; filename="${name}"\r\nContent-Type: application/octet-stream\r\n\r\n`,
      );
    const suffix = Buffer.from(`--${boundary}--\r\n`);
    // An inclusive byte range streams a slice straight from the original file.
    const sizeOf = ({ path, start = 0, end }: { path: string; start?: number; end?: number }) =>
      end === undefined ? Bun.file(path).size - start : end - start + 1;
    const total = files.reduce((sum, file) => sum + sizeOf(file), 0);
    const length = files.reduce(
      (sum, file) => sum + head(file.name).length + sizeOf(file) + 2,
      suffix.length,
    );
    async function* body() {
      let sent = 0;
      for (const { path, name, start, end } of files) {
        yield head(name);
        for await (const chunk of createReadStream(path, {
          highWaterMark: 1024 * 1024,
          start,
          end,
        })) {
          sent += chunk.length;
          onProgress?.(sent, total);
          yield chunk;
        }
        yield Buffer.from("\r\n");
      }
      yield suffix;
    }
    await this.request("POST", endpoint, {
      headers: {
        "Content-Type": `multipart/form-data; boundary=${boundary}`,
        "Content-Length": String(length),
      },
      body: Readable.toWeb(Readable.from(body())) as unknown as ReadableStream,
      signal: AbortSignal.timeout(12 * 60 * 60_000),
    });
  }
  /** Each upload replaces the destination folder's files, so one request carries a whole
   * folder, and the package root goes first. */
  async uploadRuntime(folder: string, directory: string, relativePaths: string[]): Promise<void> {
    const groups = new Map<string, string[]>();
    for (const relativePath of relativePaths) {
      const parent = posix.dirname(relativePath.replaceAll("\\", "/"));
      groups.set(parent, [...(groups.get(parent) ?? []), relativePath]);
    }
    const parents = [...groups.keys()].sort((a, b) =>
      a === "." ? -1 : b === "." ? 1 : a.localeCompare(b),
    );
    for (const parent of parents) {
      const destination = [folder, parent === "." ? "" : parent]
        .filter(Boolean)
        .join("\\")
        .replaceAll("/", "\\");
      await this.sendFiles(
        `/api/app/packagemanager/upload?${new URLSearchParams({ destinationFolder: Buffer.from(destination).toString("base64") })}`,
        groups.get(parent)!.map((file) => ({ path: join(directory, file), name: basename(file) })),
      );
    }
  }
  private query(pkg: string, path: string): URLSearchParams {
    return new URLSearchParams({
      knownfolderid: "LocalAppData",
      packagefullname: pkg,
      // The console silently ignores folder creation and rejects uploads on "/" paths.
      path: path ? `\\${path.replace(/^[/\\]+/, "").replaceAll("/", "\\")}` : "",
    });
  }
  async readFile(pkg: string, directory: string, name: string): Promise<Uint8Array> {
    const query = this.query(pkg, directory);
    query.set("filename", name);
    return new Uint8Array(
      await (await this.request("GET", `/api/filesystem/apps/file?${query}`)).arrayBuffer(),
    );
  }
  async listFiles(pkg: string, path: string): Promise<{ Name: string }[]> {
    const response = await this.raw("GET", `/api/filesystem/apps/files?${this.query(pkg, path)}`);
    const data = (await response.json()) as { Items?: { Name: string }[] };
    return data.Items ?? [];
  }
  async makeFolder(pkg: string, parent: string, name: string): Promise<void> {
    const query = this.query(pkg, parent);
    query.set("newfoldername", name);
    try {
      await this.request("POST", `/api/filesystem/apps/folder?${query}`);
    } catch (cause) {
      // Only ignore a create failure after proving that exact directory exists.
      const data = (await (
        await this.request("GET", `/api/filesystem/apps/files?${this.query(pkg, parent)}`)
      ).json()) as { Items?: { Name: string; Type?: number }[] };
      if (!data.Items?.some((item) => item.Name === name)) throw cause;
    }
  }
  /** Uploads one file (or one slice of it) and checks the console reports the same size. */
  private async pushOne(
    pkg: string,
    directory: string,
    file: { path: string; name: string; start?: number; end?: number },
    size: number,
    onProgress?: (bytes: number, total: number) => void,
  ): Promise<void> {
    const query = this.query(pkg, directory);
    query.set("extract", "false");
    await this.sendFiles(`/api/filesystem/apps/file?${query}`, [file], onProgress);
    const data = (await (
      await this.request("GET", `/api/filesystem/apps/files?${query}`)
    ).json()) as { Items?: { Name: string; FileSize?: number; SizeInBytes?: number }[] };
    const item = data.Items?.find((item) => item.Name === file.name);
    if (!item || Number(item.FileSize ?? item.SizeInBytes) !== size) throw new PackError("portal");
  }
  /**
   * Files above partSize go up as `<name>.nxpart000`, `.nxpart001`, ... and a last
   * `<name>.nxparts` manifest, so the manifest's presence means every part is on the console.
   */
  async pushFile(
    pkg: string,
    path: string,
    directory: string,
    remoteName = basename(path),
    onProgress?: (bytes: number, total: number) => void,
    partSize = PART_SIZE,
  ): Promise<void> {
    const size = Bun.file(path).size;
    const plan = uploadPlan(remoteName, size, partSize);
    if (!plan.manifest) {
      await this.pushOne(pkg, directory, { path, name: remoteName }, size, onProgress);
      return;
    }
    for (const file of plan.files) {
      await this.pushOne(
        pkg,
        directory,
        { path, ...file },
        file.end - file.start + 1,
        // Report progress of the whole file, not of the current slice.
        (sent) => onProgress?.(file.start + sent, size),
      );
    }
    const temporary = await mkdtemp(join(tmpdir(), "nxbox-pack-parts-"));
    try {
      const manifest = join(temporary, plan.manifest.name);
      const text = plan.manifest.text;
      await writeFile(manifest, text);
      await this.pushOne(
        pkg,
        directory,
        { path: manifest, name: plan.manifest.name },
        Buffer.byteLength(text),
      );
    } finally {
      await rm(temporary, { recursive: true, force: true });
    }
  }
}
