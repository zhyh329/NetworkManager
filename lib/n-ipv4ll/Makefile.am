# ------------------------------------------------------------------------------
# autotools

SHELL = bash
ACLOCAL_AMFLAGS = -I build/m4 ${ACLOCAL_FLAGS}
AUTOMAKE_OPTIONS = color-tests parallel-tests
AM_MAKEFLAGS = --no-print-directory
AM_TESTS_ENVIRONMENT = LD_LIBRARY_PATH=$(abs_builddir)
check_PROGRAMS = $(default_tests)
TESTS = $(default_tests)
CLEANFILES =
EXTRA_DIST =

# remove targets if the command fails
.DELETE_ON_ERROR:

# keep intermediate files
.SECONDARY:

# keep the test-suite.log and Makefile around at all times
.PRECIOUS: $(TEST_SUITE_LOG) Makefile

AM_CPPFLAGS = \
	-include $(top_builddir)/build/config.h \
	-I $(top_srcdir)/src \
	-I $(top_builddir)/src

AM_CFLAGS = \
	-fdata-sections \
	-ffunction-sections \
	-fvisibility=hidden \
	-fno-common \
	-fPIC \
	-g \
	-O2 \
	-Wall \
	-Wundef \
	-Wextra \
	-Wshadow \
	-Winit-self \
	-Wformat=2 \
	-Wformat-security \
	-Wformat-nonliteral \
	-Wmissing-prototypes \
	-Wmissing-declarations \
	-Wdeclaration-after-statement \
	-Wredundant-decls \
	-Wno-missing-field-initializers \
	-Wno-unused-parameter \
	-Wno-inline

AM_LDFLAGS = \
	$(NACD_LIBS) \
	-Wl,--as-needed \
	-Wl,--no-undefined \
	-Wl,--gc-sections \
	-Wl,-z,relro \
	-Wl,-z,now \
	-pie

# ------------------------------------------------------------------------------
# libnipv4ll.a

libnipv4ll_a_SOURCES = \
	src/n-ipv4ll.h \
	src/n-ipv4ll.c

# ------------------------------------------------------------------------------
# test-api

test_api_SOURCES = src/test.h src/test-api.c
test_api_LDADD = libnipv4ll.so.0 # explicitly linked against public library

# ------------------------------------------------------------------------------
# test-basic

test_basic_SOURCES = src/test-basic.c
test_basic_LDADD = libnipv4ll.a

# ------------------------------------------------------------------------------
# targets

include_HEADERS = src/n-ipv4ll.h
noinst_LIBRARIES = libnipv4ll.a
SOLIBRARIES = libnipv4ll.so.0
default_tests = \
	test-api \
	test-basic

# ------------------------------------------------------------------------------
# support for $(SOLIBRARIES) (must be after its assignment)

include solibraries.mk
