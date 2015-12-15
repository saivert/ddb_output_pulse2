#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.69])
AC_INIT([ddb_output_pulse2], [devel], [saivert@saivert.com])
AC_CONFIG_SRCDIR([src/pulse.c])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIR([m4])
AM_INIT_AUTOMAKE([foreign -Wall subdir-objects dist-xz no-dist-gzip])
PACKAGE=deadbeef

# Checks for programs.
AC_PROG_CC
AM_PROG_AR
AC_PROG_INSTALL
AC_PROG_LIBTOOL
PKG_PROG_PKG_CONFIG


# Checks for libraries.
LT_INIT
PKG_CHECK_MODULES(PULSE_DEPS, libpulse-simple, , 
	AC_MSG_ERROR([libpulse-simple is required for this package])
)


# Checks for header files.
AC_CHECK_HEADERS([stdint.h string.h unistd.h])


AC_CHECK_HEADERS([deadbeef/deadbeef.h],,
    [
     AC_MSG_ERROR([missing required header (see above)])
    ])

# Checks for typedefs, structures, and compiler characteristics.

# Checks for library functions.
AC_CHECK_FUNCS([memset])

AM_CONDITIONAL(HAVE_PULSE, test "x$HAVE_PULSEPLUGIN" = "xyes")

AC_CONFIG_FILES([Makefile
                 src/Makefile])
AC_OUTPUT
