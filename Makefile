INSTALL_BIN := scribe-bench
all := libscribe scribe-bench

libscribe-obj-y := src/scribe_template.o src/scribe_ingest.o src/scribe_store.o src/scribe_validate.o src/scribe_extract.o

LDLIBS-libscribe := -L../libcorm/lib -L../liblemma/lib -L../libhyle/lib -L../libstele/lib -lcorm -llemma -lhyle -lstele -ljson-c
LDLIBS-scribe-bench := -L./lib -lscribe ${LDLIBS-libscribe} -Wl,-rpath,'$$ORIGIN/../lib'

include ../mk/include.mk
LDLIBS-alpine :=

CFLAGS := -I./include -I../libcorm/include -I../liblemma/include -I../libhyle/include -I../libstele/include -O2 ${CFLAGS} $(EXTRA_CFLAGS)
LDFLAGS := -L./lib -L../libcorm/lib -L../liblemma/lib -L../libhyle/lib -L../libstele/lib ${LDFLAGS}

TEST_LIB_PATH := ./lib:../libcorm/lib:../liblemma/lib:../libhyle/lib:../libstele/lib:/opt/homebrew/lib:/usr/local/lib:/usr/lib
TEST_ENV := LD_LIBRARY_PATH=${TEST_LIB_PATH}:$$LD_LIBRARY_PATH DYLD_LIBRARY_PATH=${TEST_LIB_PATH}:$$DYLD_LIBRARY_PATH

bin/test_scribe_core${EXE}: tests/test_scribe_core.c lib/libscribe.${SO}
	@mkdir -p bin
	${cc} ${CFLAGS} -o $@ tests/test_scribe_core.c ${LDFLAGS} -lscribe ${LDLIBS-libscribe} -Wl,-rpath,'$$ORIGIN/../lib'

bin/test_scribe_extract${EXE}: tests/test_scribe_extract.c lib/libscribe.${SO}
	@mkdir -p bin
	${cc} ${CFLAGS} -o $@ tests/test_scribe_extract.c ${LDFLAGS} -lscribe ${LDLIBS-libscribe} -Wl,-rpath,'$$ORIGIN/../lib'

test: all bin/test_scribe_core${EXE} bin/test_scribe_extract${EXE}
	${TEST_ENV} ./bin/test_scribe_core${EXE}
	${TEST_ENV} ./bin/test_scribe_extract${EXE}

bench: all
	python3 tests/generate_synthetic_certs.py --out tests/fixtures/synthetic
	${TEST_ENV} ./bin/scribe-bench${EXE} --fixtures tests/fixtures/synthetic --templates templates --backend mock --model mock-v1 --model2 mock-v2

${DESTDIR}${PREFIX}/lib/pkgconfig/scribe.pc: scribe.pc
	install -d ${DESTDIR}${PREFIX}/lib/pkgconfig
	install -m 644 scribe.pc $@

install-templates:
	install -d ${DESTDIR}${PREFIX}/share/axil-scribe/templates
	install -m 644 templates/*.json ${DESTDIR}${PREFIX}/share/axil-scribe/templates/

install: ${DESTDIR}${PREFIX}/lib/pkgconfig/scribe.pc install-templates
