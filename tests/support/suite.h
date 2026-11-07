/* Conventions shared by the test runner and its suites. */
#ifndef DEBRIS_TEST_SUITE_H
#define DEBRIS_TEST_SUITE_H

/* Return value of a suite that cannot run on this machine (no OpenGL driver, no player executable). The same code ctest
 * understands as "skipped". */
#define DEBRIS_TEST_SKIP 77

#ifndef DEBRIS_ASSETS_DIR
#error "DEBRIS_ASSETS_DIR must name the directory holding debris_loader.v2m and debris_party.kx"
#endif
#ifndef DEBRIS_TEST_DATA_DIR
#error "DEBRIS_TEST_DATA_DIR must name tests/data (the reference fixtures)"
#endif

#endif
