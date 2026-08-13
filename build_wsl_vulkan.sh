#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_ROOT="${ROOT_DIR}/build/wsl-vulkan"
INSTALL_DEPS=0
RUN_EDITOR=0

usage() {
    cat <<'EOF'
Usage: ./build_wsl_vulkan.sh [--install-deps] [--run-editor]

  --install-deps  Install the WSL Vulkan/GLFW/Ninja build prerequisites with apt.
  --run-editor     Run the Debug Editor smoke after both configurations build.
EOF
}

for arg in "$@"; do
    case "${arg}" in
        --install-deps) INSTALL_DEPS=1 ;;
        --run-editor) RUN_EDITOR=1 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown argument: ${arg}" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ "${INSTALL_DEPS}" == 1 ]]; then
    sudo apt-get update
    sudo apt-get install -y \
        build-essential gcc-10 g++-10 cmake ninja-build pkg-config \
        libvulkan-dev mesa-vulkan-drivers vulkan-tools \
        libglfw3-dev libwayland-dev libxkbcommon-dev libx11-xcb-dev
fi

for tool in cmake ninja pkg-config; do
    command -v "${tool}" >/dev/null || {
        echo "Missing ${tool}; rerun with --install-deps." >&2
        exit 1
    }
done

# Ubuntu 20.04 ships GCC 9 by default, but the renderer uses C++20 library
# facilities (including std::span). Prefer the installed modern toolchain
# unless the caller explicitly selected CC/CXX.
if [[ -z "${CC:-}" && -z "${CXX:-}" ]]; then
    if command -v gcc-10 >/dev/null 2>&1 && command -v g++-10 >/dev/null 2>&1; then
        export CC=gcc-10
        export CXX=g++-10
    elif command -v gcc-11 >/dev/null 2>&1 && command -v g++-11 >/dev/null 2>&1; then
        export CC=gcc-11
        export CXX=g++-11
    elif command -v gcc-12 >/dev/null 2>&1 && command -v g++-12 >/dev/null 2>&1; then
        export CC=gcc-12
        export CXX=g++-12
    fi
fi
if [[ -z "${CC:-}" || -z "${CXX:-}" ]]; then
    echo "Missing a C++20-capable GCC/G++; install gcc-10/g++-10 or set CC and CXX." >&2
    exit 1
fi
echo "Using C compiler: ${CC}"
echo "Using C++ compiler: ${CXX}"

pkg-config --exists vulkan || {
    echo "Missing Vulkan loader development files (pkg-config vulkan)." >&2
    exit 1
}
pkg-config --exists glfw3 || {
    echo "Missing GLFW development files (pkg-config glfw3)." >&2
    exit 1
}

if [[ -z "${DXC_PATH:-}" ]]; then
    for candidate in \
        "${ROOT_DIR}/Tools/DXC/1.7.2308/bin/dxc" \
        "${ROOT_DIR}/Tools/DXC/bin/dxc"; do
        if [[ -x "${candidate}" ]]; then
            export DXC_PATH="${candidate}"
            break
        fi
    done
fi
command -v dxc >/dev/null 2>&1 || [[ -x "${DXC_PATH:-}" ]] || {
    echo "Missing native Linux dxc; set DXC_PATH or install DirectXShaderCompiler." >&2
    exit 1
}

mkdir -p "${ROOT_DIR}/.codex-results"
if command -v vulkaninfo >/dev/null 2>&1; then
    vulkaninfo --summary >"${ROOT_DIR}/.codex-results/wsl-vulkaninfo-summary.txt" 2>&1 || true
    # Ubuntu 20.04's vulkan-tools predates --summary; retain a useful device
    # report instead of recording only its usage text.
    if ! grep -Eq 'deviceName|GPU id|llvmpipe' "${ROOT_DIR}/.codex-results/wsl-vulkaninfo-summary.txt"; then
        vulkaninfo >"${ROOT_DIR}/.codex-results/wsl-vulkaninfo-summary.txt" 2>&1 || true
    fi
fi

for config in Debug Release; do
    build_dir="${BUILD_ROOT}-${config,,}"
    cmake -S "${ROOT_DIR}" -B "${build_dir}" -G Ninja \
        -DCMAKE_BUILD_TYPE="${config}" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DNLS_RENDER_API=Vulkan \
        -DNLS_BUILD_TESTS=ON
    cmake --build "${build_dir}" --target Editor NullusUnitTests -j "${NLS_BUILD_JOBS:-$(nproc)}"
    ctest --test-dir "${build_dir}" --output-on-failure
done

if [[ "${RUN_EDITOR}" == 1 ]]; then
    export NLS_VULKAN_VALIDATION=1
    mkdir -p "${ROOT_DIR}/.codex-results/vulkan-editor-smoke"
    "${BUILD_ROOT}-debug/App/Linux_Debug_Shared/Editor" \
        --backend Vulkan \
        --editor-validation-scene-readback-output "${ROOT_DIR}/.codex-results/vulkan-editor-smoke/scene-view.png" \
        --editor-validation-scene-readback-summary "${ROOT_DIR}/.codex-results/vulkan-editor-smoke/scene-view-summary.txt" \
        "${ROOT_DIR}/VulkanSmokeProject"
fi
