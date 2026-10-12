// NXbox Pack GUI: native window over the nxbox-pack core (`nxbox-pack --api`, JSON lines).
// Skeleton: flow and wiring first, styling later.
use eframe::egui;
use serde_json::{json, Value};
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::mpsc::{channel, Receiver};

fn main() -> eframe::Result {
    eframe::run_native(
        "NXbox Pack",
        eframe::NativeOptions {
            viewport: egui::ViewportBuilder::default().with_inner_size([560.0, 720.0]),
            ..Default::default()
        },
        Box::new(|_| Ok(Box::<App>::default())),
    )
}

// TODO(codex): use sys-locale for the device language; NXBOX_PACK_LANG=pt|en forces.
fn pt() -> bool {
    std::env::var("NXBOX_PACK_LANG")
        .or_else(|_| std::env::var("LANG"))
        .map(|l| l.starts_with("pt"))
        .unwrap_or(true)
}
fn t(pt_text: &'static str, en_text: &'static str) -> &'static str {
    if pt() { pt_text } else { en_text }
}

/// Runs one core request on a worker thread; every JSON line it prints arrives on the channel.
fn core(request: Value) -> Receiver<Value> {
    let (tx, rx) = channel();
    std::thread::spawn(move || {
        // NXBOX_PACK_CORE="bun /path/to/cli.ts" in development; the bundled binary otherwise.
        let mut argv: Vec<String> = match std::env::var("NXBOX_PACK_CORE") {
            Ok(cmd) => cmd.split_whitespace().map(String::from).collect(),
            Err(_) => {
                let exe = std::env::current_exe().unwrap_or_default();
                let name = if cfg!(windows) { "nxbox-pack.exe" } else { "nxbox-pack" };
                vec![exe.with_file_name(name).to_string_lossy().into_owned()]
            }
        };
        argv.push("--api".into());
        let child = Command::new(&argv[0])
            .args(&argv[1..])
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn();
        let mut child = match child {
            Ok(c) => c,
            Err(e) => {
                let _ = tx.send(json!({ "error": format!("core: {e}") }));
                return;
            }
        };
        let _ = child.stdin.take().unwrap().write_all(request.to_string().as_bytes());
        for line in BufReader::new(child.stdout.take().unwrap()).lines().map_while(Result::ok) {
            if let Ok(v) = serde_json::from_str(&line) {
                let _ = tx.send(v);
            }
        }
        let _ = child.wait();
    });
    rx
}

#[derive(PartialEq, Clone, Copy)]
enum Mode {
    Game,
    Full,
}

#[derive(Default)]
struct App {
    mode: Option<Mode>,
    game: Option<PathBuf>,
    game_info: Option<Value>,
    keys: Option<PathBuf>,
    firmware: Option<PathBuf>,
    saves: Option<PathBuf>,
    mods: Vec<PathBuf>,
    manual: bool,
    host: String,
    user: String,
    password: String,
    remember: bool,
    out: Option<PathBuf>,
    inspect_rx: Option<Receiver<Value>>,
    discover_rx: Option<Receiver<Value>>,
    run_rx: Option<Receiver<Value>>,
    status: String,
    progress: Option<f32>,
    done: Option<String>,
    error: Option<String>,
}

fn is_game(p: &Path) -> bool {
    matches!(
        p.extension().and_then(|e| e.to_str()).map(str::to_lowercase).as_deref(),
        Some("nsp" | "xci" | "nsz" | "xcz")
    )
}

impl App {
    fn set_game(&mut self, path: PathBuf) {
        self.inspect_rx = Some(core(json!({ "cmd": "inspect", "game": path })));
        self.game = Some(path);
        self.game_info = None;
    }

    fn take_drops(&mut self, ctx: &egui::Context) {
        for file in ctx.input(|i| i.raw.dropped_files.clone()) {
            let path = file.path().to_path_buf();
            if path.as_os_str().is_empty() {
                continue;
            }
            if is_game(&path) {
                self.set_game(path);
            } else if path.file_name().is_some_and(|n| n.eq_ignore_ascii_case("prod.keys")) {
                self.keys = Some(path);
            } else if path.is_dir() || path.extension().is_some_and(|e| e == "zip" || e == "bin") {
                // ponytail: folders/zips/NAND images are assumed to be firmware; saves and mods use their buttons.
                self.firmware = Some(path);
            }
        }
    }

    fn poll(&mut self) {
        if let Some(v) = self.inspect_rx.as_ref().and_then(|rx| rx.try_recv().ok()) {
            self.game_info = Some(v);
        }
        if let Some(v) = self.discover_rx.as_ref().and_then(|rx| rx.try_recv().ok()) {
            if self.host.is_empty() {
                if let Some(h) = v["hosts"].get(0).and_then(Value::as_str) {
                    self.host = h.into();
                }
            }
        }
        while let Some(v) = self.run_rx.as_ref().and_then(|rx| rx.try_recv().ok()) {
            if let Some(e) = v["error"].as_str() {
                self.error = Some(e.into());
            } else if v["done"].as_bool() == Some(true) {
                self.done = Some(v["path"].as_str().or(v["name"].as_str()).unwrap_or("").into());
            } else if let Some(step) = v["step"].as_str() {
                self.status = match v["file"].as_str() {
                    Some(f) => format!("{step}: {f}"),
                    None => step.into(),
                };
                self.progress = match (v["sent"].as_f64(), v["total"].as_f64()) {
                    (Some(s), Some(t)) if t > 0.0 => Some((s / t) as f32),
                    _ => None,
                };
            }
        }
    }

    fn start(&mut self) {
        let target = if self.manual {
            json!({ "kind": "export", "out": self.out })
        } else {
            json!({
                "kind": "install", "host": self.host, "user": self.user,
                "password": self.password, "remember": self.remember,
            })
        };
        let job = json!({
            "mode": if self.mode == Some(Mode::Full) { "full" } else { "game" },
            "game": self.game, "keys": self.keys, "firmware": self.firmware,
            "saves": self.saves, "mods": self.mods, "target": target,
        });
        self.error = None;
        self.done = None;
        self.status = t("Começando…", "Starting…").into();
        self.run_rx = Some(core(json!({ "cmd": "run", "job": job })));
    }
}

fn pick_row(ui: &mut egui::Ui, label: &str, value: &Option<PathBuf>, folder: bool) -> Option<PathBuf> {
    let mut picked = None;
    ui.horizontal(|ui| {
        ui.label(label);
        let shown = value
            .as_ref()
            .and_then(|p| p.file_name())
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_else(|| t("— arraste aqui ou escolha", "— drop here or choose").into());
        ui.monospace(shown);
        if ui.button(t("Escolher…", "Choose…")).clicked() {
            let dialog = rfd::FileDialog::new();
            picked = if folder { dialog.pick_folder() } else { dialog.pick_file() };
        }
    });
    picked
}

impl eframe::App for App {
    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        let ctx = ui.ctx().clone();
        let ctx = &ctx;
        self.take_drops(ctx);
        self.poll();
        if self.inspect_rx.is_some() || self.run_rx.is_some() || self.discover_rx.is_some() {
            ctx.request_repaint_after(std::time::Duration::from_millis(100));
        }

        egui::CentralPanel::default().show(ui, |ui| {
            egui::ScrollArea::vertical().show(ui, |ui| {
                ui.heading("NXbox Pack");
                ui.add_space(12.0);

                // 1. Mode
                ui.label(t("O QUE INSTALAR", "WHAT TO INSTALL"));
                ui.horizontal(|ui| {
                    ui.selectable_value(&mut self.mode, Some(Mode::Game), t("Só o jogo", "Just the game"));
                    ui.selectable_value(&mut self.mode, Some(Mode::Full), t("NXbox completo", "Full NXbox"));
                });
                ui.small(match self.mode {
                    Some(Mode::Game) => t("Um app próprio que abre direto no jogo.", "Its own app that boots straight into the game."),
                    Some(Mode::Full) => t("A biblioteca do NXbox com todos os jogos.", "The NXbox library with all games."),
                    None => t("Escolha um para continuar.", "Pick one to continue."),
                });
                let Some(mode) = self.mode else { return };
                ui.separator();

                // 2. Files
                ui.label(t("ARQUIVOS", "FILES"));
                if let Some(p) = pick_row(ui, t("Jogo", "Game"), &self.game, false) {
                    self.set_game(p);
                }
                match &self.game_info {
                    Some(info) if info.get("error").is_some() => {
                        ui.colored_label(egui::Color32::RED, t("Arquivo de jogo inválido.", "Invalid game file."));
                    }
                    Some(info) => {
                        ui.label(format!(
                            "{}  ·  {}",
                            info["name"].as_str().unwrap_or("?"),
                            info["titleId"].as_str().unwrap_or(t("ID desconhecido", "unknown ID"))
                        ));
                        let status = info["compat"]["status"].as_str().unwrap_or("unknown");
                        let (color, text) = match status {
                            "playable" => (egui::Color32::GREEN, t("Jogável", "Playable")),
                            "ingame" => (egui::Color32::YELLOW, t("Entra no jogo, com problemas", "In-game, with issues")),
                            "boots" => (egui::Color32::ORANGE, t("Só abre", "Boots only")),
                            "broken" => (egui::Color32::RED, t("Não funciona ainda", "Does not work yet")),
                            _ => (egui::Color32::GRAY, t("Ainda não testado", "Not tested yet")),
                        };
                        ui.colored_label(color, text);
                        if let Some(note) = info["compat"]["note"].as_str() {
                            ui.small(note);
                        }
                    }
                    None if self.game.is_some() => {
                        ui.spinner();
                    }
                    None => {}
                }
                if let Some(p) = pick_row(ui, "prod.keys", &self.keys, false) {
                    self.keys = Some(p);
                }
                let fw_label = if mode == Mode::Full {
                    t("Firmware (para a interface oficial)", "Firmware (for the official interface)")
                } else {
                    t("Firmware (opcional)", "Firmware (optional)")
                };
                if let Some(p) = pick_row(ui, fw_label, &self.firmware, true) {
                    self.firmware = Some(p);
                }
                if let Some(p) = pick_row(ui, t("Saves (opcional)", "Saves (optional)"), &self.saves, true) {
                    self.saves = Some(p);
                }
                ui.horizontal(|ui| {
                    ui.label(format!("Mods: {}", self.mods.len()));
                    if ui.button(t("Adicionar…", "Add…")).clicked() {
                        if let Some(p) = rfd::FileDialog::new().pick_folder() {
                            self.mods.push(p);
                        }
                    }
                });
                if self.game.is_none() || self.keys.is_none() {
                    ui.small(t("Arraste o jogo e o prod.keys para a janela.", "Drop the game and prod.keys onto the window."));
                    return;
                }
                ui.separator();

                // 3. Install
                ui.label(t("INSTALAR", "INSTALL"));
                ui.horizontal(|ui| {
                    ui.radio_value(&mut self.manual, false, t("Direto no Xbox", "Straight to the Xbox"));
                    ui.radio_value(&mut self.manual, true, t("Prefiro manualmente", "I'll do it manually"));
                });
                let ready = if self.manual {
                    if let Some(p) = pick_row(ui, t("Salvar em", "Save to"), &self.out, true) {
                        self.out = Some(p);
                    }
                    ui.small(t(
                        "Gera a pasta do app (UWP) com o jogo dentro, para instalar pelo Device Portal.",
                        "Builds the app (UWP) folder with the game inside, to install through Device Portal.",
                    ));
                    self.out.is_some()
                } else {
                    if self.discover_rx.is_none() {
                        self.discover_rx = Some(core(json!({ "cmd": "discover" })));
                    }
                    ui.horizontal(|ui| {
                        ui.label(t("IP do Xbox", "Xbox IP"));
                        ui.text_edit_singleline(&mut self.host);
                    });
                    ui.horizontal(|ui| {
                        ui.label(t("Usuário", "User"));
                        ui.text_edit_singleline(&mut self.user);
                    });
                    ui.horizontal(|ui| {
                        ui.label(t("Senha", "Password"));
                        ui.add(egui::TextEdit::singleline(&mut self.password).password(true));
                    });
                    ui.checkbox(&mut self.remember, t("Lembrar neste computador", "Remember on this computer"));
                    ui.small(t(
                        "Usuário e senha do Device Portal (Dev Home > Remote Access). Vazio se já salvou antes.",
                        "Device Portal user and password (Dev Home > Remote Access). Leave empty if saved before.",
                    ));
                    !self.host.is_empty()
                };

                let running = self.run_rx.is_some() && self.done.is_none() && self.error.is_none();
                ui.add_space(8.0);
                if ui
                    .add_enabled(ready && !running, egui::Button::new(t("Instalar", "Install")))
                    .clicked()
                {
                    self.start();
                }
                if running {
                    ui.label(&self.status);
                    match self.progress {
                        Some(p) => ui.add(egui::ProgressBar::new(p).show_percentage()),
                        None => ui.spinner(),
                    };
                }
                if let Some(e) = &self.error {
                    ui.colored_label(egui::Color32::RED, format!("{}: {e}", t("Erro", "Error")));
                }
                if let Some(d) = &self.done {
                    ui.colored_label(
                        egui::Color32::GREEN,
                        format!("{} {d}", t("Pronto! Abra no Xbox:", "Done! Open it on the Xbox:")),
                    );
                }
            });
        });
    }
}
