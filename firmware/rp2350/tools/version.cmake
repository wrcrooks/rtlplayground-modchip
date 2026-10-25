# The firmware's version, from git at build time (CMakeLists.txt runs this on
# every build): generated/version.h for RTLPlayground's "version" command and
# web UI, generated/modchip_version.h for the mod chip's own code.
#
#   cmake -DSRC=<source dir> -DGEN=<generated dir> -DGIT=<git> -P version.cmake
#
# The headers are rewritten only when "git describe --dirty" says something
# new, so an unchanged tree rebuilds nothing; the build date is the time of
# that rewrite.

execute_process(COMMAND ${GIT} describe --always --dirty --tags
    WORKING_DIRECTORY ${SRC}
    OUTPUT_VARIABLE VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if (NOT VERSION)
    set(VERSION unknown)
endif()

set(MODCHIP_H "#define MODCHIP_VERSION \"${VERSION}\"\n")
set(OLD "")
if (EXISTS ${GEN}/modchip_version.h)
    file(READ ${GEN}/modchip_version.h OLD)
endif()
if (OLD STREQUAL MODCHIP_H AND EXISTS ${GEN}/version.h)
    return()
endif()

string(TIMESTAMP BUILD_DATE "%Y-%m-%d %H:%M:%S")
file(WRITE ${GEN}/version.h
    "#ifndef VERSION_H\n#define VERSION_H\n#define VERSION_SW \"modchip-${VERSION}\"\n#define BUILD_DATE \"${BUILD_DATE}\"\n#endif\n")
file(WRITE ${GEN}/modchip_version.h "${MODCHIP_H}")
message(STATUS "Firmware version modchip-${VERSION}")
