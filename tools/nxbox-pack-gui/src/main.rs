// NXbox Pack GUI: a step-by-step installer over the nxbox-pack core (`nxbox-pack --api`, JSON lines).
// Skeleton: flow, texts and wiring first; visual design comes later.
use eframe::egui;
use serde_json::{json, Value};
use std::collections::HashMap;
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::mpsc::{channel, Receiver};

fn main() -> eframe::Result {
    eframe::run_native(
        "NXbox Pack",
        eframe::NativeOptions {
            viewport: egui::ViewportBuilder::default().with_inner_size([640.0, 600.0]),
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

#[derive(PartialEq, Clone, Copy, Debug)]
enum Page {
    Welcome,
    Game,
    Keys,
    Firmware,
    Games,
    Extras,
    Install,
    Progress,
}

fn flow(mode: Option<Mode>) -> &'static [Page] {
    match mode {
        Some(Mode::Game) => &[Page::Welcome, Page::Game, Page::Keys, Page::Extras, Page::Install, Page::Progress],
        Some(Mode::Full) => &[Page::Welcome, Page::Keys, Page::Firmware, Page::Games, Page::Install, Page::Progress],
        None => &[Page::Welcome],
    }
}

#[derive(Default)]
struct App {
    page_index: usize,
    mode: Option<Mode>,
    games: Vec<PathBuf>,
    info: HashMap<PathBuf, Value>,
    inspecting: Vec<(PathBuf, Receiver<Value>)>,
    keys: Option<PathBuf>,
    firmware: Option<PathBuf>,
    official_ui: bool,
    saves: Option<PathBuf>,
    mods: Vec<PathBuf>,
    manual: bool,
    host: String,
    user: String,
    password: String,
    remember: bool,
    out: Option<PathBuf>,
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

fn file_name(p: &Path) -> String {
    p.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default()
}

fn heading(ui: &mut egui::Ui, title: &str, body: &str) {
    ui.heading(title);
    ui.add_space(6.0);
    ui.label(body);
    ui.add_space(14.0);
}

fn choose_file(ui: &mut egui::Ui, label: &str, folder: bool) -> Option<PathBuf> {
    if ui.button(label).clicked() {
        let dialog = rfd::FileDialog::new();
        return if folder { dialog.pick_folder() } else { dialog.pick_file() };
    }
    None
}

fn compat_text(status: &str) -> (egui::Color32, &'static str, &'static str) {
    match status {
        "playable" => (egui::Color32::GREEN, t("Jogável", "Playable"), t("Funciona do início ao fim no Xbox.", "Works from start to finish on Xbox.")),
        "ingame" => (egui::Color32::YELLOW, t("Funciona com problemas", "Works with issues"), t("Chega ao jogo, mas há falhas gráficas ou de desempenho.", "Reaches gameplay, but has graphics or performance issues.")),
        "boots" => (egui::Color32::ORANGE, t("Apenas abre", "Boots only"), t("Abre, mas ainda não chega à jogabilidade.", "Opens, but does not reach gameplay yet.")),
        "broken" => (egui::Color32::RED, t("Ainda não funciona", "Does not work yet"), t("Não é possível jogar no momento.", "Cannot be played at the moment.")),
        _ => (egui::Color32::GRAY, t("Ainda não testado", "Not tested yet"), t("Ninguém mediu este jogo no Xbox; pode ou não funcionar.", "This game has not been measured on Xbox; it may or may not work.")),
    }
}

impl App {
    fn page(&self) -> Page {
        flow(self.mode)[self.page_index.min(flow(self.mode).len() - 1)]
    }

    fn add_game(&mut self, path: PathBuf) {
        if self.mode == Some(Mode::Game) {
            self.games.clear();
        }
        if !self.games.contains(&path) {
            self.inspecting.push((path.clone(), core(json!({ "cmd": "inspect", "game": path }))));
            self.games.push(path);
        }
    }

    fn take_drops(&mut self, ctx: &egui::Context) {
        for file in ctx.input(|i| i.raw.dropped_files.clone()) {
            let path = file.path().to_path_buf();
            if path.as_os_str().is_empty() {
                continue;
            }
            if is_game(&path) {
                self.add_game(path);
            } else if path.file_name().is_some_and(|n| n.eq_ignore_ascii_case("prod.keys")) {
                self.keys = Some(path);
            } else if self.page() == Page::Firmware || (self.page() == Page::Extras && self.firmware.is_none()) {
                self.firmware = Some(path);
            }
        }
    }

    fn poll(&mut self) {
        let mut finished = Vec::new();
        for (i, (path, rx)) in self.inspecting.iter().enumerate() {
            if let Ok(v) = rx.try_recv() {
                self.info.insert(path.clone(), v);
                finished.push(i);
            }
        }
        for i in finished.into_iter().rev() {
            self.inspecting.remove(i);
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
                let step = match step {
                    "firmware" => t("Verificando o firmware", "Verifying the firmware"),
                    "runtime" => t("Baixando o NXbox", "Downloading NXbox"),
                    "prepare" => t("Preparando o jogo", "Preparing the game"),
                    "connect" => t("Conectando ao Xbox", "Connecting to the Xbox"),
                    "upload" => t("Enviando ao Xbox", "Sending to the Xbox"),
                    other => if other.is_empty() { "" } else { t("Trabalhando", "Working") },
                };
                self.status = match v["file"].as_str() {
                    Some(f) => format!("{step}: {}", file_name(Path::new(f))),
                    None => step.into(),
                };
                self.progress = match (v["sent"].as_f64(), v["total"].as_f64()) {
                    (Some(s), Some(t)) if t > 0.0 => Some((s / t) as f32),
                    _ => None,
                };
            }
        }
    }

    fn can_continue(&self) -> bool {
        match self.page() {
            Page::Welcome => self.mode.is_some(),
            Page::Game => self.games.len() == 1 && self.info.get(&self.games[0]).is_some_and(|i| i.get("error").is_none()),
            Page::Keys => self.keys.is_some(),
            Page::Firmware | Page::Games | Page::Extras => true,
            Page::Install => if self.manual { self.out.is_some() } else { !self.host.trim().is_empty() },
            Page::Progress => false,
        }
    }

    fn start(&mut self) {
        let full = self.mode == Some(Mode::Full);
        let target = if self.manual {
            json!({ "kind": "export", "out": self.out })
        } else {
            json!({
                "kind": "install", "host": self.host.trim(), "user": self.user,
                "password": self.password, "remember": self.remember,
            })
        };
        let job = json!({
            "mode": if full { "full" } else { "game" },
            "games": self.games, "keys": self.keys, "firmware": self.firmware,
            "officialUi": full && self.official_ui && self.firmware.is_some(),
            "saves": self.saves, "mods": self.mods, "target": target,
        });
        self.error = None;
        self.done = None;
        self.progress = None;
        self.status = t("Iniciando a instalação", "Starting the installation").into();
        self.run_rx = Some(core(json!({ "cmd": "run", "job": job })));
    }

    fn game_card(&self, ui: &mut egui::Ui, path: &Path) {
        ui.strong(file_name(path));
        match self.info.get(path) {
            None => {
                ui.horizontal(|ui| {
                    ui.spinner();
                    ui.label(t("Identificando o jogo…", "Identifying the game…"));
                });
            }
            Some(info) if info.get("error").is_some() => {
                ui.colored_label(
                    egui::Color32::RED,
                    t(
                        "Este arquivo não pôde ser lido como um jogo de Switch. Confira se é um .nsp, .xci, .nsz ou .xcz completo.",
                        "This file could not be read as a Switch game. Check that it is a complete .nsp, .xci, .nsz or .xcz.",
                    ),
                );
            }
            Some(info) => {
                ui.label(format!(
                    "{}  ·  {} {}",
                    info["name"].as_str().unwrap_or("?"),
                    t("ID", "ID"),
                    info["titleId"].as_str().unwrap_or(t("desconhecido", "unknown"))
                ));
                let (color, label, meaning) = compat_text(info["compat"]["status"].as_str().unwrap_or(""));
                ui.horizontal(|ui| {
                    ui.colored_label(color, label);
                    ui.label(format!("— {meaning}"));
                });
                if let Some(note) = info["compat"]["note"].as_str() {
                    ui.small(note);
                }
            }
        }
    }

    fn show_page(&mut self, ui: &mut egui::Ui) {
        match self.page() {
            Page::Welcome => {
                heading(
                    ui,
                    t("Bem-vindo ao NXbox Pack", "Welcome to NXbox Pack"),
                    t(
                        "Este assistente prepara e instala o NXbox no seu Xbox Series X|S. Cada etapa explica o que é necessário; nada é enviado a lugar algum além do seu próprio Xbox.",
                        "This assistant prepares and installs NXbox on your Xbox Series X|S. Each step explains what is needed; nothing is sent anywhere except your own Xbox.",
                    ),
                );
                ui.label(t("O que você deseja instalar?", "What would you like to install?"));
                ui.add_space(6.0);
                ui.radio_value(&mut self.mode, Some(Mode::Game), t("Somente um jogo", "A single game"));
                ui.indent("game", |ui| {
                    ui.label(t(
                        "Cria um aplicativo próprio para um jogo. Ele aparece como um bloco separado no Xbox e abre diretamente no jogo, sem passar pela biblioteca do emulador.",
                        "Creates a dedicated app for one game. It appears as its own tile on the Xbox and opens straight into the game, without going through the emulator library.",
                    ));
                });
                ui.add_space(8.0);
                ui.radio_value(&mut self.mode, Some(Mode::Full), t("NXbox completo", "Full NXbox"));
                ui.indent("full", |ui| {
                    ui.label(t(
                        "Instala o emulador com a biblioteca, onde é possível manter vários jogos, configurar controles, saves e mods e, opcionalmente, usar a interface oficial do Switch.",
                        "Installs the emulator with its library, where you can keep several games, configure controllers, saves and mods and, optionally, use the official Switch interface.",
                    ));
                });
                ui.add_space(16.0);
                ui.small(t(
                    "Requisitos: Xbox em Modo Desenvolvedor com o Device Portal ativado (Dev Home > Remote Access) e arquivos do seu próprio Nintendo Switch.",
                    "Requirements: an Xbox in Developer Mode with Device Portal enabled (Dev Home > Remote Access) and files from your own Nintendo Switch.",
                ));
            }
            Page::Game => {
                heading(
                    ui,
                    t("Selecione o jogo", "Select the game"),
                    t(
                        "Arraste o arquivo do jogo para esta janela ou clique em Escolher arquivo. São aceitos arquivos .nsp, .xci, .nsz e .xcz. O NXbox identifica o título e informa o estado de compatibilidade medido no Xbox.",
                        "Drag the game file onto this window or click Choose file. .nsp, .xci, .nsz and .xcz files are accepted. NXbox identifies the title and shows its compatibility status as measured on Xbox.",
                    ),
                );
                if let Some(p) = choose_file(ui, t("Escolher arquivo…", "Choose file…"), false) {
                    self.add_game(p);
                }
                ui.add_space(10.0);
                if let Some(game) = self.games.first().cloned() {
                    self.game_card(ui, &game);
                }
            }
            Page::Keys => {
                heading(
                    ui,
                    t("Chaves do console (prod.keys)", "Console keys (prod.keys)"),
                    t(
                        "O NXbox precisa das chaves extraídas do seu próprio Nintendo Switch para ler jogos e firmware. O arquivo é copiado somente para o seu Xbox. Para obtê-lo, use o Lockpick_RCM no seu console.",
                        "NXbox needs the keys dumped from your own Nintendo Switch to read games and firmware. The file is copied only to your Xbox. To obtain it, run Lockpick_RCM on your console.",
                    ),
                );
                if let Some(p) = choose_file(ui, t("Escolher prod.keys…", "Choose prod.keys…"), false) {
                    self.keys = Some(p);
                }
                ui.add_space(8.0);
                match &self.keys {
                    Some(k) if file_name(k).eq_ignore_ascii_case("prod.keys") => {
                        ui.colored_label(egui::Color32::GREEN, format!("✓ {}", k.display()));
                    }
                    Some(_) => {
                        ui.colored_label(egui::Color32::RED, t("O arquivo precisa se chamar prod.keys.", "The file must be named prod.keys."));
                    }
                    None => {
                        ui.label(t("Nenhum arquivo selecionado.", "No file selected."));
                    }
                }
            }
            Page::Firmware => {
                heading(
                    ui,
                    t("Firmware do Switch (opcional)", "Switch firmware (optional)"),
                    t(
                        "Sem firmware, os jogos funcionam normalmente: o NXbox substitui os serviços do sistema. Com o firmware do seu console, recursos do sistema passam a funcionar como no Switch original, como o editor de Mii e o menu HOME. Aceita a pasta de firmware, um arquivo .zip ou um backup da NAND.",
                        "Without firmware, games work normally: NXbox replaces the system services. With your console's firmware, system features behave as on the original Switch, such as the Mii editor and the HOME menu. Accepts the firmware folder, a .zip file or a NAND backup.",
                    ),
                );
                ui.horizontal(|ui| {
                    if let Some(p) = choose_file(ui, t("Escolher pasta…", "Choose folder…"), true) {
                        self.firmware = Some(p);
                    }
                    if let Some(p) = choose_file(ui, t("Escolher arquivo…", "Choose file…"), false) {
                        self.firmware = Some(p);
                    }
                    if self.firmware.is_some() && ui.button(t("Remover", "Remove")).clicked() {
                        self.firmware = None;
                    }
                });
                ui.label(match &self.firmware {
                    Some(f) => f.display().to_string(),
                    None => t("Nenhum firmware selecionado.", "No firmware selected.").into(),
                });
                ui.add_space(14.0);
                ui.add_enabled_ui(self.firmware.is_some(), |ui| {
                    ui.checkbox(&mut self.official_ui, t("Usar a interface oficial do Switch", "Use the official Switch interface"));
                    ui.indent("ui", |ui| {
                        ui.label(t(
                            "Ao abrir o NXbox, o menu HOME do Switch aparece no lugar da biblioteca. As opções do NXbox continuam disponíveis pressionando Start + Select duas vezes. Pode ser alterado depois nas configurações do NXbox.",
                            "When NXbox opens, the Switch HOME menu is shown instead of the library. NXbox options remain available by pressing Start + Select twice. This can be changed later in the NXbox settings.",
                        ));
                    });
                });
                if self.firmware.is_none() {
                    ui.small(t("Requer firmware.", "Requires firmware."));
                }
            }
            Page::Games => {
                heading(
                    ui,
                    t("Jogos (opcional)", "Games (optional)"),
                    t(
                        "Adicione os jogos que deseja copiar para a biblioteca agora. Arraste vários arquivos de uma vez ou clique em Adicionar. Também é possível adicioná-los depois, pelo próprio NXbox no Xbox.",
                        "Add the games you want to copy to the library now. Drag several files at once or click Add. You can also add them later from NXbox on the Xbox.",
                    ),
                );
                if let Some(p) = choose_file(ui, t("Adicionar jogo…", "Add game…"), false) {
                    self.add_game(p);
                }
                ui.add_space(8.0);
                let mut remove = None;
                for (i, game) in self.games.clone().iter().enumerate() {
                    ui.group(|ui| {
                        self.game_card(ui, game);
                        if ui.small_button(t("Remover", "Remove")).clicked() {
                            remove = Some(i);
                        }
                    });
                }
                if let Some(i) = remove {
                    self.games.remove(i);
                }
                if self.games.is_empty() {
                    ui.label(t("Nenhum jogo adicionado. A biblioteca começará vazia.", "No games added. The library will start empty."));
                }
            }
            Page::Extras => {
                heading(
                    ui,
                    t("Conteúdo adicional (opcional)", "Additional content (optional)"),
                    t(
                        "Itens que acompanham o jogo. Todos são opcionais; pode avançar sem nenhum deles.",
                        "Items that come with the game. All are optional; you can continue without any of them.",
                    ),
                );
                for (label, help, folder, slot) in [
                    (t("Firmware", "Firmware"), t("Necessário apenas para jogos que usam recursos do sistema, como alguns títulos online locais.", "Only needed by games that use system features."), true, 0),
                    (t("Saves", "Saves"), t("Pasta de saves exportada do seu Switch, por exemplo com o JKSV.", "Save folder exported from your Switch, for example with JKSV."), true, 1),
                ] {
                    ui.strong(label);
                    ui.small(help);
                    let current = if slot == 0 { &self.firmware } else { &self.saves };
                    ui.label(current.as_ref().map(|p| p.display().to_string()).unwrap_or_else(|| t("Nada selecionado.", "Nothing selected.").into()));
                    if let Some(p) = choose_file(ui, t("Escolher…", "Choose…"), folder) {
                        if slot == 0 { self.firmware = Some(p) } else { self.saves = Some(p) }
                    }
                    ui.add_space(10.0);
                }
                ui.strong("Mods");
                ui.small(t("Pastas no formato LayeredFS (com romfs ou exefs dentro).", "Folders in LayeredFS layout (with romfs or exefs inside)."));
                for m in &self.mods {
                    ui.label(m.display().to_string());
                }
                if let Some(p) = choose_file(ui, t("Adicionar mod…", "Add mod…"), true) {
                    self.mods.push(p);
                }
            }
            Page::Install => {
                heading(
                    ui,
                    t("Instalação", "Installation"),
                    t(
                        "Escolha como o NXbox chegará ao seu Xbox.",
                        "Choose how NXbox will reach your Xbox.",
                    ),
                );
                ui.radio_value(&mut self.manual, false, t("Instalar automaticamente no Xbox", "Install automatically on the Xbox"));
                ui.indent("auto", |ui| {
                    ui.label(t(
                        "O NXbox Pack se conecta ao Device Portal do Xbox pela rede local e instala tudo sozinho. O Xbox e este computador precisam estar na mesma rede.",
                        "NXbox Pack connects to the Xbox Device Portal over the local network and installs everything by itself. The Xbox and this computer must be on the same network.",
                    ));
                });
                ui.radio_value(&mut self.manual, true, t("Prefiro instalar manualmente", "I prefer to install manually"));
                ui.indent("manual", |ui| {
                    ui.label(t(
                        "Gera a pasta do aplicativo (UWP) com todo o conteúdo já dentro. Depois, registre-a você mesmo pelo Device Portal do Xbox.",
                        "Builds the app (UWP) folder with all content already inside. You then register it yourself through the Xbox Device Portal.",
                    ));
                });
                ui.add_space(14.0);
                if self.manual {
                    if let Some(p) = choose_file(ui, t("Escolher onde salvar…", "Choose where to save…"), true) {
                        self.out = Some(p);
                    }
                    ui.label(self.out.as_ref().map(|p| p.display().to_string()).unwrap_or_else(|| t("Nenhuma pasta escolhida.", "No folder chosen.").into()));
                } else {
                    if self.discover_rx.is_none() {
                        self.discover_rx = Some(core(json!({ "cmd": "discover" })));
                    }
                    egui::Grid::new("portal").num_columns(2).spacing([12.0, 8.0]).show(ui, |ui| {
                        ui.label(t("Endereço IP do Xbox", "Xbox IP address"));
                        ui.text_edit_singleline(&mut self.host);
                        ui.end_row();
                        ui.label(t("Usuário", "User name"));
                        ui.text_edit_singleline(&mut self.user);
                        ui.end_row();
                        ui.label(t("Senha", "Password"));
                        ui.add(egui::TextEdit::singleline(&mut self.password).password(true));
                        ui.end_row();
                    });
                    ui.checkbox(&mut self.remember, t("Lembrar o usuário e a senha neste computador", "Remember the user name and password on this computer"));
                    ui.small(t(
                        "O endereço aparece sozinho quando o Xbox é encontrado na rede; também é mostrado em Dev Home. Usuário e senha são os definidos em Dev Home > Remote Access; deixe em branco se já foram salvos antes.",
                        "The address fills in when the Xbox is found on the network; it is also shown in Dev Home. User name and password are the ones set in Dev Home > Remote Access; leave them empty if they were saved before.",
                    ));
                }
            }
            Page::Progress => {
                if let Some(e) = self.error.clone() {
                    heading(
                        ui,
                        t("A instalação não foi concluída", "The installation did not finish"),
                        t(
                            "Nada foi alterado no Xbox além do que já tinha sido enviado. Volte, confira os dados e tente novamente.",
                            "Nothing was changed on the Xbox beyond what had already been sent. Go back, check the details and try again.",
                        ),
                    );
                    ui.monospace(e);
                } else if let Some(d) = self.done.clone() {
                    let (title, body) = if self.manual {
                        (t("Pasta gerada", "Folder created"), t("A pasta do aplicativo está pronta para ser registrada pelo Device Portal:", "The app folder is ready to be registered through Device Portal:"))
                    } else {
                        (t("Instalação concluída", "Installation complete"), t("O aplicativo já aparece em Jogos e aplicativos no Xbox:", "The app now appears in Games & apps on the Xbox:"))
                    };
                    heading(ui, title, body);
                    ui.monospace(d);
                } else {
                    heading(ui, t("Instalando", "Installing"), t("Mantenha esta janela aberta até o fim.", "Keep this window open until it finishes."));
                    ui.label(&self.status);
                    match self.progress {
                        Some(p) => ui.add(egui::ProgressBar::new(p).show_percentage()),
                        None => ui.spinner(),
                    };
                }
            }
        }
    }
}

impl eframe::App for App {
    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        let ctx = ui.ctx().clone();
        self.take_drops(&ctx);
        self.poll();
        if !self.inspecting.is_empty() || self.run_rx.is_some() || self.discover_rx.is_some() {
            ctx.request_repaint_after(std::time::Duration::from_millis(100));
        }

        let steps = flow(self.mode).len();
        let page = self.page();
        egui::Panel::bottom("nav").show(ui, |ui| {
            ui.add_space(8.0);
            ui.horizontal(|ui| {
                let running = page == Page::Progress && self.done.is_none() && self.error.is_none();
                if self.page_index > 0 && !running && ui.button(t("Voltar", "Back")).clicked() {
                    self.page_index -= 1;
                    if page == Page::Progress {
                        self.run_rx = None;
                    }
                }
                if self.mode.is_some() && page != Page::Welcome {
                    ui.label(format!("{} {} {} {}", t("Etapa", "Step"), self.page_index, t("de", "of"), steps - 1));
                }
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    if page == Page::Progress {
                        if self.done.is_some() && ui.button(t("Concluir", "Finish")).clicked() {
                            ctx.send_viewport_cmd(egui::ViewportCommand::Close);
                        }
                    } else {
                        let next = if page == Page::Install {
                            if self.manual { t("Gerar pasta", "Create folder") } else { t("Instalar", "Install") }
                        } else {
                            t("Próximo", "Next")
                        };
                        if ui.add_enabled(self.can_continue(), egui::Button::new(next)).clicked() {
                            if page == Page::Install {
                                self.start();
                            }
                            self.page_index += 1;
                        }
                    }
                });
            });
            ui.add_space(8.0);
        });
        egui::CentralPanel::default().show(ui, |ui| {
            egui::ScrollArea::vertical().show(ui, |ui| self.show_page(ui));
        });
    }
}
