a2x = find_program(['a2x', 'a2x.py'], required: false)

ignore_headers = [
    'ufo-priv.h',
    'ufo-two-way-queue.h',
]

manpage_names = [
    'ufo-launch.1',
    'ufo-mkfilter.1',
    'ufo-query.1',
    'ufo-prof.1',
    'ufo-runjson.1',
]

if a2x.found()
    foreach manpage: manpage_names
        source = 'man/@0@.txt'.format(manpage)
        command = [a2x.path()]
        command += [
            '-L', '-d', 'manpage', '-f', 'manpage',
            '-D', '@OUTDIR@',
            '@INPUT@'
            ]

        man = custom_target(manpage,
            build_by_default: true,
            command: command,
            input: source,
            output: manpage,
            install: true,
            install_dir: get_option('mandir'),
        )
    endforeach
else
    message('a2x not found, cannot generate manpages.')
endif

if gtk_doc_dep.found() and get_option('gtk_doc')
    gnome.gtkdoc('Ufo',
        main_xml: 'Ufo-docs.xml',
        dependencies: lib_dep,
        gobject_typesfile: 'Ufo.types',
        src_dir: join_paths(meson.source_root(), 'ufo'),
        scan_args: [
          '--ignore-headers=' + ' '.join(ignore_headers),
        ],
        install: true,
    )
endif
