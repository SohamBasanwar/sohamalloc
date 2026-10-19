# Builds a subset of mimalloc-bench (https://github.com/daanx/mimalloc-bench)
# with the same compile flags as its bench/CMakeLists.txt, but each target is
# guarded so that a missing source (e.g. shbench, which is not redistributable)
# skips that benchmark instead of failing the whole configure step.
#
# Usage (done by bench/build_bench.sh):
#   cmake -S bench/cmake -B bench/build/mb -DMB_SRC=<copy of mimalloc-bench/bench>
cmake_minimum_required(VERSION 3.5)
project(sohamalloc-bench-subset C CXX)
set(CMAKE_CXX_STANDARD 17)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Release")
endif()
if(NOT MB_SRC)
  message(FATAL_ERROR "pass -DMB_SRC=<mimalloc-bench/bench directory>")
endif()

# Same warning suppression as upstream (these are old K&R-era programs).
set(FLAGS " -w -Wno-implicit-function-declaration -Wno-implicit-int -Wno-int-conversion -Wno-incompatible-pointer-types")
if(CMAKE_C_COMPILER_ID MATCHES "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_GREATER_EQUAL 14)
  string(APPEND FLAGS " -Wno-return-mismatch")
endif()
string(APPEND CMAKE_C_FLAGS ${FLAGS})
string(APPEND CMAKE_CXX_FLAGS ${FLAGS})

find_package(Threads REQUIRED)

function(mb_prepend var prefix)
  set(l "")
  foreach(f ${ARGN})
    list(APPEND l "${prefix}/${f}")
  endforeach()
  set(${var} "${l}" PARENT_SCOPE)
endfunction()

# mb_exe(<name> SOURCES ... [DEFS ...] [OPTS ...] [INCS ...] [LIBS ...])
function(mb_exe name)
  cmake_parse_arguments(A "" "" "SOURCES;DEFS;OPTS;INCS;LIBS" ${ARGN})
  foreach(s ${A_SOURCES})
    if(NOT EXISTS "${s}")
      message(WARNING "skip ${name}: missing ${s}")
      return()
    endif()
  endforeach()
  add_executable(${name} ${A_SOURCES})
  if(A_DEFS)
    target_compile_definitions(${name} PRIVATE ${A_DEFS})
  endif()
  if(A_OPTS)
    target_compile_options(${name} PRIVATE ${A_OPTS})
  endif()
  if(A_INCS)
    target_include_directories(${name} PRIVATE ${A_INCS})
  endif()
  target_link_libraries(${name} PRIVATE Threads::Threads m ${A_LIBS})
endfunction()

mb_prepend(cfrac_sources ${MB_SRC}/cfrac
    cfrac.c pops.c pconst.c pio.c pabs.c pneg.c pcmp.c podd.c phalf.c
    padd.c psub.c pmul.c pdivmod.c psqrt.c ppowmod.c atop.c ptoa.c itop.c
    utop.c ptou.c errorp.c pfloat.c pidiv.c pimod.c picmp.c primes.c
    pcfrac.c pgcd.c)
mb_exe(cfrac SOURCES ${cfrac_sources}
       OPTS $<$<C_COMPILER_ID:GNU>:-std=gnu89> DEFS NOMEMOPT=1)

mb_prepend(espresso_sources ${MB_SRC}/espresso
    cofactor.c cols.c compl.c contain.c cubestr.c cvrin.c cvrm.c cvrmisc.c
    cvrout.c dominate.c equiv.c espresso.c essen.c exact.c expand.c gasp.c
    getopt.c gimpel.c globals.c hack.c indep.c irred.c main.c map.c matrix.c
    mincov.c opo.c pair.c part.c primes.c reduce.c rows.c set.c setc.c
    sharp.c sminterf.c solution.c sparse.c unate.c utility.c verify.c)
mb_exe(espresso SOURCES ${espresso_sources} OPTS $<$<C_COMPILER_ID:GNU>:-std=gnu89>)

mb_prepend(barnes_sources ${MB_SRC}/barnes
    code.c code_io.c load.c grav.c getparam.c util.c)
mb_exe(barnes SOURCES ${barnes_sources} OPTS $<$<C_COMPILER_ID:GNU>:-std=gnu89>)

mb_exe(larson SOURCES ${MB_SRC}/larson/larson.cpp
       OPTS -Wno-unused-result DEFS CPP=1)
mb_exe(larson-sized SOURCES ${MB_SRC}/larson/larson.cpp
       OPTS -Wno-unused-result -fsized-deallocation DEFS CPP=1 SIZED=1)

mb_exe(alloc-test SOURCES ${MB_SRC}/alloc-test/test_common.cpp
                          ${MB_SRC}/alloc-test/allocator_tester.cpp
       DEFS BENCH=4)

mb_exe(sh6bench SOURCES ${MB_SRC}/shbench/sh6bench-new.c
       DEFS BENCH=1 SYS_MULTI_THREAD=1)
mb_exe(sh8bench SOURCES ${MB_SRC}/shbench/sh8bench-new.c
       DEFS BENCH=1 SYS_MULTI_THREAD=1)

mb_exe(cache-scratch SOURCES ${MB_SRC}/cache-scratch/cache-scratch.cpp)
mb_exe(cache-thrash  SOURCES ${MB_SRC}/cache-thrash/cache-thrash.cpp)
mb_exe(xmalloc-test  SOURCES ${MB_SRC}/xmalloc-test/xmalloc-test.c)
mb_exe(malloc-large  SOURCES ${MB_SRC}/malloc-large/malloc-large.cpp)
mb_exe(mstress       SOURCES ${MB_SRC}/mstress/mstress.c)
mb_exe(mleak         SOURCES ${MB_SRC}/mleak/mleak.c)
mb_exe(rptest SOURCES ${MB_SRC}/rptest/rptest.c ${MB_SRC}/rptest/thread.c
                      ${MB_SRC}/rptest/timer.c
       INCS ${MB_SRC}/rptest)
mb_exe(glibc-simple SOURCES ${MB_SRC}/glibc-bench/bench-malloc-simple.c)
mb_exe(glibc-thread SOURCES ${MB_SRC}/glibc-bench/bench-malloc-thread.c)
