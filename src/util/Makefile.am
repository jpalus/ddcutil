AM_CPPFLAGS =        \
  $(GLIB_CFLAGS)     \
  $(LIBDRM_CFLAGS)

AM_CFLAGS = -Wall 
AM_CFLAGS += -Werror
# -pedantic issues warnings re code that doesn't conform to ISO C
#  Alas, -m modifier on sscanf is a POSIX extension, not ISO C
#  In combination with -Werror, causes device_util.c to fail compilation
# AM_CFLAGS += -pedantic
# AM_CFLAGS += -H

if ENABLE_CALLGRAPH_COND
AM_CFLAGS += -fdump-rtl-expand
endif


CLEANFILES = \
*expand

clean-local:
	@echo "(src/util/Makefile) clean-local"

mostlyclean-local:
	@echo "(src/util/Makefile) mostlyclean-local"

distclean-local:
	@echo "(src/util/Makefile) distclean-local"

# Intermediate Libraries

noinst_LTLIBRARIES = libutil.la

libutil_la_SOURCES = \
data_structures.c          \
debug_util.c               \
device_id_util.c           \
edid.c                     \
error_info.c               \
file_util.c                \
glib_util.c                \
glib_string_util.c         \
i2c_util.c                 \
multi_level_map.c          \
output_sink.c              \
report_util.c              \
string_util.c              \
sysfs_util.c               \
subprocess_util.c          \
timestamp.c                \
udev_i2c_util.c            \
udev_usb_util.c            \
udev_util.c                \
utilrpt.c

# libutil_la_SOURCES += systemd_util.c


if ENABLE_FAILSIM_COND
libutil_la_SOURCES += failsim.c
endif

if USE_LIBDRM_COND
libutil_la_SOURCES += libdrm_util.c
endif

if USE_X11_COND
libutil_la_SOURCES += x11_util.c
endif


dist-hook:
	@echo "(src/util/Makefile) dist-hook"

