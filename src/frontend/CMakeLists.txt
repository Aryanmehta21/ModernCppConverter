target_sources(ModernCppConverterCore
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/frontend/FrontendFactory.cpp
        ${PROJECT_SOURCE_DIR}/src/frontend/LightweightFrontend.cpp
)

if(ENABLE_CLANG_EXPERIMENTS)
    target_sources(ModernCppConverterCore
        PRIVATE
            ${PROJECT_SOURCE_DIR}/src/frontend/ClangExperimentalFrontend.cpp
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
endif()
