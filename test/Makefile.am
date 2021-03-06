include $(top_builddir)/version.mk

# Help the Developers and yourself. Just use the C locale and settings
# for the compilation. They can still be overriden by make LANG=<whatever>
# but that is general a not very good idea
LANG=C
LC_ALL=C

AM_CPPFLAGS = -D_GNU_SOURCE -D_XOPEN_SOURCE=600 -D_POSIX_C_SOURCE=201001L
AM_CPPFLAGS += -I$(abs_top_srcdir)/src
AM_LDFLAGS = -L$(abs_top_builddir)/src

EXTRA_DIST = $(TESTS)
EXTRA_DIST += $(BUILT_SOURCES)
check_PROGRAMS =
TESTS =
BUILT_SOURCES =

TEST_EXTENSIONS = .truftest
TRUFTEST_LOG_COMPILER = $(builddir)/truf-test
AM_TRUFTEST_LOG_FLAGS = --builddir $(top_builddir)/src --hash sha1sum
LOG_COMPILER = echo

check_PROGRAMS += truf-test
truf_test_LDADD = ../src/libtruffle.a
BUILT_SOURCES += truf-test-clo.c truf-test-clo.h
EXTRA_DIST += truf-test.sh

TESTS += toy1.1.truftest
TESTS += toy1.2.truftest
EXTRA_DIST += toy1.schema toy1.series

TESTS += toy2.1.truftest
TESTS += toy2.2.truftest
EXTRA_DIST += toy2.schema toy2.series

TESTS += toy3.1.truftest
TESTS += toy3.2.truftest
EXTRA_DIST += toy3.schema toy3.series

TESTS += toy4.1.truftest
TESTS += toy4.2.truftest
EXTRA_DIST += toy4.schema toy4.series

TESTS += toy5.1.truftest
TESTS += toy5.2.truftest
EXTRA_DIST += toy5.schema toy5.series

TESTS += toy6.1.truftest
EXTRA_DIST += toy6.schema

TESTS += toy7.1.truftest
EXTRA_DIST += toy7.schema

TESTS += toy8.1.truftest
TESTS += toy8.2.truftest
EXTRA_DIST += toy8.schema

TESTS += schema_to_trod.1.deflt.truftest
TESTS += schema_to_trod.1.abs.truftest
TESTS += schema_to_trod.1.oco.truftest

TESTS += schema_to_trod.2.deflt.truftest
TESTS += schema_to_trod.2.abs.truftest
TESTS += schema_to_trod.2.oco.truftest

TESTS += schema_to_trod.3.deflt.truftest
TESTS += schema_to_trod.3.abs.truftest
TESTS += schema_to_trod.3.oco.truftest

TESTS += schema_to_trod.4.deflt.truftest
EXTRA_DIST += toy4.trod

TESTS += schema_to_trod.5.deflt.truftest
EXTRA_DIST += toy5.trod

TESTS += schema_to_trod.6.deflt.truftest
TESTS += schema_to_trod.6.abs.truftest

TESTS += schema_to_trod.7.deflt.truftest
TESTS += schema_to_trod.7.abs.truftest

TESTS += trod_to_trod.5.deflt.truftest
TESTS += trod_to_trod.5.abs.truftest
TESTS += trod_to_trod.5.oco.truftest
EXTRA_DIST += toy5.trod

TESTS += toy1.1.mmy.truftest
TESTS += toy1.2.mmy.truftest
EXTRA_DIST += toy1.mmy.series

TESTS += trod.1.truftest
TESTS += trod.1.f.truftest
EXTRA_DIST += toy1.trod

TESTS += trod.2.truftest
TESTS += trod.2.f.truftest
EXTRA_DIST += toy2.trod

TESTS += trod.2.abs.truftest
TESTS += trod.2.abs.f.truftest
EXTRA_DIST += toy2.abs.trod

TESTS += trod.2.oco.truftest
TESTS += trod.2.oco.f.truftest
EXTRA_DIST += toy2.oco.trod

TESTS += truffle_schema_contracts.1.deflt.truftest
TESTS += truffle_schema_contracts.1.abs.truftest
TESTS += truffle_schema_contracts.1.oco.truftest

TESTS += truffle_trod_contracts.1.deflt.truftest
TESTS += truffle_trod_contracts.1.abs.truftest
TESTS += truffle_trod_contracts.1.oco.truftest

## gengetopt rule with sed hack to avoid using VERSION and config.h
## depend on Makefile.am to survive a git pull
%.c %.h: %.ggo Makefile.am
	gengetopt -l -i $< -F $*  \
	&& sed \
	  -e 's,config.h,version.h,g' \
	  -e 's,\(.*\)_PARSER_VERSION VERSION,\1_PARSER_VERSION git_version_string,g' \
	  $*.h >$*.h-t \
	&& mv $*.h-t $*.h

clean-local:
	-rm -rf *.tmpd

## Makefile.am ends here
