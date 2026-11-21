#!/bin/sh
set -eu

usage()
{
    printf 'Usage: %s [--install-deps] [--cuda]\n' "$0"
}

as_root()
{
    if [ "$(id -u)" -eq 0 ]; then
        "$@"
    else
        sudo "$@"
    fi
}

install_dependencies()
{
    case "$host" in
        Linux)
            if command -v apt-get >/dev/null 2>&1; then
                as_root apt-get update
                as_root apt-get install -y --no-install-recommends build-essential cmake ninja-build \
                    libopencv-dev libsqlite3-dev libtiff-dev libpng-dev libjpeg-dev zlib1g-dev
            elif command -v dnf >/dev/null 2>&1; then
                as_root dnf install -y gcc-c++ cmake ninja-build opencv-devel sqlite-devel \
                    libtiff-devel libpng-devel libjpeg-turbo-devel zlib-devel
            else
                printf '%s\n' 'Install the compiler, CMake, Ninja and libraries with your distribution package manager.' >&2
                exit 1
            fi
            ;;
        Darwin)
            if ! xcode-select -p >/dev/null 2>&1; then
                printf '%s\n' 'Run xcode-select --install and wait for the command line tools to finish installing.' >&2
                exit 1
            fi
            if ! command -v brew >/dev/null 2>&1; then
                printf '%s\n' 'Install Homebrew or provide the dependencies through CMAKE_PREFIX_PATH.' >&2
                exit 1
            fi
            brew install cmake ninja opencv@4 sqlite libtiff libpng jpeg-turbo
            ;;
    esac
}

install_deps=false
cuda=false
while [ "$#" -gt 0 ]; do
    case "$1" in
        --install-deps) install_deps=true ;;
        --cuda) cuda=true ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
    shift
done

host=$(uname -s)
case "$host" in
    Linux)
        platform=linux
        ;;
    Darwin)
        if "$cuda"; then
            printf '%s\n' 'The CUDA build is supported on Linux only by this script.' >&2
            exit 2
        fi
        platform=macos
        ;;
    *)
        printf '%s\n' 'This script supports Linux and macOS.' >&2
        exit 2
        ;;
esac
machine=$(uname -m)
case "$machine" in
    x86_64|amd64) arch=x64 ;;
    arm64|aarch64) arch=arm64 ;;
    *) printf 'Unsupported %s architecture: %s\n' "$host" "$machine" >&2; exit 2 ;;
esac
preset="$platform-$arch"

if "$install_deps"; then
    install_dependencies
fi

repo=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
cd "$repo"
build_dir="build/$preset"
set -- --preset "$preset"
if "$cuda"; then
    build_dir="$build_dir-cuda"
    set -- "$@" -DMOSAICRAFT_CUDA=ON
fi
cmake "$@" -B "$build_dir"
CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-2}" \
    cmake --build "$build_dir" --target mosaicraft MosaicraftWebUI
