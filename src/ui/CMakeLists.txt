add_library(ModernCppConverterUi
    ${PROJECT_SOURCE_DIR}/src/app/MainWindow.cpp
    ${PROJECT_SOURCE_DIR}/src/app/MainWindow.h
)
modern_cpp_converter_use_root_output(ModernCppConverterUi)

target_include_directories(ModernCppConverterUi
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src
)

target_compile_features(ModernCppConverterUi PUBLIC cxx_std_20)
target_link_libraries(ModernCppConverterUi
    PUBLIC
        ModernCppConverterCore
        ModernCppConverterEditor
        Qt6::Concurrent
        Qt6::Widgets
)

add_executable(ModernCppConverter
    ${PROJECT_SOURCE_DIR}/src/main.cpp
)
modern_cpp_converter_use_root_output(ModernCppConverter)

target_link_libraries(ModernCppConverter
    PRIVATE
        ModernCppConverterUi
)

if(WIN32)
    set_target_properties(ModernCppConverter PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
