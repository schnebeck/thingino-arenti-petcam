#!/bin/sh
# Rebuild stepper for the petcam (MIPS32r2 LE, soft-float, static).
# Toolchain: musl.cc mipsel-linux-muslsf-cross, baked into a local Docker image.
set -e
cd "$(dirname "$0")"

if [ ! -f mipsel-linux-muslsf-cross.tgz ]; then
    curl -fSL https://musl.cc/mipsel-linux-muslsf-cross.tgz -o mipsel-linux-muslsf-cross.tgz
fi

docker build -t stepper-mips-build:muslsf .
docker run --rm -v "$(pwd)":/src stepper-mips-build:muslsf \
    mipsel-linux-muslsf-gcc -EL -mips32r2 -static -Os -Wall -Wextra -o stepper stepper.c
docker run --rm -v "$(pwd)":/src stepper-mips-build:muslsf \
    mipsel-linux-muslsf-strip stepper

echo "Built: $(pwd)/stepper"
file stepper
