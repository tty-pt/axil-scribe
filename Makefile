INSTALL_BIN := scribe-bench
all := libscribe libaxil-scribe scribe-bench

SONAME-libaxil-scribe := axil-scribe

libscribe-obj-y := src/scribe_template.o src/scribe_ingest.o src/scribe_store.o src/scribe_validate.o src/scribe_extract.o
libaxil-scribe-obj-y := ux/scribe_ux.o

LDLIBS-libscribe := -L../libcorm/lib -L../liblemma/lib -L../libhyle/lib -L../libstele/lib -lcorm -llemma -lhyle -lstele -ljson-c
LDLIBS-libaxil-scribe := -L./lib -lscribe -L../libbud/lib -L../axil/lib -L../libxylem/lib ${LDLIBS-libscribe} -lbud -laxil -lxylem -Wl,-rpath,'$$ORIGIN'
LDLIBS-scribe-bench := -L./lib -lscribe ${LDLIBS-libscribe} -Wl,-rpath,'$$ORIGIN/../lib'

WASI_CC_DEFAULT := $(shell command -v clang-18 2>/dev/null || command -v clang 2>/dev/null || echo clang)
WASI_CC ?= ${WASI_CC_DEFAULT}
SCRIBE_WASM_CFLAGS := --target=wasm32-wasi -Os -DNDEBUG -fvisibility=hidden -D__wasm__ \
	-Wall -Wextra -Wpedantic \
	-I./include -I../libbud/include -I../libstele/include
SCRIBE_WASM_LDFLAGS := -mexec-model=reactor -Wl,--gc-sections -Wl,--no-entry -Wl,--strip-debug \
	-Wl,--export=malloc -Wl,--export=free -Wl,--export=_initialize \
	-Wl,--export=wasm_init -Wl,--export=bud_app_render \
	-Wl,--export=stl_kernel_boot -Wl,--export=stl_kernel_mount \
	-Wl,--export=stl_kernel_unmount -Wl,--export=stl_kernel_update \
	-Wl,--export=stl_kernel_action -Wl,--export=stl_kernel_dispatch_event \
	-Wl,--export=stl_kernel_register_module

SCRIBE_WASM_SRCS := \
	ux/scribe_ux.c \
	../libstele/src/stele_wasm_kernel.c \
	../libstele/src/stele_event.c \
	../libbud/src/libbud.c \
	../libbud/src/bud_tpl.c \
	../libbud/src/bud_render.c \
	../libbud/src/bud_runtime.c \
	../libbud/src/bud_json.c \
	../libbud/src/bud_reconcile.c

CLEAN_EXTRA := ux/*.o ux/*.o.d htdocs/scribe_ux.wasm bin/test_scribe_core${EXE} bin/test_scribe_extract${EXE} bin/test_scribe_ux${EXE}

include ../mk/include.mk
LDLIBS-alpine :=

CFLAGS := -I./include -I../libcorm/include -I../liblemma/include -I../libhyle/include -I../libbud/include -I../libstele/include -I../libxylem/include -I../axil/include -O2 ${CFLAGS} $(EXTRA_CFLAGS)
LDFLAGS := -L./lib -L../libcorm/lib -L../liblemma/lib -L../libhyle/lib -L../libbud/lib -L../libstele/lib -L../libxylem/lib -L../axil/lib ${LDFLAGS}

TEST_LIB_PATH := ./lib:../libcorm/lib:../liblemma/lib:../libhyle/lib:../libbud/lib:../libstele/lib:../libxylem/lib:../axil/lib:/opt/homebrew/lib:/usr/local/lib:/usr/lib
TEST_ENV := LD_LIBRARY_PATH=${TEST_LIB_PATH}:$$LD_LIBRARY_PATH DYLD_LIBRARY_PATH=${TEST_LIB_PATH}:$$DYLD_LIBRARY_PATH

lib/libaxil-scribe.${SO}: lib/libscribe.${SO}

ux/scribe_ux.o: ux/scribe_ux.c include/ttypt/scribe-ux.h
	${cc} -MMD -MP -MT $@ -MF $@.d -fPIC ${CFLAGS} -c -o $@ $<

htdocs/scribe_ux.wasm: ${SCRIBE_WASM_SRCS} include/ttypt/scribe-ux.h
	@mkdir -p htdocs
	${WASI_CC} ${SCRIBE_WASM_CFLAGS} ${SCRIBE_WASM_LDFLAGS} -o $@ ${SCRIBE_WASM_SRCS}
	@if command -v wasm-strip >/dev/null 2>&1; then wasm-strip $@; fi

wasm-ux: htdocs/scribe_ux.wasm

all: htdocs/scribe_ux.wasm

bin/test_scribe_core${EXE}: tests/test_scribe_core.c lib/libscribe.${SO}
	@mkdir -p bin
	${cc} ${CFLAGS} -o $@ tests/test_scribe_core.c ${LDFLAGS} -lscribe ${LDLIBS-libscribe} -Wl,-rpath,'$$ORIGIN/../lib'

bin/test_scribe_extract${EXE}: tests/test_scribe_extract.c lib/libscribe.${SO}
	@mkdir -p bin
	${cc} ${CFLAGS} -o $@ tests/test_scribe_extract.c ${LDFLAGS} -lscribe ${LDLIBS-libscribe} -Wl,-rpath,'$$ORIGIN/../lib'

bin/test_scribe_ux${EXE}: tests/test_scribe_ux.c ux/scribe_ux.o
	@mkdir -p bin
	${cc} ${CFLAGS} -o $@ tests/test_scribe_ux.c ux/scribe_ux.o ${LDFLAGS} -lbud -lstele -Wl,-rpath,'$$ORIGIN/../lib'

test: all bin/test_scribe_core${EXE} bin/test_scribe_extract${EXE} bin/test_scribe_ux${EXE}
	${TEST_ENV} ./bin/test_scribe_core${EXE}
	${TEST_ENV} ./bin/test_scribe_extract${EXE}
	${TEST_ENV} ./bin/test_scribe_ux${EXE}
	bash ./test.sh

bench: all
	python3 tests/generate_synthetic_certs.py --out tests/fixtures/synthetic
	${TEST_ENV} ./bin/scribe-bench${EXE} --fixtures tests/fixtures/synthetic --templates templates --backend mock --model mock-v1 --model2 mock-v2

bench-handwriting: all
	python3 tests/fetch_real_handwriting.py --out tests/fixtures/real_handwriting
	${TEST_ENV} ./bin/scribe-bench${EXE} --fixtures tests/fixtures/real_handwriting --pack packs/pt-archival-handwriting --backend auren --model fast --model2 very-fast --escalate smart --enhance --verbose

${DESTDIR}${PREFIX}/lib/pkgconfig/scribe.pc: scribe.pc
	install -d ${DESTDIR}${PREFIX}/lib/pkgconfig
	install -m 644 scribe.pc $@

${DESTDIR}${PREFIX}/lib/pkgconfig/axil-scribe.pc: axil-scribe.pc
	install -d ${DESTDIR}${PREFIX}/lib/pkgconfig
	install -m 644 axil-scribe.pc $@

install-templates:
	install -d ${DESTDIR}${PREFIX}/share/axil-scribe/templates
	install -m 644 templates/*.json ${DESTDIR}${PREFIX}/share/axil-scribe/templates/

install-packs:
	install -d ${DESTDIR}${PREFIX}/share/axil-scribe/packs
	cp -R packs/. ${DESTDIR}${PREFIX}/share/axil-scribe/packs/

install-htdocs: htdocs/scribe_ux.wasm
	install -d ${DESTDIR}${PREFIX}/share/axil-scribe/htdocs
	install -m 644 htdocs/scribe_ux.wasm ${DESTDIR}${PREFIX}/share/axil-scribe/htdocs/

install-scripts: scripts/scribe-server tests/fetch_real_handwriting.py
	install -d ${DESTDIR}${PREFIX}/bin
	install -m 755 scripts/scribe-server ${DESTDIR}${PREFIX}/bin/scribe-server
	install -d ${DESTDIR}${PREFIX}/share/axil-scribe/tests
	install -m 755 tests/fetch_real_handwriting.py ${DESTDIR}${PREFIX}/share/axil-scribe/tests/fetch_real_handwriting.py

install: ${DESTDIR}${PREFIX}/lib/pkgconfig/scribe.pc ${DESTDIR}${PREFIX}/lib/pkgconfig/axil-scribe.pc install-templates install-packs install-htdocs install-scripts
