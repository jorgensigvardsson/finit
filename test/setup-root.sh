#!/bin/sh

set -eu

# shellcheck disable=SC2154
make -C "$top_builddir" DESTDIR="$TENV_ROOT" install

# shellcheck disable=SC2154
FINITBIN="$(pwd)/$top_builddir/src/finit" DEST="$TENV_ROOT" make -f "$srcdir/tenv/root.mk"

# Drop plugins we don't want to use in test, only causes FAIL in logs.
rm -f "$TENV_ROOT/lib/finit/plugins/urandom.so"
