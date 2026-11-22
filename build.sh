#!/bin/sh
# Baut den Debug-Build, ohne die Makefile zu aendern.
# libogc 3.x und libwiisocket definieren beide inet_ntop/inet_pton; der Linker soll das akzeptieren.
exec make LDFLAGS='-g $(MACHDEP) -Wl,-Map,$(notdir $@).map -Wl,--allow-multiple-definition' "$@"
