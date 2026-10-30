i18n.gettext(gettext_package, preset: 'glib')

# Development convenience: also lay the compiled catalogues out as
# <build>/po/<lang>/LC_MESSAGES/<package>.mo, which is where the executable
# looks when run straight from the build directory (see setup_i18n()).
fs = import('fs')
msgfmt = find_program('msgfmt')
foreach lang : fs.read('LINGUAS').strip().split('\n')
  custom_target('mo-' + lang,
    input: lang + '.po',
    output: lang + '.stamp',
    command: [find_program('sh'), '-c',
              'mkdir -p "$1/@0@/LC_MESSAGES" && "$2" -o "$1/@0@/LC_MESSAGES/@1@.mo" "$3" && touch "$4"'.format(lang, gettext_package),
              'sh', meson.current_build_dir(), msgfmt, '@INPUT@', '@OUTPUT@'],
    build_by_default: true)
endforeach
