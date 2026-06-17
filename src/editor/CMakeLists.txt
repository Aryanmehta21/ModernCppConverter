add_library(ModernCppConverterEditor
    ${PROJECT_SOURCE_DIR}/src/editor/CppCodeEditor.cpp
    ${PROJECT_SOURCE_DIR}/src/editor/CppCodeEditor.h
    ${PROJECT_SOURCE_DIR}/src/editor/CppSyntaxHighlighter.cpp
    ${PROJECT_SOURCE_DIR}/src/editor/CppSyntaxHighlighter.h
    ${PROJECT_SOURCE_DIR}/src/editor/LineNumberArea.cpp
    ${PROJECT_SOURCE_DIR}/src/editor/LineNumberArea.h
)
modern_cpp_converter_use_root_output(ModernCppConverterEditor)

target_include_directories(ModernCppConverterEditor
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src
)

target_compile_features(ModernCppConverterEditor PUBLIC cxx_std_20)
target_link_libraries(ModernCppConverterEditor
    PUBLIC
        ModernCppConverterWarnings
        Qt6::Widgets
)
