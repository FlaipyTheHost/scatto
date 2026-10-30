project('scatto', 'cpp',
        version : '0.1.0',
        default_options : ['warning_level=3', 'cpp_std=c++17'])

i18n = import('i18n')
gettext_package = meson.project_name()
add_project_arguments(
  '-DGETTEXT_PACKAGE="@0@"'.format(gettext_package),
  '-DLOCALEDIR="@0@"'.format(get_option('prefix') / get_option('localedir')),
  language: 'cpp')

gtk_dep = dependency('gtk4', version: '>= 4.10')
adw_dep = dependency('libadwaita-1', version: '>= 1.5')  # AdwAboutDialog
gio_dep = dependency('gio-2.0')
gio_unix_dep = dependency('gio-unix-2.0')
threads_dep = dependency('threads')

# The only part of the app that runs as root: started once via `pkexec` by
# the (unprivileged) main executable, it only ever execs /usr/bin/snapper.
executable(
  'snapper-manager-helper',
  'helper/helper_main.cpp',
  'ipc_protocol.cpp',
  dependencies: [gio_dep],
  install: true,
)

executable(
  'snapper-manager',
  'main.cpp',
  'home_page.cpp',
  'snaps_page.cpp',
  'schedule_page.cpp',
  'schedule_dialog.cpp',
  'ring_chart.cpp',
  'fs_info.cpp',
  'snapper_cli.cpp',
  'systemd_timer.cpp',
  'ipc_protocol.cpp',
  'root_helper.cpp',
  dependencies: [gtk_dep, adw_dep, gio_unix_dep, threads_dep],
  install: true,
)

i18n.merge_file(
  input: 'data/org.gnome.scatto.desktop.in',
  output: 'org.gnome.scatto.desktop',
  type: 'desktop',
  po_dir: 'po',
  install: true,
  install_dir: get_option('datadir') / 'applications')
install_data('data/org.gnome.scatto.svg',
  install_dir: get_option('datadir') / 'icons/hicolor/scalable/apps')

subdir('po')
