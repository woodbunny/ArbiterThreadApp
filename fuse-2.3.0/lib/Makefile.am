## Process this file with automake to produce Makefile.in

lib_LTLIBRARIES = libfuse.la

libfuse_la_SOURCES = 	\
	fuse.c		\
	fuse_mt.c	\
	helper.c	\
	mount.c		\
	fuse_i.h	\
	fuse_kernel_compat5.h \
	$(AB_ROOT)/client_module/lib_client.c

libfuse_la_LDFLAGS = -lpthread -version-number 2:3:0 \
	-Wl,--version-script,fuse_versionscript

EXTRA_DIST = fuse_versionscript
