dnl
dnl UNI_TINGYUN_APM()
dnl
dnl Detects the TingYun APM C SDK under libs/tingyun/ and selects the
dnl appropriate static library for the host platform. The SDK is required
dnl for APM instrumentation of the TTS/ASR plugins and main executables.
dnl
dnl Adds the following argument to the configure script:
dnl
dnl   --enable-tingyun   (default: auto; auto-enabled on supported platforms)
dnl
dnl Sets the following variables on exit:
dnl
dnl   enable_tingyun        : "yes" or "no"
dnl   TINGYUN_CPPFLAGS      : include flags for the SDK header
dnl   TINGYUN_LIBADD        : static library path for LDADD
dnl
dnl Supported host platforms (matching libs/tingyun/lib/ contents):
dnl   x86_64  + glibc  -> libtingyun_sdk.a
dnl   x86     + glibc  -> libtingyun_sdk.x86.a
dnl   x86_64  + musl   -> libtingyun_musl.x64.a
dnl All other platforms: auto-disabled unless --enable-tingyun=yes is explicit.
dnl
AC_DEFUN([UNI_TINGYUN_APM],[
    AC_ARG_ENABLE([tingyun],
        [AC_HELP_STRING([--enable-tingyun],
            [enable TingYun APM instrumentation (default: auto)])],
        [enable_tingyun="$enableval"],
        [enable_tingyun="auto"])

    tingyun_dir="$srcdir/libs/tingyun"
    TINGYUN_CPPFLAGS=""
    TINGYUN_LIBADD=""

    AS_IF([test "$enable_tingyun" != "no"], [
        dnl Select static library by host platform.
        tingyun_lib=""
        AS_CASE([$host],
            [x86_64-*-linux-musl*], [tingyun_lib="libtingyun_musl.x64.a"],
            [*86-*-linux-musl*],    [tingyun_lib="libtingyun_musl.x64.a"],
            [x86_64-*-linux*],      [tingyun_lib="libtingyun_sdk.a"],
            [*86-*-linux*],         [tingyun_lib="libtingyun_sdk.x86.a"],
            [tingyun_lib=""])

        dnl Require header.
        AS_IF([test ! -f "$tingyun_dir/include/tingyun.h"], [
            AS_IF([test "$enable_tingyun" = "yes"],
                [AC_MSG_ERROR([--enable-tingyun=yes but $tingyun_dir/include/tingyun.h is missing])],
                [enable_tingyun="no"])
        ])

        dnl Require matching static library.
        AS_IF([test "$enable_tingyun" != "no"], [
            AS_IF([test -z "$tingyun_lib" || test ! -f "$tingyun_dir/lib/$tingyun_lib"], [
                AS_IF([test "$enable_tingyun" = "yes"],
                    [AC_MSG_ERROR([--enable-tingyun=yes but static library for host '$host' is missing (looked for libs/tingyun/lib/$tingyun_lib)])],
                    [enable_tingyun="no"])
            ])
        ])

        AS_IF([test "$enable_tingyun" != "no"], [
            TINGYUN_CPPFLAGS="-I\$(top_srcdir)/libs/tingyun/include"
            TINGYUN_LIBADD="\$(top_srcdir)/libs/tingyun/lib/$tingyun_lib"
            AC_DEFINE([TINGYUN_ENABLED], [1],
                [Define to 1 to enable TingYun APM instrumentation])
            enable_tingyun="yes"
        ])
    ])

    AC_SUBST([TINGYUN_CPPFLAGS])
    AC_SUBST([TINGYUN_LIBADD])
    AM_CONDITIONAL([TINGYUN_ENABLED], [test "$enable_tingyun" = "yes"])
])
