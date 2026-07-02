add_library(ModernCppConverterCore)
modern_cpp_converter_use_root_output(ModernCppConverterCore)

target_include_directories(ModernCppConverterCore
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(ModernCppConverterCore PUBLIC cxx_std_20)
target_compile_definitions(ModernCppConverterCore
    PUBLIC
        MODERNCPP_APP_VERSION="${MODERNCPP_APP_VERSION}"
        MODERNCPP_RELEASE_CHANNEL="${MODERNCPP_RELEASE_CHANNEL}"
        MODERNCPP_BUILD_TYPE="$<CONFIG>"
        MODERNCPP_GIT_COMMIT="${MODERNCPP_GIT_COMMIT}"
        MODERNCPP_BUILD_DATE="${MODERNCPP_BUILD_DATE}"
        MODERNCPP_CLANG_ENABLED=${MODERNCPP_CLANG_ENABLED_VALUE}
)
target_link_libraries(ModernCppConverterCore
    PUBLIC
        ModernCppConverterWarnings
        Qt6::Network
)

include(${CMAKE_CURRENT_LIST_DIR}/services/CMakeLists.txt)
include(${CMAKE_CURRENT_LIST_DIR}/parser/CMakeLists.txt)
include(${CMAKE_CURRENT_LIST_DIR}/frontend/CMakeLists.txt)
include(${CMAKE_CURRENT_LIST_DIR}/utils/CMakeLists.txt)
include(${CMAKE_CURRENT_LIST_DIR}/modernization/CMakeLists.txt)
include(${CMAKE_CURRENT_LIST_DIR}/pipeline/CMakeLists.txt)
include(${CMAKE_CURRENT_LIST_DIR}/editor/CMakeLists.txt)
include(${CMAKE_CURRENT_LIST_DIR}/ui/CMakeLists.txt)
