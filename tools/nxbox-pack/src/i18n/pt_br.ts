// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later

import type { Translation } from "./registry";

export default {
  installMode:
    "1. Enviar jogos para a biblioteca do NXbox\n2. Instalar este jogo como um app próprio\nEscolha o modo",
  libraryGame:
    "Arraste um jogo, update ou DLC (.nsp .nsz .xci .xcz), um arquivo por vez; Enter termina",
  libraryKeys: "Arraste prod.keys ou pressione Enter para pular",
  libraryMissing:
    "O app principal NXbox não foi encontrado neste console. Instale o NXbox primeiro e execute --library novamente.",
  libraryDone:
    "Pronto. Abra o NXbox: os jogos aparecem na biblioteca após juntar as partes ao iniciar. Jogos grandes podem levar um minuto.",
  profileUsage:
    "Uso: nxbox-pack profile multi on|off, ou nxbox-pack profile avatar <nome> <imagem> | --character <id> --color <nome> [--insecure-portal]",
  profilePackage: "Nome completo do pacote (escolha o app cujas contas quer editar)",
  profileMissing:
    "Perfil não encontrado, nome duplicado ou dados inválidos. Abra o NXbox uma vez para criar os perfis.",
  profileClose:
    "Feche o app NXbox selecionado no console antes de continuar. Pressione Enter quando fechar.",
  profileDone: "Perfil atualizado. Reabra o app NXbox selecionado.",
  avatarInvalid:
    "Escolha uma imagem PNG, JPEG, WebP, GIF ou BMP válida (até 20 MB). O navegador precisa concluir em dois minutos.",
  avatarBrowser: "Convertendo no seu navegador local. Se ele não abriu, abra esta URL neste PC:",
  avatarConverting: "Preparando seu avatar NXbox…",
  avatarConverted: "Imagem preparada. Volte ao NXbox Pack; você pode fechar esta aba.",

  guide:
    "Arraste o arquivo do jogo para esta janela e pressione Enter (suportados: .nsp .xci .nsz .xcz)",
  legal:
    "Você deve possuir o jogo e extrair suas próprias chaves. NXbox Pack nunca baixa jogos ou chaves.",
  keys: "Arraste seu prod.keys para esta janela e pressione Enter",
  extra: "Arraste um update/DLC ou pressione Enter para continuar (um arquivo por vez)",
  host: "Endereço do Xbox (Dev Home > Acesso remoto)",
  found: "Possíveis Xbox encontrados",
  user: "Usuário do Device Portal",
  password: "Senha do Device Portal (oculta)",
  saved: "Usar credenciais do armazenamento seguro do sistema? [S/n]",
  remember: "Salvar credenciais no armazenamento seguro do sistema? [s/N]",
  secureUnavailable: "Armazenamento seguro indisponível; credenciais ficam apenas nesta sessão.",
  fallback:
    "A extração de metadados do Control NCA ainda não está implementada. Usando ID/nome do arquivo e um ícone genérico do NXbox.",
  title: "ID do jogo base (16 dígitos hexadecimais começando com 0100)",
  name: "Nome do jogo",
  runtime: "Baixando e verificando o runtime do NXbox…",
  install: "Registrando seu aplicativo independente do jogo…",
  upload: "Enviando para o LocalState privado deste aplicativo…",
  done: "Pronto. Abra o ícone deste jogo no Dev Home do Xbox; defina seu tipo como Game para usar toda a memória disponível.",
  failed: "NXbox Pack falhou",
  file: "Escolha um arquivo .nsp, .xci, .nsz ou .xcz existente e não vazio.",
  keyFile: "Escolha um arquivo existente e não vazio chamado prod.keys.",
  titleInvalid:
    "O ID do jogo base deve ter 16 dígitos hexadecimais, começar com 0100 e terminar com 000.",
  nameInvalid: "Informe um nome de 1–64 caracteres sem caracteres de controle.",
  compressed:
    "NSZ/XCZ precisa do descompressor opcional nsz no PATH (nsz -D). Instale-o ou forneça um dump NSP/XCI.",
  runtimeMissing:
    "Runtime indisponível; downloads do CI podem exigir NXBOX_PACK_GITHUB_TOKEN. Informe --runtime-url e --runtime-sha256 (ZIP do CI ou APPX de jogo).",
  runtimeInvalid: "Falha no download, checksum ou validação do runtime.",
  portal: "Falha na requisição ao Device Portal. Confira endereço, credenciais e Dev Mode.",
  deploy:
    "O registro no Xbox falhou ou excedeu o tempo. Confira o VCLibs e o log de implantação no Device Portal.",
  interrupted: "Cancelado.",
  secret: "É necessário um terminal para digitar uma senha oculta.",
  args: "Argumentos inválidos. Execute --help.",
  progress: "Enviando",
  itemGame: "Jogo",
  itemKeys: "Chaves (prod.keys)",
  itemUpdate: "Update",
  itemDlc: "DLC",
  itemFirmware: "Firmware",
  itemSaves: "Dados salvos",
  itemMods: "Mods",
  itemTitleKeys: "Chaves (title.keys)",
  fileCount: "arquivos",
  firmware:
    "Arraste uma pasta de NCAs, ZIP de firmware ou backup rawnand do hekate; Enter pula (recomendado: sem firmware, telas Mii e alguns applets do sistema ficam vazios)",
  saves: "Arraste a pasta de um backup JKSV/Checkpoint ou ZIP deste título; Enter pula",
  mods: "Arraste pastas romfs/exefs ou pasta/ZIP de mods deste título; Enter pula",
  titleKeys: "Arraste seu title.keys; Enter pula",
  inputInvalid:
    "Pasta/ZIP inválido: caminhos inseguros, formato não suportado, dados vazios ou sistema de arquivos corrompido.",
  firmwareInvalid:
    "Firmware rejeitado: SHA-256 do nome, cabeçalho NCA3 criptografado ou dados SystemVersion inválidos.",
  firmwareIncomplete:
    "Firmware incompleto: precisa de SystemVersion (0100000000000809, Data) e modelo Mii (0100000000000802, Data/PublicData).",
  firmwareKeys:
    "Firmware exige header_key e, para NAND, bis_key_02 (32 bytes, crypt+tweak). SystemVersion criptografado também exige a key_area_key correspondente no prod.keys.",
  nandInvalid:
    "NAND hekate inválida: partição SYSTEM ausente/ambígua no GPT, partes divididas ausentes ou FAT32 ilegível (confira bis_key_02).",
  optionalMismatch:
    "Rejeitado: ID do backup/mod não corresponde ao jogo. Arraste um único backup deste título.",
  modsInvalid:
    "Rejeitado: mods devem conter romfs/exefs em uma pasta de mod ou load/<titleid>/<modname>/.",
  titleKeysInvalid: "Escolha um arquivo existente e não vazio chamado title.keys.",
  saveExists:
    "Importação interrompida: este app já tem um perfil. Use um app novo do jogo para não substituir usuário ou saves.",
  none: "-",
  rejectNoId:
    "Rejeitado: o nome do arquivo não tem um único [ID do título], então não dá para associar ao jogo.",
  rejectMismatch:
    "Rejeitado: este ID de título não pertence ao jogo (esperado o ID do update ou a faixa de DLC).",
  rejectDuplicate: "Rejeitado: este arquivo ou ID de título já foi adicionado.",
  help: "NXbox Pack\nUso: nxbox-pack [--library] [--runtime-url URL_HTTPS --runtime-sha256 HEX] [--insecure-portal]\n--insecure-portal aceita o certificado TLS autoassinado do Xbox somente nesta conexão.\nRuntime padrão: última release de NspxMiguel/NXbox com digest SHA-256 do arquivo.\nJogos e chaves ficam fora do runtime preparado. App próprio requer nsz no PATH para NSZ/XCZ; --library envia diretamente.\n--library envia jogos ao app principal NXbox instalado sem montar pacote.",
} satisfies Translation;
