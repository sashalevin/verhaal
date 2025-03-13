# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2024 Greg Kroah-Hartman <gregkh@linuxfoundation.org>

project(
  'verhaal',
  'c',
  version: '017',
  license: ['GPL-2.0-only', 'GPL-2.0-or-later','GPL-2.0-or-later'],
  meson_version : '>=0.60.0',
  default_options : ['c_std=gnu99', 'warning_level=2', 'prefix=/usr']
)

pkg = import('pkgconfig')

# Compiler stuff
cc = meson.get_compiler('c')

add_project_arguments(
  cc.get_supported_arguments([
    '-Wbad-function-cast',
    # should be removed and the code fixed
    '-Wno-cast-align',
    '-Wchar-subscripts',
    '-Wempty-body',
    '-Wformat',
    '-Wformat=2',
    '-Wformat-nonliteral',
    '-Wformat-security',
    '-Wformat-y2k',
    '-Winit-self',
    '-Winline',
    '-Wint-conversion',
    '-Wmissing-declarations',
    '-Wmissing-format-attribute',
    '-Wmissing-include-dirs',
    '-Wmissing-prototypes',
    '-Wmissing-noreturn',
    '-Wnested-externs',
    '-Wold-style-definition',
    '-Wold-style-declaration',
    '-Wpointer-arith',
    '-Wredundant-decls',
    '-Wshadow',
    '-Wsign-compare',
    #'-Wsuggest-attribute=const',
    #'-Wsuggest-attribute=noreturn',
    #'-Wsuggest-attribute=malloc',
    #'-Wsuggest-attribute=returns_nonnull',
    #'-Wno-unused-function',
    '-Wstrict-prototypes',
    '-Wswitch',
    '-Wtype-limits',
    '-Wundef',
    '-Wuninitialized',
    '-Wunused',
    '-Wunused-variable',
    '-Wvla',
    '-Wwrite-strings',
    '-fdiagnostics-color=auto',
    # warnings disabled on purpose
    '-Wno-unused-parameter',
    '-Wno-deprecated-declarations',
  ]),
  language: 'c',
)

# Configuration information
config = configuration_data()
config.set_quoted('PACKAGE_NAME', meson.project_name())
config.set_quoted('VERSION', meson.project_version())
config.set_quoted('DATABASE_NAME', 'verhaal.db')
config_h = configure_file(output: 'config.h', configuration: config)

add_project_arguments('-include', 'config.h', language : 'c')

##########################
# lsusb build instructions
##########################
verhaal_sources = [
  'src/db.c',
  'src/fixes.c',
  'src/main.c',
  'src/search.c',
  'src/terminal.c',
  'src/time.c',
  'src/versions.c',
]

libsqlite3  = dependency('sqlite3', version: '>= 3.40')
libgit2     = dependency('libgit2', version: '>= 1.5')
libpcre2    = dependency('libpcre2-8', version: '>= 10.42')

executable('verhaal', verhaal_sources, dependencies: [libsqlite3, libgit2, libpcre2], install: true)
