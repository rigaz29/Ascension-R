cmake_minimum_required(VERSION 3.20)
project(GowARecomp
    VERSION 0.1.0
    LANGUAGES C CXX
    DESCRIPTION "God of War: Ascension (PS3, NPUA80918) static recompilation on ps3recomp"
)

# ---------------------------------------------------------------------------
# Standards
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Prefer Ninja if available
if(NOT CMAKE_GENERATOR MATCHES "Ninja" AND NOT DEFINED ENV{CMAKE_GENERATOR})
    message(STATUS "Tip: use -G Ninja for faster builds")
endif()

# ---------------------------------------------------------------------------
# The two directories this project is built from
# ---------------------------------------------------------------------------
# PS3RECOMP_DIR is the toolkit checkout: include/, runtime/, libs/, tools/.
# RECOMP_DIR is where the lifter's output for THIS game lives. They are
# separate on purpose -- a port keeps its generated code in its own tree and
# points at whatever toolkit checkout it is building against:
#
#   cmake -B build -G Ninja \
#         -DPS3RECOMP_DIR=/path/to/ps3recomp \
#         -DRECOMP_DIR=/path/to/my_game/recompiled
#
if(NOT DEFINED PS3RECOMP_DIR)
    # Default: the ps3recomp fork checked out as this repo's submodule
    # (rigaz29/ps3recomp, branch gowa).
    get_filename_component(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ps3recomp" ABSOLUTE)
endif()
get_filename_component(PS3RECOMP_DIR "${PS3RECOMP_DIR}" ABSOLUTE)

set(RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/recompiled" CACHE PATH
    "Directory holding the lifter's output: ppu_recomp.h and the lifted sources")
get_filename_component(RECOMP_DIR "${RECOMP_DIR}" ABSOLUTE)

message(STATUS "ps3recomp SDK:   ${PS3RECOMP_DIR}")
message(STATUS "Lifted output:   ${RECOMP_DIR}")

if(NOT EXISTS "${RECOMP_DIR}/ppu_recomp.h")
    message(FATAL_ERROR
        "No ppu_recomp.h in ${RECOMP_DIR}.\n"
        "That header and the lifted sources beside it are what "
        "tools/ppu_lifter.py writes. Run the lifter, drop its output there, "
        "or point -DRECOMP_DIR= at wherever it already is.")
endif()

# The lifted translation units. A game's lift is hundreds of generated files,
# so they are globbed rather than listed; re-run CMake after a fresh lift.
file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS
    "${RECOMP_DIR}/*.c"
    "${RECOMP_DIR}/*.cpp"
)
if(NOT RECOMP_SOURCES)
    message(FATAL_ERROR
        "${RECOMP_DIR} has a ppu_recomp.h but no .c or .cpp beside it. The "
        "lifted functions and the function table live in those; without them "
        "there is nothing to run.")
endif()
list(LENGTH RECOMP_SOURCES RECOMP_SOURCE_COUNT)
message(STATUS "Lifted sources:  ${RECOMP_SOURCE_COUNT} file(s)")

# ---------------------------------------------------------------------------
# ps3recomp runtime library
# ---------------------------------------------------------------------------
# Built from source as a subdirectory rather than imported as a prebuilt
# archive. A static library carries none of its own link requirements, so an
# imported one arrives without SDL2, pthreads, the Metal frameworks or the
# Winsock/XInput imports -- the link then fails on every platform for a
# different reason. add_subdirectory hands all of that over with the target.
# PORT_SPU_JOBCAP: SPU job capture and replay (SPU_JOB_CAPTURE, SPU_JOB_REPLAY,
# gowa.py replay). Its hooks wrap the runtime's DMA, channel and drain paths, so
# only measurement builds have them (gowa.py build --jobcap) and a default build
# compiles those paths as before. Before add_subdirectory: the runtime library
# needs the definition too.
option(PORT_SPU_JOBCAP "SPU job capture and replay hooks (measurement builds)" OFF)
get_directory_property(port_compile_defs COMPILE_DEFINITIONS)
if(PORT_SPU_JOBCAP)
    add_compile_definitions(PS3_SPU_JOBCAP)
endif()
add_subdirectory("${PS3RECOMP_DIR}" "${CMAKE_BINARY_DIR}/ps3recomp_sdk"
                 EXCLUDE_FROM_ALL)
# The lifted SPU code takes it with its other definitions (spu_defs below); the
# lifted PPU code has none of the hooks, and with the definition on its command
# line every switch rebuilt all of it.
set_directory_properties(PROPERTIES COMPILE_DEFINITIONS "${port_compile_defs}")

# ---------------------------------------------------------------------------
# The HLE NID table
# ---------------------------------------------------------------------------
# Maps each firmware import NID onto the handler in the runtime that answers
# it. A port may keep a hand-curated one beside its lift; otherwise generate
# the canonical every-module table, which is what the toolkit's own boot
# harness uses.
if(EXISTS "${RECOMP_DIR}/ppu_hle_nids.cpp")
    set(HLE_NID_TABLE "${RECOMP_DIR}/ppu_hle_nids.cpp")
    message(STATUS "HLE NID table:   ${HLE_NID_TABLE}")
else()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(HLE_NID_TABLE "${CMAKE_CURRENT_BINARY_DIR}/ppu_hle_nids.cpp")
    # The generator discovers handlers from library definitions. Rebuild when
    # an existing module changes, and reconfigure when a new module is added.
    file(GLOB_RECURSE HLE_NID_SOURCES CONFIGURE_DEPENDS
        "${PS3RECOMP_DIR}/libs/*.c")
    # A removed input cannot make the output older than a remaining dependency.
    # Track the list itself so deletions also remove stale handler references.
    set(HLE_NID_SOURCE_LIST "${CMAKE_CURRENT_BINARY_DIR}/hle_nid_sources.txt")
    file(GENERATE OUTPUT "${HLE_NID_SOURCE_LIST}" CONTENT "${HLE_NID_SOURCES}\n")
    add_custom_command(
        OUTPUT "${HLE_NID_TABLE}"
        COMMAND "${Python3_EXECUTABLE}"
                "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                --out "${HLE_NID_TABLE}" --all
        DEPENDS "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                "${PS3RECOMP_DIR}/tools/nid_database.py"
                ${HLE_NID_SOURCES} "${HLE_NID_SOURCE_LIST}"
        COMMENT "Generating the HLE NID -> handler table")
    message(STATUS "HLE NID table:   generated (every module)")
endif()

# ---------------------------------------------------------------------------
# The game executable
# ---------------------------------------------------------------------------
# The PPU boot scaffold is compiled here rather than taken from the runtime
# library: every one of these files #includes the lifter's per-game
# ppu_recomp.h, so they can only be built against a particular lift.
set(PS3RECOMP_SCAFFOLD
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"
)

# ---------------------------------------------------------------------------
# Lifted SPU images (optional)
# ---------------------------------------------------------------------------
# A title that runs SPU work gets its images lifted into <SPU_DIR>/<image>/ by
# tools/spu_lifter.py, with a registration TU generated by
# tools/build_spu_workloads.py. cellSpurs dispatches them by content
# fingerprint, so the generated table is keyed to the images THIS build ships:
# a table generated from another release registers signatures that can never
# match, which is worse than shipping none. Both the table and the job-module
# registry self-register from constructors and are therefore optional -- a tree
# that has not lifted its SPU images yet configures and links without them.
set(SPU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/spu" CACHE PATH
    "Directory holding the lifted SPU images (<SPU_DIR>/<image>/spu_recomp.c)")
option(PORT_ENABLE_SPU "Compile and register the title's lifted SPU images" ON)

set(PORT_SPU_INPUTS "" CACHE STRING
    "Input files to scan for all embedded SPU ELF programs (semicolon-separated decrypted ELF/PRX/data paths)")
set(SPU_SOURCES "")
# SPU loops that span many small chunks: lifted one function per chunk, every
# branch between them is a trampoline round trip. Merging them was worth a fifth
# of a particle task's time in the God of War HD port.
option(PORT_SPU_MERGE_CHUNKS "Lift the embedded SPU programs with --merge-chunks" ON)
if(PORT_ENABLE_SPU AND PORT_SPU_INPUTS)
    include("${PS3RECOMP_DIR}/cmake/PS3RecompSPU.cmake")
    set(PS3RECOMP_SPU_MERGE_CHUNKS ${PORT_SPU_MERGE_CHUNKS})
    ps3recomp_generate_spu("${CMAKE_CURRENT_BINARY_DIR}/generated_spu" ${PORT_SPU_INPUTS})
    list(APPEND SPU_SOURCES ${SPU_GENERATED_SOURCES}
         "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c")
elseif(PORT_ENABLE_SPU)
    file(GLOB SPU_LIFTED CONFIGURE_DEPENDS "${SPU_DIR}/*/spu_recomp.c")
    if(SPU_LIFTED)
        list(LENGTH SPU_LIFTED SPU_IMAGE_COUNT)
        message(STATUS "Lifted SPU:      ${SPU_IMAGE_COUNT} image(s)")
        # The musttail indirect-branch fast path. Excluded from the runtime
        # library on purpose (see the SDK root CMakeLists): it must be built
        # here, by clang, or the tail call degrades to a plain call and a guest
        # loop that branches indirectly walks the thread stack off its guard
        # page a few thousand iterations in.
        list(APPEND SPU_SOURCES ${SPU_LIFTED}
             "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c")
        # Generated beside the lift; each is present only once its generator
        # has been run, so each is included only if it is actually here.
        foreach(gen IN ITEMS spu_workloads.c jobmods_register.c taskset_policy_bytes.c)
            if(EXISTS "${SPU_DIR}/${gen}")
                list(APPEND SPU_SOURCES "${SPU_DIR}/${gen}")
            else()
                message(STATUS "  (no ${gen} -- not registered)")
            endif()
        endforeach()
    else()
        message(STATUS "Lifted SPU:      none found in ${SPU_DIR}")
    endif()
endif()

# The Wws_Job policy module (the title's SPURS job workloads), a raw SPU image
# in the game ELF that `scripts/gowa.py lift` lifts into spu/wwspm/. It builds
# with the other lifted SPU sources.
file(GLOB WWS_PM_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/spu/wwspm/*.c")
if(WWS_PM_SOURCES)
    message(STATUS "Wws_Job PM:      lifted")
    list(APPEND SPU_SOURCES ${WWS_PM_SOURCES})
endif()

# The WWS job modules the title links into its own ELF, which the policy module
# streams into local store from fixed addresses. `scripts/gowa.py lift` lifts
# them (tools/lift_jobmods.py --engine-elf) into spu/jobmods/, and each
# registers as a verified region at its address, so a job runs lifted rather
# than in the SPU interpreter.
file(GLOB JOBMOD_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/spu/jobmods/*/spu_recomp.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/spu/jobmods/jobmods_register.c")
if(JOBMOD_SOURCES)
    message(STATUS "WWS job modules: lifted")
    list(APPEND SPU_SOURCES ${JOBMOD_SOURCES})
endif()

# The position-independent code pieces the EDGE jobs stream to wherever they
# have room (tools/lift_spu_pic.py, from scripts/spu_pic_pieces.json): each
# registers as a position-independent verified region and runs lifted at
# whatever address it lands.
file(GLOB PIC_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/spu/picfrags/*/spu_recomp.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/spu/picfrags/pic_register.c")
if(PIC_SOURCES)
    message(STATUS "SPU PIC pieces:  lifted")
    list(APPEND SPU_SOURCES ${PIC_SOURCES})
endif()

# ---------------------------------------------------------------------------
# Lifted libsre (optional)
# ---------------------------------------------------------------------------
# `scripts/gowa.py lle` relocates the firmware's libsre and lifts it into
# firmware/libsre/lifted/. stubs.cpp then loads it so the SPURS lock-free queue
# functions run as the firmware code; the rest of cellSpurs stays HLE. Without
# that directory the port builds as before.
set(LIBSRE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/firmware/libsre/lifted" CACHE PATH
    "Directory holding the lifted libsre (libsre_recomp_*.cpp, gowa_libsre_exports.c)")
file(GLOB LIBSRE_SOURCES CONFIGURE_DEPENDS "${LIBSRE_DIR}/*.cpp" "${LIBSRE_DIR}/*.c")
if(LIBSRE_SOURCES)
    message(STATUS "Lifted libsre:   ${LIBSRE_DIR}")
    set_source_files_properties(stubs.cpp ${LIBSRE_SOURCES}
        "${PS3RECOMP_DIR}/runtime/prx/prx_loader.c" PROPERTIES
        INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/prx"
        COMPILE_DEFINITIONS GOWA_LIBSRE_LLE)
    list(APPEND LIBSRE_SOURCES "${PS3RECOMP_DIR}/runtime/prx/prx_loader.c")
else()
    message(STATUS "Lifted libsre:   none (SPURS queues stay HLE)")
endif()

add_executable(${PROJECT_NAME}
    main.cpp
    launcher.cpp
    stubs.cpp
    poll_wait.cpp
    ${PS3RECOMP_SCAFFOLD}
    ${HLE_NID_TABLE}
    ${RECOMP_SOURCES}
    ${SPU_SOURCES}
    ${LIBSRE_SOURCES}
)

# The lifted SPU sources use bare #include "spu_context.h". Scope that include
# directory (and their flags) to those TUs alone: adding it to the whole target
# changes the compile command of every lifted PPU TU, and those are large enough
# that the needless rebuild is measured in tens of minutes.
#   SPU_USE_DISPATCH_MT -- route SPU_IB_DISPATCH through the musttail path.
#   -O2                 -- the target-wide -O1 exists for the huge PPU TUs; the
#                          SPU sources are small and decode-hot, so they get
#                          full optimization. The last -O on the line wins.
#   -msse4.1 / -mavx2   -- the SPU helpers pick their SIMD forms at compile time:
#                          pshufb for shufb (SSE4.1), f64x4 for fa/fm/fma (AVX2).
#                          Built for baseline x86-64, shufb was a byte loop and
#                          the float ops ran lane by lane: 55% and 25% of an
#                          audio mixer's SPU time in the God of War HD port.
#                          PORT_SPU_AVX2=OFF keeps SSE4.1 only, for CPUs without
#                          AVX2.
#   /GS-                -- no stack cookie in lifted SPU functions (below).
#   -mfma               -- with AVX2 (every AVX2 CPU has FMA3): SPU_FLOAT_RZ's fma is
#                          one vfmadd. -ffp-contract=off keeps clang from fusing
#                          anything else, as built without FMA3 it could not.
#   PORT_SPU_LINES      -- line tables for the lifted SPU code and a PDB, so a
#                          PS3_SAMPLE_THREAD profile names the lifted C line,
#                          i.e. the SPU instruction, each sample landed on
#                          (gowa.py symbolize --lines). Line tables do not
#                          change the generated code; the link keeps /OPT:REF
#                          and /OPT:ICF, which /DEBUG would otherwise drop.
#   PORT_SPU_FLOAT_RZ_ONLY -- with AVX2: the lifted SPU code carries only the
#                          round-toward-zero forms of single precision
#                          (SPU_FLOAT_RZ_ONLY), and the port always runs SPU code
#                          in that mode. Chosen at run time, every float operation
#                          was compiled in both forms -- the exact one widened to
#                          double beside the native one -- about two and a half
#                          times the code of a float-heavy job, for the same
#                          results (docs/notes/2026-10-28-spu-codegen-design.md).
#   PORT_SPU_FLOAT_APPROX -- with PORT_SPU_FLOAT_RZ_ONLY: a zero result of an SPU
#                          float operation keeps the host's sign, -0 where the SPU
#                          has +0 (SPU_FLOAT_APPROX), which saves the two
#                          operations that make it +0. The extended-range checks
#                          stay: this title's EDGE jobs depend on them. Off, the
#                          lifted SPU code is bit-exact.
#   PORT_SPU_LS_DIAG    -- the local-store diagnostics (SPU_LS_WATCH,
#                          SPU_LS_LOWREAD, SPU_SMC_WATCH, SPU_WWS_PROBE) see the
#                          lifted SPU code's quadword loads and stores. Off
#                          (SPU_LS_NO_DIAG), those have no gate: a load and a
#                          branch less at every lqd/stqd.
option(PORT_SPU_AVX2 "Build the lifted SPU code with AVX2" ON)
option(PORT_SPU_LINES "Line tables for the lifted SPU code, and a PDB (profiling)" OFF)
option(PORT_SPU_FLOAT_RZ_ONLY "Lifted SPU code with the round-toward-zero float forms only" ON)
option(PORT_SPU_FLOAT_APPROX "Lifted SPU code: a zero float result may be -0 (SPU_FLOAT_APPROX)" OFF)
option(PORT_SPU_LS_DIAG "Local-store diagnostics in the lifted SPU code (a gate per lqd/stqd)" OFF)
# PORT_SPU_PGO: profile-guided optimization of the lifted SPU code (clang).
#   gen          -- instrument it; main.cpp writes the profile to LLVM_PROFILE_FILE every 15 s
#   <.profdata>  -- optimize it with that profile (llvm-profdata merge of the runs')
set(PORT_SPU_PGO "" CACHE STRING "Lifted SPU code: 'gen' to instrument, or a .profdata to use")
if(SPU_SOURCES)
    # SPU_INLINE_MFC_PARAMS: the MFC command parameters as stores, no call.
    set(spu_defs SPU_USE_DISPATCH_MT SPU_INLINE_MFC_PARAMS)
    if(PORT_SPU_JOBCAP)
        list(APPEND spu_defs PS3_SPU_JOBCAP)
    endif()
    if(NOT PORT_SPU_LS_DIAG)
        list(APPEND spu_defs SPU_LS_NO_DIAG)
    endif()
    if(PORT_SPU_AVX2 AND PORT_SPU_FLOAT_RZ_ONLY)
        list(APPEND spu_defs SPU_FLOAT_RZ_ONLY)
        if(PORT_SPU_FLOAT_APPROX)
            list(APPEND spu_defs SPU_FLOAT_APPROX)
        endif()
        # main.cpp then sets SPU_FLOAT_RZ=1 whatever the environment says.
        set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/main.cpp" PROPERTIES
            COMPILE_DEFINITIONS PORT_SPU_FLOAT_RZ_ONLY)
    endif()
    set_source_files_properties(${SPU_SOURCES} PROPERTIES
        INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu"
        COMPILE_DEFINITIONS "${spu_defs}")
    if(NOT MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(spu_opts -O2 -msse4.1)
        if(PORT_SPU_AVX2)
            list(APPEND spu_opts -mavx2 -mfma -ffp-contract=off)
        endif()
        if(PORT_SPU_LINES)
            list(APPEND spu_opts -gline-tables-only)
        endif()
        # No stack cookie: every lifted function with a large frame set and
        # checked one at each entry and exit, and a transfer between lifted
        # functions is an exit and an entry. The frames hold guest register
        # values, not host buffers.
        if(MSVC)
            list(APPEND spu_opts /GS-)
        else()
            list(APPEND spu_opts -fno-stack-protector)
        endif()
        if(PORT_SPU_PGO STREQUAL "gen")
            list(APPEND spu_opts -fprofile-instr-generate)
        elseif(PORT_SPU_PGO)
            list(APPEND spu_opts "-fprofile-instr-use=${PORT_SPU_PGO}" -Wno-profile-instr-unprofiled
                 -Wno-profile-instr-out-of-date -Wno-profile-instr-missing)
        endif()
        set_source_files_properties(${SPU_SOURCES} PROPERTIES COMPILE_OPTIONS "${spu_opts}")
    endif()
    if(PORT_SPU_LINES AND MSVC)
        target_link_options(${PROJECT_NAME} PRIVATE /DEBUG /OPT:REF /OPT:ICF)
    endif()
    if(PORT_SPU_PGO STREQUAL "gen")
        # The profile runtime of the clang that builds the port, next to its linker.
        get_filename_component(_llvm_bin "${CMAKE_LINKER}" DIRECTORY)
        file(GLOB _pgo_rt "${_llvm_bin}/../lib/clang/*/lib/windows/clang_rt.profile-x86_64.lib")
        if(NOT _pgo_rt)
            message(FATAL_ERROR "PORT_SPU_PGO=gen: no clang_rt.profile-x86_64.lib near ${CMAKE_LINKER}")
        endif()
        list(GET _pgo_rt 0 _pgo_rt)
        target_link_libraries(${PROJECT_NAME} PRIVATE "${_pgo_rt}")
        set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp" APPEND PROPERTY
                     COMPILE_DEFINITIONS PORT_SPU_PGO_GEN)
    endif()
endif()

# app.rc carries the icon, the version block and the manifest, and needs a
# resource compiler. Listing it as a source on a host that has none is not a
# link error, it is a "no rule to make" for a file nothing can consume.
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
    target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
    # The manifest comes from app.rc; the linker's own would be a second one.
    if(MSVC)
        target_link_options(${PROJECT_NAME} PRIVATE "/MANIFEST:NO")
    endif()
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    "${RECOMP_DIR}"                            # the generated ppu_recomp.h
    "${PS3RECOMP_DIR}/include"
    "${PS3RECOMP_DIR}/runtime/ppu"
    "${PS3RECOMP_DIR}/runtime/spu"
    "${PS3RECOMP_DIR}/runtime/memory"
    "${PS3RECOMP_DIR}/runtime/syscalls"
    "${PS3RECOMP_DIR}/runtime/platform"        # win32_compat.h and friends
    "${PS3RECOMP_DIR}/libs/video"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ps3recomp_runtime
)

# The runtime is C++, and on Apple also Objective-C. Letting CMake infer the
# link language from the generated C would leave the C++ standard library out.
set_target_properties(${PROJECT_NAME} PROPERTIES LINKER_LANGUAGE CXX)

# Windows system import libraries. dbghelp symbolizes crash-handler addresses
# against the PDB, user32 and gdi32 are what the backend's window and its GDI
# present are made of, and winmm is timeBeginPeriod. None of the four names
# anything a POSIX linker can find; the D3D12 backend's own imports come in
# through the #pragma comment(lib) in libs/video/rsx_d3d12_backend.c.
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        dbghelp
        user32
        gdi32
        winmm
    )
    # A linker map, so the PS3_SAMPLE profiler's raw RVAs in runtime code can be
    # named (tools: scripts/gowa.py symbolize). Costs nothing at run time.
    if(MSVC)
        target_link_options(${PROJECT_NAME} PRIVATE "/MAP:${CMAKE_BINARY_DIR}/${PROJECT_NAME}.map")
    endif()
endif()

# ---------------------------------------------------------------------------
# Compiler-specific flags
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fno-strict-aliasing
        # The lift spells PPC arithmetic as signed C arithmetic, which must wrap;
        # without this, clang exploits overflow UB (e.g. folds "x*x < 0" to false).
        -fwrapv
    )
elseif(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W3 /wd4100 /wd4244 /wd4267
        /Zc:__cplusplus         # report correct __cplusplus value
        # ppu_recomp.h defines PPU_FENCE, the lowering of the guest's
        # sync/lwsync/eieio/isync, and in C that is <stdatomic.h>. The lifter
        # writes C++ by default, but --single-file writes ppu_recomp.c, and a
        # port that took it would not compile without this. The runtime's own
        # build already carries the same flag for the same reason.
        /experimental:c11atomics
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
endif()

# The lifted translation units are machine-generated and can run to hundreds of
# thousands of lines each, so they get their own flags:
#
#   - warnings off. They are about generated code and nobody will act on them,
#     and at this volume they bury the diagnostics that matter.
#   - no floating-point contraction. arm64 fuses a multiply and an add by
#     default; a PPC fmadd is spelt as one where the lifter means one.
#   - /bigobj on MSVC. A single lifted file routinely passes the 65,536-section
#     limit of the default object format.
#   - FMA3 on x86-64 (PORT_PPU_FMA, on by default). PPC fmadd/fmsub are lifted
#     as fma(), and built for baseline x86-64 each one is a call into the CRT's
#     software fma: 176 of them in one lifted file. -mfma makes each a vfmadd,
#     with the same correctly rounded result. OFF for CPUs without FMA3.
option(PORT_PPU_FMA "Build the lifted PPU code with FMA3 instructions (x86-64)" ON)
set(recomp_fma OFF)
if(PORT_PPU_FMA AND CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
    set(recomp_fma ON)
endif()
if(MSVC)
    set(recomp_opts /w /bigobj)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        list(APPEND recomp_opts /clang:-ffp-contract=off)
        if(recomp_fma)
            list(APPEND recomp_opts /clang:-mfma)
        endif()
    endif()
else()
    set(recomp_opts -w -ffp-contract=off)
    if(recomp_fma)
        list(APPEND recomp_opts -mfma)
    endif()
endif()
set_source_files_properties(${RECOMP_SOURCES} ${LIBSRE_SOURCES} PROPERTIES
    COMPILE_OPTIONS "${recomp_opts}")

# PPC float arithmetic without the NaN checks (PORT_PPU_FAST_FP): the lifted game
# code takes the host's NaN, which differs from the PPC one only in its sign and
# in which of two NaN operands propagates. It made hot float code markedly
# shorter in the God of War HD port, whose frames looked the same either way.
# Off until this title boots and can be compared with it on.
option(PORT_PPU_FAST_FP "Lifted game code: host NaN results instead of the PPC ones" OFF)
if(PORT_PPU_FAST_FP)
    set_property(SOURCE ${RECOMP_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS PPU_FAST_FP)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
