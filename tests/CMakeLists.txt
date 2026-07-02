add_executable(ConverterTests
    ConverterTests.cpp
)
modern_cpp_converter_use_root_output(ConverterTests)

target_link_libraries(ConverterTests
    PRIVATE
        ModernCppConverterCore
        ModernCppConverterWarnings
        Qt6::Network
)

add_test(NAME ConverterTests COMMAND ConverterTests)

add_executable(Phase4EnterpriseTests
    Phase4EnterpriseTests.cpp
)
modern_cpp_converter_use_root_output(Phase4EnterpriseTests)

target_link_libraries(Phase4EnterpriseTests
    PRIVATE
        ModernCppConverterCore
        ModernCppConverterWarnings
        Qt6::Network
)

add_test(NAME OwnershipTests COMMAND Phase4EnterpriseTests ownership)
add_test(NAME PolymorphismTests COMMAND Phase4EnterpriseTests polymorphism)
add_test(NAME RuleOfZeroTests COMMAND Phase4EnterpriseTests rule-zero)
add_test(NAME AlgorithmModernizationTests COMMAND Phase4EnterpriseTests algorithm)
add_test(NAME IteratorModernizationTests COMMAND Phase4EnterpriseTests iterator)
add_test(NAME FileIoModernizationTests COMMAND Phase4EnterpriseTests file-io)
add_test(NAME SemanticConsistencyTests COMMAND Phase4EnterpriseTests semantic)
add_test(NAME ScopedEnumOutputTests COMMAND Phase4EnterpriseTests scoped-enum)
add_test(NAME FinalPolishTests COMMAND Phase4EnterpriseTests polish)
add_test(NAME RegressionModernizationTests COMMAND Phase4EnterpriseTests regression)
add_test(NAME QualitySprintTests COMMAND Phase4EnterpriseTests quality)
add_test(NAME CrossScopeTypePropagationTests COMMAND Phase4EnterpriseTests cross-scope)
add_test(NAME NsdmiScopeSafetyTests COMMAND Phase4EnterpriseTests nsdmi-scope)
add_test(NAME FunctionalModernizationTests COMMAND Phase4EnterpriseTests functional)
add_test(NAME FunctorToLambdaSpecificTests COMMAND Phase4EnterpriseTests functor-specific)
add_test(NAME RepositoryModeTests COMMAND Phase4EnterpriseTests repository)

add_executable(V1RegressionTests
    V1RegressionTests.cpp
)
modern_cpp_converter_use_root_output(V1RegressionTests)

target_link_libraries(V1RegressionTests
    PRIVATE
        ModernCppConverterCore
        ModernCppConverterWarnings
        Qt6::Network
)

add_test(NAME V1RegressionTests COMMAND V1RegressionTests)

add_executable(ModernCppConverterSmokeTests
    ModernCppConverterSmokeTests.cpp
)
modern_cpp_converter_use_root_output(ModernCppConverterSmokeTests)

target_link_libraries(ModernCppConverterSmokeTests
    PRIVATE
        ModernCppConverterCore
        ModernCppConverterWarnings
        Qt6::Network
)
target_compile_definitions(ModernCppConverterSmokeTests
    PRIVATE
        MODERNCPP_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

add_test(NAME ModernCppConverterSmokeTests COMMAND ModernCppConverterSmokeTests)

add_executable(GuiConversionSmokeTests
    GuiConversionSmokeTests.cpp
)
modern_cpp_converter_use_root_output(GuiConversionSmokeTests)

target_link_libraries(GuiConversionSmokeTests
    PRIVATE
        ModernCppConverterUi
        ModernCppConverterWarnings
        Qt6::Concurrent
        Qt6::Widgets
)

add_test(NAME GuiConversionSmokeTests COMMAND GuiConversionSmokeTests)
set_tests_properties(GuiConversionSmokeTests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(ParserLayerTests
    ParserLayerTests.cpp
)
modern_cpp_converter_use_root_output(ParserLayerTests)

target_link_libraries(ParserLayerTests
    PRIVATE
        ModernCppConverterCore
        ModernCppConverterWarnings
)

add_test(NAME ParserLayerTests COMMAND ParserLayerTests)
