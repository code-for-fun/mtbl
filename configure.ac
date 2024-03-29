AC_PREREQ(2.60)
AC_INIT([mtbl], [0.1])
AC_CONFIG_SRCDIR([mtbl/mtbl.h])
AC_CONFIG_AUX_DIR([build-aux])
AM_INIT_AUTOMAKE([foreign 1.11 -Wall -Wno-portability silent-rules color-tests])
AC_PROG_CC_STDC
AC_USE_SYSTEM_EXTENSIONS
AC_SYS_LARGEFILE
AC_CONFIG_MACRO_DIR([m4])
AM_SILENT_RULES([yes])
LT_INIT

my_CFLAGS="-Wall \
-Wmissing-declarations -Wmissing-prototypes \
-Wnested-externs -Wpointer-arith \
-Wpointer-arith -Wsign-compare -Wchar-subscripts \
-Wstrict-prototypes -Wshadow \
-Wformat-security"
AC_SUBST([my_CFLAGS])

AC_CONFIG_HEADERS(config.h)
AC_CONFIG_FILES([
        Makefile
])

AC_CHECK_FUNC([mkstemp], [], [
    AC_MSG_ERROR([required system function not found])
])

AC_CHECK_HEADERS([sys/endian.h endian.h])

AC_CHECK_HEADER([snappy-c.h], [], [
    AC_MSG_ERROR([required header file not found])
])
AC_CHECK_LIB([snappy], [snappy_compress], [], [
    AC_MSG_ERROR([required library not found])
])

AC_CHECK_HEADER([zlib.h], [], [
    AC_MSG_ERROR([required header file not found])
])
AC_CHECK_LIB([z], [deflate], [], [
    AC_MSG_ERROR([required library not found])
])

AC_SEARCH_LIBS([dlopen], [dl])

AC_OUTPUT
AC_MSG_RESULT([
    $PACKAGE $VERSION

        compiler:               ${CC}
        cflags:                 ${CFLAGS}
        ldflags:                ${LDFLAGS}
        libs:                   ${LIBS}

        prefix:                 ${prefix}
        sysconfdir:             ${sysconfdir}
        libdir:                 ${libdir}
        includedir:             ${includedir}
])
