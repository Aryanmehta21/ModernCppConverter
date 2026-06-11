add_executable(ConverterTests
    ConverterTests.cpp
)

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
add_test(NAME RepositoryModeTests COMMAND Phase4EnterpriseTests repository)
