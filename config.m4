PHP_ARG_ENABLE([flashpoint],
  [whether to enable Flashpoint support],
  [AS_HELP_STRING([--enable-flashpoint],
    [Enable Flashpoint support])])

if test "$PHP_FLASHPOINT" = "yes"; then
  PHP_NEW_EXTENSION(flashpoint, flashpoint.c, $ext_shared)
end