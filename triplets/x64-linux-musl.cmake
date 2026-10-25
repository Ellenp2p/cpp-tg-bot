# musl 静态 triplet：在 Alpine 容器内使用（vcpkg 官方已移除内置 musl triplet，
# 这里以 overlay 方式提供。工具链来自容器环境本身，即 musl-gcc）
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
