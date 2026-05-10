#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p vendor
curl -fsSL https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.h -o vendor/cJSON.h
curl -fsSL https://raw.githubusercontent.com/DaveGamble/cJSON/master/cJSON.c -o vendor/cJSON.c
