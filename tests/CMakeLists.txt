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
