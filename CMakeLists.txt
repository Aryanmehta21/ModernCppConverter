cmake_minimum_required(VERSION 3.21)

project(ModernCppConverter
    VERSION 0.1.0
    DESCRIPTION "Rule-based legacy C++ to modern C++ converter"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)

add_library(ModernCppConverterWarnings INTERFACE)
target_compile_options(ModernCppConverterWarnings
    INTERFACE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)

add_library(ModernCppConverterCore
    src/backend/BackendClient.cpp
    src/backend/BackendConfig.cpp
    src/app/ConversionCoordinator.cpp
    src/converter/AggressiveRewriteEngine.cpp
    src/converter/ClangAstStructureAnalyzer.cpp
    src/converter/CompilerDiagnosticCleanupPass.cpp
    src/converter/CompileVerifier.cpp
    src/converter/DependentUsageRewritePass.cpp
    src/converter/ImpactCascadingCleanupPass.cpp
    src/converter/IncludeManager.cpp
    src/converter/OfflineModernizationPipeline.cpp
    src/converter/OrphanedGrowthSymbolCleanupPass.cpp
    src/converter/OrphanedTempBufferLoopCleanupPass.cpp
    src/converter/SafeReplacementEngine.cpp
    src/converter/StructuralAnalyzers.cpp
    src/converter/StructuralModernizationEngine.cpp
    src/converter/TokenBasedStructureAnalyzer.cpp
    src/converter/TransformationContext.cpp
    src/converter/VectorAppendMethodRewritePass.cpp
    src/converter/VectorEmulationEliminationPass.cpp
    src/converter/VectorParadigmRewritePass.cpp
    src/converter/ValueTypePointerOperationScanner.cpp
    src/converter/VectorGrowthEmulationCleanupPass.cpp
    src/converter/ModernCppConverter.cpp
    src/converter/ConversionRule.cpp
    src/converter/RuleBasedConverterEngine.cpp
    src/converter/ModernCppExplanationGenerator.cpp
    src/converter/RawTextRepresentation.cpp
    src/converter/TokenRepresentation.cpp
    src/converter/AstRepresentation.cpp
    src/repository/RepositoryBackupService.cpp
    src/repository/RepositoryCloneService.cpp
    src/repository/RepositoryModernizationService.cpp
    src/repository/RepositoryReportWriter.cpp
    src/repository/RepositoryScanner.cpp
    src/repository/RepositoryVerificationService.cpp
)

target_include_directories(ModernCppConverterCore
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(ModernCppConverterCore PUBLIC cxx_std_20)
target_link_libraries(ModernCppConverterCore
    PUBLIC
        ModernCppConverterWarnings
        Qt6::Network
)

add_executable(ModernCppConverter
    src/main.cpp
    src/app/MainWindow.cpp
    src/app/MainWindow.h
)

target_link_libraries(ModernCppConverter
    PRIVATE
        ModernCppConverterCore
        Qt6::Widgets
)

if(WIN32)
    set_target_properties(ModernCppConverter PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

enable_testing()
add_subdirectory(tests)
