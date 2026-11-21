# SDD-013: the local sleep model's runtime, llama.cpp, through FetchContent.
#
# Pinned to the commit the narrator model was exported and measured with
# (SDD-013 Status, 2026-11-21): the model's tokenizer fixes (add_space_prefix,
# the user-defined <n> and <|summary|> pieces) were checked against exactly
# this llama.cpp, so the pin moves only together with a re-check of the model.
#
# What is built: the llama and ggml libraries, static, CPU only. No curl, no
# server, no tools, examples or tests, no GPU backend, no OpenMP (one runtime
# fewer to ship; ggml's own thread pool does the work). GGML_NATIVE is off so a
# build runs on any machine of its architecture, not only the one it was built
# on; on x86 the SIMD extensions are off too, because "off" there still turns
# AVX2 on, and NAS-class CPUs (Atom, Celeron, Pentium Silver) have none. The
# armhf toolchain gives ARMv6/VFPv2 without NEON, for every 32-bit Pi.
#
# A local checkout is used instead of the download with
#   -DFETCHCONTENT_SOURCE_DIR_LLAMA_CPP=/path/to/llama.cpp
# (it must be at the pinned commit). -DBUILD_WITH_LOCAL_MODEL=OFF drops the
# runtime: GgufModel then reports that the build has none.

option(BUILD_WITH_LOCAL_MODEL "Link the local sleep model's runtime (llama.cpp, SDD-013)" ON)

set(HMS_LLAMA_CPP_COMMIT 919794500475862f6768c353255469dc661cb734)

# A function, so the options below stay llama.cpp's and do not leak into the
# rest of the build (BUILD_SHARED_LIBS above all).
function(hms_local_model_fetch)
    # llama.cpp writes Release into the cache when no build type is given,
    # which would change the build type of all of hms-cpap (the Dockerfile
    # gives none). Set here, it is the build type of llama.cpp's directories
    # only, and ggml is optimized either way.
    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE Release)
    endif()
    set(BUILD_SHARED_LIBS OFF)
    set(LLAMA_BUILD_COMMON OFF)
    set(LLAMA_BUILD_TESTS OFF)
    set(LLAMA_BUILD_TOOLS OFF)
    set(LLAMA_BUILD_EXAMPLES OFF)
    set(LLAMA_BUILD_SERVER OFF)
    set(LLAMA_BUILD_APP OFF)
    set(LLAMA_BUILD_UI OFF)
    set(LLAMA_OPENSSL OFF)
    set(LLAMA_SUBPROCESS OFF)
    set(LLAMA_ALL_WARNINGS OFF)
    set(LLAMA_BUILD_IS_DEV OFF)
    set(GGML_NATIVE OFF CACHE BOOL "ggml: optimize the build for the current system")
    set(GGML_CPU ON)
    set(GGML_OPENMP OFF)
    set(GGML_METAL OFF)
    set(GGML_BLAS OFF)
    set(GGML_ACCELERATE OFF)
    set(GGML_VULKAN OFF)
    set(GGML_CUDA OFF)
    set(GGML_CCACHE OFF)
    set(GGML_BACKEND_DL OFF)
    if(NOT GGML_NATIVE)
        foreach(ext SSE42 AVX AVX2 BMI2 FMA F16C)
            set(GGML_${ext} OFF)
        endforeach()
    endif()
    FetchContent_Declare(llama_cpp
        GIT_REPOSITORY https://github.com/ggml-org/llama.cpp.git
        GIT_TAG        ${HMS_LLAMA_CPP_COMMIT}
    )
    FetchContent_MakeAvailable(llama_cpp)
endfunction()

if(BUILD_WITH_LOCAL_MODEL)
    hms_local_model_fetch()
    # 32-bit ARM without the 64-bit compare-and-swap (ARMv6) does ggml's 64-bit
    # atomics through libatomic. It has to come after the ggml archives on the
    # link line, which a link-only dependency of ggml-base gives.
    include(CheckCXXSourceCompiles)
    check_cxx_source_compiles("
        #include <atomic>
        #include <cstdint>
        std::atomic<int64_t> v{0};
        int main() { return static_cast<int>(v.fetch_add(1)); }"
        HMS_ATOMIC64_WITHOUT_LIBATOMIC)
    if(NOT HMS_ATOMIC64_WITHOUT_LIBATOMIC AND NOT MSVC)
        target_link_libraries(ggml-base PRIVATE atomic)
        message(STATUS "Local sleep model: 64-bit atomics need libatomic")
    endif()
    message(STATUS "Local sleep model: llama.cpp ${HMS_LLAMA_CPP_COMMIT} (CPU, static)")
else()
    message(STATUS "Local sleep model: not in this build (BUILD_WITH_LOCAL_MODEL=OFF)")
endif()

# [target] links the runtime and is compiled with HMS_WITH_LOCAL_MODEL=1, which
# is what makes GgufModel::runtimeAvailable() true.
function(hms_link_local_model target)
    if(BUILD_WITH_LOCAL_MODEL)
        target_link_libraries(${target} llama)
        target_compile_definitions(${target} PRIVATE HMS_WITH_LOCAL_MODEL=1)
    endif()
endfunction()
