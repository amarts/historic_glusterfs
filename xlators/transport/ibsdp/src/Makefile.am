
xlator_PROGRAMS = ibsdp.so
xlatordir = $(libdir)/glusterfs/xlator/transport

ibsdp_so_SOURCES = ibsdp.c
noinst_HEADERS = transport-socket.h

AM_CFLAGS = -fPIC -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE -Wall \
	-I$(top_srcdir)/libglusterfs/src -shared -nostartfiles

CLEANFILES = *~

