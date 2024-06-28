#!/bin/bash

if test \( $# -ne 1 \);
then
    echo "Usage: ./build_macos.sh config"
    echo ""
    echo "config:"
    echo "  debug   -   build with the debug configuration"
    echo "  release -   build with the release configuration"
    echo ""
    exit 1
fi


if test \( \( -n "$1" \) -a \( "$1" = "debug" \) \);then
    CONFIG=" Debug"
elif test \( \( -n "$1" \) -a \( "$1" = "release" \) \);then
    CONFIG=" Release"
else
    echo "The config \"$1\" is not supported!"
    echo ""
    echo "Configs:"
    echo "  debug   -   build with the debug configuration"
    echo "  release -   build with the release configuration"
    echo ""
    exit 1
fi

# 设置编译选项
CMAKE_OPTIONS="-Wno-shorten-64-to-32"
if [ "$CONFIG" = "Release" ]; then
    CMAKE_OPTIONS="$CMAKE_OPTIONS -Werror"
fi

cmake -S . -B build -G "Xcode" -DCMAKE_CXX_FLAGS="$CMAKE_OPTIONS"
#cmake -S . -B build -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++-11
cmake --build build --config "${CONFIG}"
