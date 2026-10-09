# Regression checks

Run `make test` with GCC or Clang, a C99 mode, and the system math library
available. The recipe uses GCC/Clang's `-Wno-unused-function` flag. The test
compiles the production `vanitygaps.c` layout code into a small executable,
records the geometry requests from `tile` and `righttile`, and checks the
layout-specific `adjustmfact` calculation used by `setmfact`. It does not need
an X server.

The geometry harness supplies the monitor/client iteration and resize boundary
used by the layout code. It verifies requested rectangles; it does not test X
server placement, size hints, or full key event dispatch. The normal `make`
target remains the production build check.
