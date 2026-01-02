#!/bin/bash
find . -name "*.c" -exec sh -c 'gcc "$1" -o "${1%.c}" -lnuma' _ {} \;
