set(astra-sim-ns3-libs-list
  "libapplications"
  "libcore"
  "libcsma"
  "libinternet"
  "libpoint-to-point"
  "libnetwork"
)
list(JOIN astra-sim-ns3-libs-list " " astra-sim-ns3-libs)
set(astra-sim-dir "${PROJECT_SOURCE_DIR}/../../../")
include_directories("../scratch")

# The raw transport-event stream in common.h is zstd-compressed at runtime.
find_library(ZSTD_LIBRARY zstd REQUIRED)
find_path(ZSTD_INCLUDE_DIR zstd.h REQUIRED)
include_directories(${ZSTD_INCLUDE_DIR})
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../../ AstraSim)

build_exec(
  EXECNAME AstraSimNetwork
  SOURCE_FILES ${astra-sim-dir}/astra-sim/network_frontend/ns3/AstraSimNetwork.cc
  DIRECTORY_PATHS ${astra-sim-dir}/extern/helper
  LIBRARIES_TO_LINK "${astra-sim-ns3-libs-list}" "AstraSim" "${ZSTD_LIBRARY}"
  EXECUTABLE_DIRECTORY_PATH "${PROJECT_SOURCE_DIR}/build/scratch/"
)
