#!/bin/bash

set -x # expand and echo commands
export SPHERE_TRAVIS=1

echo TRAVIS_TAG is $TRAVIS_TAG
echo TRAVIS_BUILD_DIR is $TRAVIS_BUILD_DIR
echo TRAVIS_OS_NAME is $TRAVIS_OS_NAME
export TRAVIS_TAG=master
git tag master -f

if [[ "$TRAVIS_OS_NAME" == "osx" ]]; then
    export HOMEBREW_NO_AUTO_UPDATE=1
    brew install ninja
    brew install mysql-client
    gcc -v && g++ -v && cmake --version
    echo Building Release
    mkdir -p release && cd release && cmake -G Ninja ../src -DCMAKE_BUILD_TYPE=Release && cmake --build . || exit 1
    echo Building $BUILD package
    cd ..
    zip -r sphereX64-osx.zip ./release/bin64
fi

if [[ "$TRAVIS_OS_NAME" == "windows" ]]; then
    echo Building Release
    mkdir release
    cd release
    cmake -G "Visual Studio 15 2017 Win64" ../src && cmake --build . --config Nightly
    cd ..
    echo Building $BUILD package
    7z a sphereX64-windows.zip ./release/bin64/*
    echo ** End of the compilation of the 64 bits Nightly version **
fi
