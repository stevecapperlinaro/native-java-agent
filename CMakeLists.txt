cmake_minimum_required (VERSION 2.6)
project (native-java-agent)

set(OUTDIR ${PROJECT_BINARY_DIR}/out)
set(LIBRARY_OUTPUT_PATH ${OUTDIR})

find_package(JNI)
INCLUDE_DIRECTORIES(${JAVA_INCLUDE_PATH})
INCLUDE_DIRECTORIES(${JAVA_INCLUDE_PATH2})

add_library(nativejava SHARED nativejava.c methodentry.c methodentry.h)
