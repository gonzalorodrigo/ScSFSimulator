#
# Makefile for slurmd

AUTOMAKE_OPTIONS = foreign

sbin_PROGRAMS = slurmd

LIBS = @LIBS@ -lyajl -lcgraph -lrt

AM_CPPFLAGS = -DLIBSLURM_SO=\"$(libdir)/libslurm.so\" \
	-I$(top_srcdir) $(HWLOC_CPPFLAGS)

slurmd_LDADD = 					   \
	$(top_builddir)/src/common/libdaemonize.la \
	$(top_builddir)/src/api/libslurm.o $(DL_LIBS)	   \
	$(HWLOC_LDFLAGS) $(HWLOC_LIBS) \
	../common/libslurmd_common.la \
	../common/libslurmd_reverse_tree_math.la

SLURMD_SOURCES = \
	slurmd.c slurmd.h \
	req.c req.h \
	get_mach_stat.c get_mach_stat.h	\
	read_proc.c 	        	\
	xcpu.c xcpu.h \
	slurmd_plugstack.c slurmd_plugstack.h

slurmd_SOURCES = $(SLURMD_SOURCES)

if HAVE_AIX
# We need to set maxdata back to 0 because this effects the "max memory size"
# ulimit, and the ulimit is inherited by child processes.
slurmd_LDFLAGS = -export-dynamic $(CMD_LDFLAGS) \
	$(HWLOC_LDFLAGS) $(HWLOC_LIBS) -Wl,-bmaxdata:0x0
else
slurmd_LDFLAGS = -export-dynamic $(CMD_LDFLAGS) \
	$(HWLOC_LDFLAGS) $(HWLOC_LIBS)
endif

force:
$(slurmd_LDADD) : force
	@cd `dirname $@` && $(MAKE) `basename $@`
