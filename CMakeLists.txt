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

find_package(Qt6 REQUIRED COMPONENTS Widgets Network Concurrent)

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
    src/converter/AlgorithmPolishPass.cpp
    src/converter/AlgorithmModernizationPass.cpp
    src/converter/AutoPtrRemovalPass.cpp
    src/converter/ClassResourceAnalyzerPass.cpp
    src/converter/ClassContextResolver.cpp
    src/converter/ClassStringBufferModernizationPass.cpp
    src/converter/ClangAstStructureAnalyzer.cpp
    src/converter/CompilerDiagnosticCleanupPass.cpp
    src/converter/CompileVerifier.cpp
    src/converter/ConcurrencyRaiiModernizationPass.cpp
    src/converter/ContainerModernizationCleanupPass.cpp
    src/converter/ContainerPolishPass.cpp
    src/converter/CrossFunctionTypePropagationPass.cpp
    src/converter/CrossScopeTypePropagationPass.cpp
    src/converter/DependentUsageRewritePass.cpp
    src/converter/DestructorDeclarationValidationPass.cpp
    src/converter/EnumToStringCandidatePass.cpp
    src/converter/FilePointerModernizationPass.cpp
    src/converter/FileIoModernizationPass.cpp
    src/converter/FunctionPointerModernizationPass.cpp
    src/converter/AutoDeductionPass.cpp
    src/converter/FunctionalModernizationPass.cpp
    src/converter/FunctionalModernizationValidator.cpp
    src/converter/FunctorToLambdaPass.cpp
    src/converter/FunctorToLambdaValidationPass.cpp
    src/converter/ImpactCascadingCleanupPass.cpp
    src/converter/IndexLoopModernizationPass.cpp
    src/converter/IteratorModernizationPass.cpp
    src/converter/IncludeManager.cpp
    src/converter/MemberApiCascadePass.cpp
    src/converter/MallocFreeModernizationPass.cpp
    src/converter/ModernizationPolishPass.cpp
    src/converter/ModernizationPolishValidator.cpp
    src/converter/OfflineModernizationPipeline.cpp
    src/converter/OwnershipGraphAnalyzer.cpp
    src/converter/OwnershipGraphModernizationPass.cpp
    src/converter/OwnershipSanityScanner.cpp
    src/converter/OverrideEnforcementPass.cpp
    src/converter/NsdmiScopeSafetyPass.cpp
    src/converter/PassByValueToConstRefPass.cpp
    src/converter/PolymorphicContractPass.cpp
    src/converter/PolymorphicContractPolishPass.cpp
    src/converter/PolymorphicSafetyPass.cpp
    src/converter/PrintfModernizationPass.cpp
    src/converter/QualityModernizationPass.cpp
    src/converter/RangeForModernizationPass.cpp
    src/converter/ReturnTypePropagationPass.cpp
    src/converter/OrphanedGrowthSymbolCleanupPass.cpp
    src/converter/OrphanedTempBufferLoopCleanupPass.cpp
    src/converter/SafeReplacementEngine.cpp
    src/converter/ScopeAwareSymbolTable.cpp
    src/converter/ScopeLeakValidationPass.cpp
    src/converter/ScopedEnumCastValidationPass.cpp
    src/converter/ScopedEnumOutputPropagationPass.cpp
    src/converter/ScopedEnumOutputValidator.cpp
    src/converter/SemanticConsistencyValidator.cpp
    src/converter/SemanticModernizationValidator.cpp
    src/converter/SemanticTypeValidationPass.cpp
    src/converter/SmartPointerCollectionPropagationPass.cpp
    src/converter/SmartPointerSinkPropagationPass.cpp
    src/converter/SmartPointerTypePropagationPass.cpp
    src/converter/StructuralAnalyzers.cpp
    src/converter/StructuralModernizationEngine.cpp
    src/converter/StringViewPolishPass.cpp
    src/converter/StructuredBindingPass.cpp
    src/converter/TokenBasedStructureAnalyzer.cpp
    src/converter/TransformationContext.cpp
    src/converter/RuleOfZeroPass.cpp
    src/converter/RuleOfZeroPolishPass.cpp
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
    src/editor/CppCodeEditor.cpp
    src/editor/CppCodeEditor.h
    src/editor/CppSyntaxHighlighter.cpp
    src/editor/CppSyntaxHighlighter.h
    src/editor/LineNumberArea.cpp
    src/editor/LineNumberArea.h
)

target_link_libraries(ModernCppConverter
    PRIVATE
        ModernCppConverterCore
        Qt6::Concurrent
        Qt6::Widgets
)

if(WIN32)
    set_target_properties(ModernCppConverter PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

enable_testing()
add_subdirectory(tests)
