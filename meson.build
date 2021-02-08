project('ddb_output_pulse2', 'c',
        version: '0.1',
        default_options: [
          'buildtype=debugoptimized',
          'warning_level=1',
        ],
        license: 'GPLv2.0+')

config_data = configuration_data()

debug_cflags = []
buildtype = get_option('buildtype')
if buildtype.startswith('debug')
  debug_cflags += '-DDBPULSE_DEBUG'
endif

add_project_arguments(debug_cflags, language: 'c')
add_project_arguments('-DENABLE_NLS', language: 'c')

cc = meson.get_compiler('c')
if not cc.has_header('deadbeef/deadbeef.h')
  error('missing header: deadbeef.h')
endif

pulse_dep = dependency('libpulse')
libintl = cc.find_library('intl', required : false)

if get_option('nls') == true
  subdir('po')
endif

configure_file(output: 'config.h',
               configuration: config_data)

shared_library('pulse2', 'pulse.c', dependencies : [pulse_dep, libintl], name_prefix: '',
  install: true, install_dir: 'lib/deadbeef')

