#!/usr/bin/env bash

set -Eeuo pipefail

# --------------------------------------------------
# Project configuration
# --------------------------------------------------

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"

BUILD_TYPE="${1:-Release}"
JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

# --------------------------------------------------
# Helpers
# --------------------------------------------------

info() {
    echo -e "\033[1;34m[INFO]\033[0m $*"
}

success() {
    echo -e "\033[1;32m[OK]\033[0m $*"
}

error() {
    echo -e "\033[1;31m[ERROR]\033[0m $*" >&2
    exit 1
}

install_debian() {
    info "Installing Debian/Ubuntu dependencies..."

    sudo apt-get update

    sudo apt-get install -y \
        build-essential \
        cmake \
        git \
        pkg-config \
        libgtk-3-dev \
        libglib2.0-dev \
        libx11-dev \
        libxext-dev \
        libxinerama-dev \
        libxrandr-dev \
        libxcursor-dev \
        libxi-dev \
        libxfixes-dev \
        libxtst-dev \
        libgl1-mesa-dev \
        libglu1-mesa-dev \
        libpng-dev \
        libjpeg-dev \
        libtiff-dev \
        libnotify-dev \
        libsm-dev \
        libice-dev
}

install_arch() {
    info "Installing Arch Linux dependencies..."

    sudo pacman -Sy --needed --noconfirm \
        base-devel \
        cmake \
        git \
        pkgconf \
        gtk3 \
        glib2 \
        libx11 \
        libxext \
        libxinerama \
        libxrandr \
        libxcursor \
        libxi \
        libxfixes \
        libxtst \
        mesa \
        glu \
        libpng \
        libjpeg-turbo \
        libtiff \
        libnotify
}

install_fedora() {
    info "Installing Fedora dependencies..."

    sudo dnf install -y \
        gcc \
        gcc-c++ \
        make \
        cmake \
        git \
        pkgconf-pkg-config \
        gtk3-devel \
        glib2-devel \
        libX11-devel \
        libXext-devel \
        libXinerama-devel \
        libXrandr-devel \
        libXcursor-devel \
        libXi-devel \
        libXfixes-devel \
        libXtst-devel \
        mesa-libGL-devel \
        mesa-libGLU-devel \
        libpng-devel \
        libjpeg-turbo-devel \
        libtiff-devel \
        libnotify-devel
}

install_opensuse() {
    info "Installing openSUSE dependencies..."

    sudo zypper install -y \
        gcc \
        gcc-c++ \
        make \
        cmake \
        git \
        pkgconf-pkg-config \
        gtk3-devel \
        glib2-devel \
        libX11-devel \
        libXext-devel \
        libXinerama-devel \
        libXrandr-devel \
        libXcursor-devel \
        libXi-devel \
        libXfixes-devel \
        libXtst-devel \
        Mesa-libGL-devel \
        Mesa-libGLU-devel \
        libpng16-devel \
        libjpeg8-devel \
        libtiff-devel \
        libnotify-devel
}

install_macos() {
    info "Installing macOS dependencies..."

    command -v brew >/dev/null 2>&1 ||
        error "Homebrew is required. Install it from https://brew.sh"

    brew install cmake git pkg-config gtk+3
}

# --------------------------------------------------
# Detect operating system
# --------------------------------------------------

info "Detecting operating system..."

OS="$(uname -s)"

case "${OS}" in
    Linux)
        if [[ ! -f /etc/os-release ]]; then
            error "Cannot detect Linux distribution."
        fi

        source /etc/os-release

        case "${ID}" in
            debian|ubuntu|linuxmint|pop)
                install_debian
                ;;
            arch|manjaro|endeavouros)
                install_arch
                ;;
            fedora)
                install_fedora
                ;;
            opensuse*|sles)
                install_opensuse
                ;;
            *)
                error "Unsupported Linux distribution: ${ID}"
                ;;
        esac
        ;;

    Darwin)
        install_macos
        ;;

    *)
        error "Unsupported operating system: ${OS}"
        ;;
esac

success "Dependencies installed."

# --------------------------------------------------
# Create build directory
# --------------------------------------------------

info "Creating build directory..."

mkdir -p "${BUILD_DIR}"

# --------------------------------------------------
# Configure CMake
# --------------------------------------------------

info "Configuring project..."

cmake \
    -S "${PROJECT_DIR}" \
    -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DFETCHCONTENT_BASE_DIR="${BUILD_DIR}/_deps"

# --------------------------------------------------
# Build
# --------------------------------------------------

info "Building project..."

cmake --build "${BUILD_DIR}" \
    --config "${BUILD_TYPE}" \
    --parallel "${JOBS}"

# --------------------------------------------------
# Finished
# --------------------------------------------------

success "Build completed successfully!"

echo
echo "Project:       ${PROJECT_DIR}"
echo "Build:         ${BUILD_DIR}"
echo "Configuration: ${BUILD_TYPE}"
echo "Jobs:          ${JOBS}"

if [[ "${OS}" == "Linux" ]]; then
    echo
    echo "Wayland sessions: run ./linux-input-setup.sh once so the app may read /dev/input and use /dev/uinput."
fi
