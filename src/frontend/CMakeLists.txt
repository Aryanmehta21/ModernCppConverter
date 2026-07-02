target_sources(ModernCppConverterCore
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/frontend/FrontendFactory.cpp
        ${PROJECT_SOURCE_DIR}/src/frontend/LightweightFrontend.cpp
)

if(ENABLE_CLANG_EXPERIMENTS)
    target_sources(ModernCppConverterCore
        PRIVATE
            ${PROJECT_SOURCE_DIR}/src/frontend/ClangExperimentalFrontend.cpp
            ${PROJECT_SOURCE_DIR}/src/frontend/ClangParseService.cpp
    )
    target_compile_definitions(ModernCppConverterCore PRIVATE MODERNCPP_ENABLE_CLANG_EXPERIMENTS=1)
    target_include_directories(ModernCppConverterCore SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
    target_compile_definitions(ModernCppConverterCore PRIVATE ${LLVM_DEFINITIONS})
    target_link_libraries(ModernCppConverterCore
        PRIVATE
            clangTooling
            clangFrontend
            clangAST
            clangBasic
            clangLex
    )

    add_executable(ModernCppClangParseHelper
        ${PROJECT_SOURCE_DIR}/src/frontend/ClangParseHelperMain.cpp
    )
    modern_cpp_converter_use_root_output(ModernCppClangParseHelper)
    target_link_libraries(ModernCppClangParseHelper
        PRIVATE
            ModernCppConverterCore
            ModernCppConverterWarnings
            Qt6::Core
    )
endif()
