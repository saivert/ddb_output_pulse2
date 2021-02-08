if get_option('nls')
    i18n = import('i18n')
    # define GETTEXT_PACKAGE
    add_project_arguments('-DGETTEXT_PACKAGE="ddb_output_pulse2"', language:'c')
    i18n.gettext(meson.project_name(),
        args: [
            '--directory=' + meson.source_root(),
            '--keyword=_',
            '--foreign-user'
        ]
    )
    config_data.set('HAVE_GETTEXT', '1')
    config_data.set('ENABLE_NLS', '1')
endif
