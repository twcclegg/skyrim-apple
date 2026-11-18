#!/bin/sh
# Builds ./reproject (Swift + Metal, system frameworks only).
set -e
cd "$(dirname "$0")"
swiftc -O -o reproject Sources/main.swift Sources/Core.swift Sources/Shaders.swift Sources/Synth.swift
echo "built $(pwd)/reproject"
