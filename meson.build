project('mig_mon', 'c',
        version: run_command('git', 'describe', '--tags', '--dirty', check: true)
                 .stdout().strip())

add_project_arguments('-DMIG_MON_VERSION="' + meson.project_version() + '"',
                      language: 'c')

sources = files('mig_mon.c', 'downtime.c', 'mm_dirty.c', 'utils.c', 'vm.c')

executable('mig_mon', sources)
