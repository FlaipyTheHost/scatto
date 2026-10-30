# Snapper Manager

App em GTK4 + libadwaita (C++17) para gerenciar o Snapper.
Usa o tema Adwaita padrão do sistema — nenhum CSS próprio é necessário.

## Compilar

```sh
meson setup build
ninja -C build
./build/snapper-manager
```

## Dependências (Debian/Ubuntu)

```sh
sudo apt install g++ meson ninja-build pkg-config libgtk-4-dev libadwaita-1-dev
```

## Estrutura

- `main.cpp`        — aplicação, janela principal, header bar com ViewSwitcher
- `home_page.{hpp,cpp}` — aba "Home": uso do filesystem + lista de subvolumes do Snapper
- `ring_chart.{hpp,cpp}` — gráfico de anéis (wheel) desenhado com Cairo, usando as
  cores do tema (accent color / dark mode) em vez de valores fixos
- `fs_info.{hpp,cpp}` — leitura de Size/Allocated/Used/Free via `statfs()` e,
  quando o filesystem é Btrfs, via `/sys/fs/btrfs/<uuid>/allocation/*`

## Como funciona a aba Home

- **Gráfico**: anel externo = Used/Free; anel interno = Allocated/Unallocated
  (Allocated só existe de fato em Btrfs; em outros filesystems ele é igual ao Used).
- **Lista de subvolumes**: roda `snapper --csvout list-configs` de forma assíncrona
  (não trava a janela) e lista cada subvolume configurado, com um ✓ verde se a
  pasta `.snapshots` existir, ou um aviso caso contrário.
- Botão de atualizar no header bar (ou Ctrl+R / F5) para reler tudo.

## Próximos passos sugeridos

- Aba "Snapshots": listar/criar/apagar snapshots de uma config selecionada
  (`snapper -c <config> list` / `create` / `delete`).
- Aba "Configs": criar/editar configurações do Snapper (precisa de privilégios
  de root — dá pra rodar os comandos via `pkexec`).

## Aba Snaps

- Um grupo por volume gerenciado pelo Snapper (título = caminho do subvolume,
  descrição = nome da config), com um botão **+** no cabeçalho para criar um
  novo snapshot (abre um `AdwAlertDialog` pedindo a descrição).
- Cada snapshot é uma linha com a descrição como título e `#número — data/hora`
  como subtítulo, ícone de acordo com o tipo (single/pre/post/timeline), e um
  selo quando é o snapshot padrão/ativo da config.
- Um botão de menu (⋮) por linha com **Browse Files** (abre a pasta do
  snapshot no gerenciador de arquivos padrão), **Restore Snapshot** e
  **Delete Snapshot** — os dois últimos pedem confirmação antes de agir.
- Feedback de sucesso/erro aparece como toast no rodapé da janela.

Comandos do Snapper usados:
- Listar: `snapper -c <config> --csvout --iso list --columns number,date,description,type,active`
- Criar:  `snapper -c <config> create --type single --description "..."`
- Restaurar: `snapper -c <config> undochange <N>..0`
- Apagar: `snapper -c <config> delete <N>`

## Aba Schedule

- Um grupo por volume, listando as regras de agendamento **ativas**: os
  períodos do timeline nativo do Snapper (Hourly/Daily/Weekly/Monthly/Yearly
  — cada um é só uma contagem de retenção, ex. "mantém os 5 snapshots
  horários mais recentes") e os agendamentos **customizados** criados por
  este app.
- Botão **+** no cabeçalho de cada volume abre a janela "New schedule for
  <subvolume>" com 6 abas: Hourly, Daily, Weekly, Monthly, Yearly (campo
  numérico "quantidade a manter" + preview em texto) e Custom (checkboxes
  de dia da semana + hora:minuto + preview).
- Cada regra tem um botão de lixeira para removê-la.

**Importante — como o agendamento realmente funciona:**
- As abas Hourly/Daily/Weekly/Monthly/Yearly editam a configuração real do
  Snapper via `snapper -c <config> set-config TIMELINE_CREATE=yes
  TIMELINE_LIMIT_<PERIODO>=<N>`. Isso só é executado de fato se o timer
  `snapper-timeline.timer` estiver habilitado no sistema
  (`sudo systemctl enable --now snapper-timeline.timer`) — o Snapper roda
  esse timer de hora em hora e cada TIMELINE_LIMIT_* decide quantos
  snapshots de cada período ficam retidos.
- O Snapper **não tem** um agendamento arbitrário nativo (tipo "toda terça e
  quinta às 3h"). A aba Custom implementa isso criando um timer systemd
  `--user` (arquivo `.timer` + `.service` em `~/.config/systemd/user/`) que
  roda `snapper -c <config> create` no dia/hora escolhidos — sem precisar de
  root, usando a mesma permissão que o app já usa para criar snapshots pela
  aba Snaps.

## Privilégios de administrador (root)

O app pede autenticação de administrador **uma única vez**, ao abrir — como
o GParted faz — e não abre a janela principal se a autenticação for
cancelada ou falhar.

Diferente do GParted, porém, a **interface gráfica nunca roda como root**:
só um processo auxiliar minúsculo e sem interface (`snapper-manager-helper`)
roda como root, iniciado uma vez via `pkexec` na inicialização e mantido
rodando durante toda a sessão. A janela continua sendo um processo comum do
seu usuário, herdando normalmente tema, fontes, dark mode, etc. Toda
chamada ao `snapper` passa por esse processo através de um pipe simples;
o helper só sabe fazer uma coisa: executar `/usr/bin/snapper <args>` com o
que a interface pede — nada além disso (não executa comandos arbitrários).

Isso exige que os dois binários fiquem na mesma pasta (o app localiza o
helper automaticamente ao lado do seu próprio executável). Ao instalar com
`sudo ninja -C build install`, o meson já garante isso.

Se o `pkexec`/PolicyKit não estiver instalado, ou a autenticação for
recusada, aparece um diálogo nativo explicando o motivo e o app fecha.

## Traduções

Usa gettext, o padrão do GTK. As strings ficam em `po/` (`scatto.pot` é o modelo e
`pt_BR.po` a tradução). Para atualizar depois de mudar textos no código:

    meson compile -C build scatto-pot          # regenera o modelo
    meson compile -C build scatto-update-po    # atualiza os .po existentes

Para adicionar um idioma: crie `po/<idioma>.po` (por exemplo com `msginit`) e
acrescente o código em `po/LINGUAS`. Rodando direto de `build/`, as traduções
vêm de `build/po`; `SCATTO_LOCALEDIR` sobrescreve o diretório.

## Licença

GPL-3.0-or-later. Veja o arquivo `LICENSE`.
