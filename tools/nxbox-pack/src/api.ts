// Machine interface for the native GUI (tools/nxbox-pack-gui): one JSON request on stdin,
// JSON-lines events on stdout. Same core as the interactive CLI, no prompts.
import { cp, mkdtemp, mkdir, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { basename, extname, join } from "node:path";
import compat from "./compat.json";
import { PackError } from "./i18n";
import { filenameName, parseTitleId, validateFile, validateName, validateTitleId } from "./files";
import { prepareFirmware } from "./firmware";
import { prepareMods, prepareSaves, type Transfer } from "./optional";
import { downloadRuntime, latestRuntime, stageRuntime } from "./runtime";
import { prepareContent } from "./content";
import { discoverXbox } from "./discovery";
import { DevicePortal, portalUrl } from "./portal";
import { loadCredentials, saveCredentials } from "./credentials";
import { deployGame } from "./deploy";

// ponytail: the library app reuses the per-game runtime with a fixed id; give it its own
// identity in the manifest if it ever needs to coexist with a real title 0000000000000000.
const LIBRARY_ID = "0000000000000000";

type Emit = (event: Record<string, unknown>) => void;
export type Job = {
  mode: "game" | "full";
  /** Game mode: exactly one. Full mode: zero or more, all go to the library. */
  games: string[];
  /** Full mode: boot the firmware HOME instead of the NXbox library (needs firmware). */
  officialUi?: boolean;
  keys: string;
  name?: string;
  firmware?: string;
  saves?: string;
  mods?: string[];
  // install = Device Portal; export = loose package folder for a manual install.
  target: { kind: "install"; host: string; user?: string; password?: string; remember?: boolean; insecure?: boolean } | { kind: "export"; out: string };
};

export function compatFor(titleId: string): { status: string; note?: string } {
  return (compat as Record<string, { status: string; note?: string }>)[titleId] ?? { status: "unknown" };
}

export async function inspect(game: string) {
  await validateFile(game);
  // TODO(codex): read name/version/icon from the control NCA instead of the filename.
  const titleId = parseTitleId(game);
  return { name: filenameName(game), titleId, size: Bun.file(game).size, compat: titleId ? compatFor(titleId) : { status: "unknown" } };
}

export async function runJob(job: Job, emit: Emit): Promise<void> {
  const temporary = await mkdtemp(join(tmpdir(), "nxbox-pack-"));
  try {
    const full = job.mode === "full";
    if (!full && job.games.length !== 1) throw new PackError("file");
    for (const game of job.games) await validateFile(game);
    await validateFile(job.keys, true);
    const titleId = full ? LIBRARY_ID : validateTitleId(parseTitleId(job.games[0]!) ?? "");
    const transfers: Transfer[] = [];
    if (job.firmware) {
      emit({ step: "firmware" });
      const fw = await prepareFirmware(job.firmware, job.keys, join(temporary, "firmware"));
      transfers.push(...fw.files.map((f) => ({ path: f.path, destination: `LocalState/firmware/${f.name}` })));
    }
    if (full && job.officialUi) {
      if (!job.firmware) throw new PackError("inputInvalid");
      const settings = join(temporary, "eden_settings.txt");
      await writeFile(settings, "use_official_switch_interface=true\n");
      transfers.push({ path: settings, destination: "LocalState/eden_settings.txt" });
    }
    // Saves and mods are per title, so only the single-game app takes them here.
    if (!full && job.saves) transfers.push(...(await prepareSaves(job.saves, titleId, join(temporary, "saves"))).files);
    if (!full)
      for (const [i, mod] of (job.mods ?? []).entries())
        transfers.push(...(await prepareMods(mod, titleId, join(temporary, `mods-${i}`))).files);

    const name = full ? "NXbox" : validateName(job.name ?? filenameName(job.games[0]!));
    emit({ step: "runtime" });
    const runtimeDir = join(temporary, "runtime");
    const runtimeFiles = await stageRuntime(await downloadRuntime(await latestRuntime()), runtimeDir, titleId, name);
    const content: string[] = [];
    for (const [i, game] of job.games.entries()) {
      emit({ step: "prepare", file: game });
      content.push(await prepareContent(game, join(temporary, `content-${i}`), job.keys));
    }

    if (job.target.kind === "export") {
      const out = join(job.target.out, name);
      await cp(runtimeDir, out, { recursive: true });
      const local = join(out, "LocalState");
      await mkdir(join(local, "games"), { recursive: true });
      await mkdir(join(local, "eden", "keys"), { recursive: true });
      await cp(job.keys, join(local, "eden", "keys", "prod.keys"));
      for (const [i, game] of content.entries()) {
        const file = full ? basename(game) : `base${extname(game)}`;
        await cp(game, join(local, "games", i === 0 || full ? file : `addon-${i}${extname(game)}`));
      }
      if (!full) {
        await writeFile(join(local, "game.txt"), `games\\base${extname(content[0]!)}\n`);
        await writeFile(join(local, "skip_library.txt"), "1\n");
      }
      for (const t of transfers) {
        await mkdir(join(out, t.destination, ".."), { recursive: true });
        await cp(t.path, join(out, t.destination));
      }
      emit({ done: true, path: out });
      return;
    }

    const base = portalUrl(job.target.host);
    const credentials =
      job.target.user && job.target.password ? { user: job.target.user, password: job.target.password } : await loadCredentials(base);
    if (!credentials) throw new PackError("portal");
    const portal = new DevicePortal(base, credentials, job.target.insecure ?? true);
    emit({ step: "connect" });
    await portal.packages();
    if (job.target.remember) await saveCredentials(base, credentials);
    emit({ step: "upload" });
    const identity = await deployGame({
      portal, titleId, runtimeDirectory: runtimeDir, runtimeFiles, content, keys: job.keys, temporary, transfers,
      directBoot: !full,
      onProgress: (file, sent, total) => emit({ step: "upload", file, sent, total }),
    });
    emit({ done: true, identity, name });
  } finally {
    await rm(temporary, { recursive: true, force: true });
  }
}

export async function serveApi(): Promise<void> {
  const emit: Emit = (event) => console.log(JSON.stringify(event));
  try {
    const request = JSON.parse(await Bun.stdin.text());
    if (request.cmd === "inspect") emit(await inspect(request.game));
    else if (request.cmd === "discover") emit({ hosts: await discoverXbox() });
    else if (request.cmd === "run") await runJob(request.job, emit);
    else throw new PackError("args");
  } catch (error) {
    emit({ error: error instanceof PackError ? error.code : String(error) });
    process.exitCode = 1;
  }
}
