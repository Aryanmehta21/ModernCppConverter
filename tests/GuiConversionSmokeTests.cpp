#include "app/MainWindow.h"
#include "converter/IConverterEngine.h"
#include "converter/RuleBasedConverterEngine.h"
#include "editor/CppCodeEditor.h"
#include "frontend/FrontendFactory.h"
#include "utils/CrashBreadcrumb.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QWidget>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool contains(const QString& text, const QString& needle)
{
    return text.contains(needle);
}

int countOccurrences(const QString& text, const QString& needle)
{
    int count = 0;
    int offset = 0;
    while ((offset = text.indexOf(needle, offset)) >= 0) {
        ++count;
        offset += needle.size();
    }
    return count;
}

QString canBufferReproInput()
{
    return QStringLiteral(
        "#include <iostream>\n"
        "#include <cstring>\n\n"
        "#define INITIAL_MAX 2\n\n"
        "typedef struct _CanFrame {\n"
        "    unsigned long arbitrationId;\n"
        "    char dataPayload[8];\n"
        "} CanFrame;\n\n"
        "class CanBufferManager {\n"
        "private:\n"
        "    CanFrame* backingStore;\n"
        "    int count;\n"
        "    int capacity;\n\n"
        "public:\n"
        "    CanBufferManager() {\n"
        "        count = 0;\n"
        "        capacity = INITIAL_MAX;\n"
        "        backingStore = new CanFrame[capacity];\n"
        "    }\n\n"
        "    CanBufferManager(const CanBufferManager& other) {\n"
        "        count = other.count;\n"
        "        capacity = other.capacity;\n"
        "        backingStore = new CanFrame[capacity];\n"
        "        for (int i = 0; i < count; ++i) {\n"
        "            backingStore[i] = other.backingStore[i];\n"
        "        }\n"
        "    }\n\n"
        "    ~CanBufferManager() {\n"
        "        if (backingStore != NULL) {\n"
        "            delete[] backingStore;\n"
        "            backingStore = NULL;\n"
        "        }\n"
        "    }\n\n"
        "    bool appendFrame(unsigned long id, const char* bytes) {\n"
        "        if (count >= capacity) {\n"
        "            int newCap = capacity * 2;\n"
        "            CanFrame* newStore = new CanFrame[newCap];\n"
        "            for (int i = 0; i < count; ++i) {\n"
        "                newStore[i] = backingStore[i];\n"
        "            }\n"
        "            delete[] backingStore;\n"
        "            backingStore = newStore;\n"
        "            capacity = newCap;\n"
        "        }\n\n"
        "        backingStore[count].arbitrationId = id;\n"
        "        std::strncpy(backingStore[count].dataPayload, bytes, 7);\n"
        "        backingStore[count].dataPayload[7] = '\\0';\n\n"
        "        count++;\n"
        "        return true;\n"
        "    }\n\n"
        "    int getFrameCount() const { return count; }\n"
        "    CanFrame* getFrame(int index) { return &backingStore[index]; }\n"
        "};\n\n"
        "int main() {\n"
        "    CanBufferManager buffer;\n"
        "    buffer.appendFrame(0x7DF, \"\\x02\\x01\\x0D\\x00\\x00\\x00\\x00\");\n"
        "    buffer.appendFrame(0x7E8, \"\\x03\\x41\\x0D\\x32\\x00\\x00\\x00\");\n\n"
        "    std::cout << \"Buffered Frames: \" << buffer.getFrameCount() << std::endl;\n"
        "    return 0;\n"
        "}\n");
}

template <typename Widget>
Widget* findByText(QWidget& root, const QString& text)
{
    const QList<Widget*> widgets = root.findChildren<Widget*>();
    for (Widget* widget : widgets) {
        if (widget->text() == text) {
            return widget;
        }
    }
    return nullptr;
}

QComboBox* findComboWithItem(QWidget& root, const QString& item)
{
    const QList<QComboBox*> combos = root.findChildren<QComboBox*>();
    for (QComboBox* combo : combos) {
        if (combo->findText(item) >= 0) {
            return combo;
        }
    }
    return nullptr;
}

std::pair<CppCodeEditor*, CppCodeEditor*> findCodeEditors(QWidget& root)
{
    CppCodeEditor* inputEditor = nullptr;
    CppCodeEditor* outputEditor = nullptr;
    const QList<QPlainTextEdit*> plainEditors = root.findChildren<QPlainTextEdit*>();
    for (QPlainTextEdit* editor : plainEditors) {
        if (auto* codeEditor = dynamic_cast<CppCodeEditor*>(editor)) {
            if (codeEditor->isReadOnly()) {
                outputEditor = codeEditor;
            } else {
                inputEditor = codeEditor;
            }
        }
    }
    return {inputEditor, outputEditor};
}

QString collectPlainText(QWidget& root)
{
    QString text;
    const QList<QPlainTextEdit*> textEditors = root.findChildren<QPlainTextEdit*>();
    for (const QPlainTextEdit* editor : textEditors) {
        text += editor->toPlainText();
        text += '\n';
    }
    return text;
}

bool pumpUntil(QApplication& app, int timeoutMs, const std::function<bool()>& condition)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        app.processEvents();
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    app.processEvents();
    return condition();
}

void pumpFor(QApplication& app, int durationMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < durationMs) {
        app.processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

class SlowFirstConverterEngine final : public IConverterEngine
{
public:
    explicit SlowFirstConverterEngine(int firstCallDelayMs)
        : firstCallDelayMs_(firstCallDelayMs)
    {
    }

    ConversionResult convert(const std::string& legacyCode) const override
    {
        return convert(legacyCode, ModernizationOptions{});
    }

    ConversionResult convert(const std::string& legacyCode, const ModernizationOptions&) const override
    {
        const int call = ++callCount_;
        if (call == 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(firstCallDelayMs_));
        }

        ConversionResult result;
        result.modernCode = "converted-call-" + std::to_string(call) + "\n" + legacyCode;
        result.conversionSource = "GUI worker lifecycle test";
        result.backendStatus = "Not used";
        result.explanation = "Deterministic test converter.";
        result.diagnosticMessages.push_back("slow-engine-call=" + std::to_string(call));
        return result;
    }

private:
    int firstCallDelayMs_ = 0;
    mutable std::atomic<int> callCount_{0};
};

class DebugDiagnosticConverterEngine final : public IConverterEngine
{
public:
    ConversionResult convert(const std::string& legacyCode) const override
    {
        return convert(legacyCode, ModernizationOptions{});
    }

    ConversionResult convert(const std::string& legacyCode, const ModernizationOptions&) const override
    {
        ConversionResult result;
        result.modernCode = legacyCode;
        result.conversionSource = "Debug diagnostic test";
        result.backendStatus = "Not used";
        result.diagnosticVerbosity = DiagnosticVerbosity::Debug;
        result.diagnosticMessages.push_back("FRONTEND requested=Lightweight selected=LightweightFrontend clang_enabled=false clang_available=false fallback=false reason=\"\" parse=success classes=0 functions=1 enums=0 variables=0");
        result.diagnosticMessages.push_back("Functional diagnostics: FunctorToLambdaPass candidates found: 0, candidates converted: 0, candidates skipped: 0");
        result.diagnosticMessages.push_back("START PASS VectorParadigmRewritePass iteration=1 hash_before=1 visited=4");
        result.diagnosticMessages.push_back("END PASS VectorParadigmRewritePass iteration=1 hash_before=1 hash_after=2 visited=4 modified=1 rewrites=1 time_ms=0 status=changed");
        result.diagnosticMessages.push_back("PASS SUMMARY pass=\"VectorParadigmRewritePass\" applied=1 skipped=0 warnings=0 rollbacks=0 visited=4 modified=1 rewrites=1 time_ms=0 status=changed");
        result.diagnosticMessages.push_back("COMPILE VERIFICATION status=passed stage=initial compiler=test time_ms=0");
        result.diagnosticMessages.push_back("FINAL RESULT status=success applied=1 suggestions=0 skipped=0 compile=passed");
        result.compileVerificationEnabled = true;
        result.compileVerificationPassed = true;
        result.compilerUsed = "test";
        result.changes.push_back(ConversionChange{
            "Functional diagnostics: FunctorToLambdaPass",
            "pass started",
            {},
            "candidates found: 0, candidates converted: 0, candidates skipped: 0",
            false,
            true,
        });
        return result;
    }
};

class StaticDiagnosticConverterEngine final : public IConverterEngine
{
public:
    explicit StaticDiagnosticConverterEngine(ConversionResult result)
        : result_(std::move(result))
    {
    }

    ConversionResult convert(const std::string& legacyCode) const override
    {
        return convert(legacyCode, ModernizationOptions{});
    }

    ConversionResult convert(const std::string& legacyCode, const ModernizationOptions&) const override
    {
        ConversionResult result = result_;
        if (result.modernCode.empty()) {
            result.modernCode = legacyCode;
        }
        return result;
    }

private:
    ConversionResult result_;
};

void configureOfflineMode(MainWindow& window);

ConversionResult summaryDiagnosticResult(std::vector<std::string> diagnostics,
                                         std::vector<ConversionChange> changes = {})
{
    ConversionResult result;
    result.modernCode = "int main() { return 0; }\n";
    result.conversionSource = "Summary diagnostic test";
    result.backendStatus = "Not used";
    result.diagnosticVerbosity = DiagnosticVerbosity::Summary;
    result.diagnosticMessages = std::move(diagnostics);
    result.changes = std::move(changes);
    result.compileVerificationEnabled = true;
    result.compileVerificationPassed = true;
    result.compilerUsed = "test";
    return result;
}

QString runStaticDiagnosticConversion(QApplication& app, ConversionResult result)
{
    MainWindow window(std::make_unique<StaticDiagnosticConverterEngine>(std::move(result)));
    configureOfflineMode(window);

    auto [inputEditor, outputEditor] = findCodeEditors(window);
    require(inputEditor != nullptr, "static diagnostic test should find input editor");
    require(outputEditor != nullptr, "static diagnostic test should find output editor");

    QPushButton* convertButton = findByText<QPushButton>(window, "Convert");
    require(convertButton != nullptr, "static diagnostic test should find Convert button");

    inputEditor->setPlainText("int main() { return 0; }\n");
    convertButton->click();
    require(pumpUntil(app, 2000, [&]() {
        return convertButton->isEnabled() && !outputEditor->toPlainText().isEmpty();
    }), "static diagnostic conversion should finish");

    return collectPlainText(window);
}

void configureOfflineMode(MainWindow& window)
{
    QComboBox* modeCombo = findComboWithItem(window, "Offline Rule-Based");
    require(modeCombo != nullptr, "GUI smoke test should find conversion mode combo");
    modeCombo->setCurrentIndex(modeCombo->findText("Offline Rule-Based"));
}

void testBalancedThenAggressiveClangDebugConversionDoesNotCrash(QApplication& app)
{
    MainWindow window(std::make_unique<RuleBasedConverterEngine>());
    configureOfflineMode(window);

    auto [inputEditor, outputEditor] = findCodeEditors(window);
    require(inputEditor != nullptr, "aggressive Clang crash regression should find input editor");
    require(outputEditor != nullptr, "aggressive Clang crash regression should find output editor");

    QPushButton* convertButton = findByText<QPushButton>(window, "Convert");
    require(convertButton != nullptr, "aggressive Clang crash regression should find Convert button");

    inputEditor->setPlainText("typedef int Count;\nint main() { Count value = 0; return value; }\n");
    convertButton->click();
    require(pumpUntil(app, 35000, [&]() {
        return convertButton->isEnabled() && !outputEditor->toPlainText().isEmpty();
    }), "balanced conversion should complete before aggressive Clang conversion");
    require(outputEditor->toPlainText().contains("using Count = int;"),
            "balanced conversion should produce output before the second conversion");

    QComboBox* levelCombo = findComboWithItem(window, "AI-Style Aggressive Rewrite");
    QComboBox* rewriteCombo = findComboWithItem(window, "Aggressive AI-like Rewrite");
    QComboBox* frontendCombo = findComboWithItem(window, "Clang Experimental");
    QComboBox* diagnosticsCombo = findComboWithItem(window, "Debug");
    QCheckBox* compileCheckBox = findByText<QCheckBox>(window, "Automatically verify converted code");
    require(levelCombo != nullptr, "aggressive Clang crash regression should find modernization level combo");
    require(rewriteCombo != nullptr, "aggressive Clang crash regression should find rewrite style combo");
    require(frontendCombo != nullptr, "aggressive Clang crash regression should find frontend combo");
    require(diagnosticsCombo != nullptr, "aggressive Clang crash regression should find diagnostics combo");
    require(compileCheckBox != nullptr, "aggressive Clang crash regression should find compile checkbox");

    levelCombo->setCurrentIndex(levelCombo->findText("AI-Style Aggressive Rewrite"));
    rewriteCombo->setCurrentIndex(rewriteCombo->findText("Aggressive AI-like Rewrite"));
    frontendCombo->setCurrentIndex(frontendCombo->findText("Clang Experimental"));
    diagnosticsCombo->setCurrentIndex(diagnosticsCombo->findText("Debug"));
    compileCheckBox->setChecked(true);

    inputEditor->setPlainText(
        "#include <string>\n"
        "#define MAKE_ALIAS(type, name) typedef type name;\n"
        "MAKE_ALIAS(int, MacroCount)\n"
        "namespace telemetry {\n"
        "    typedef std::string RecordName;\n"
        "    class Record {\n"
        "    public:\n"
        "        typedef int Identifier;\n"
        "        Identifier id{};\n"
        "        RecordName name{};\n"
        "    };\n"
        "}\n"
        "int main()\n"
        "{\n"
        "    telemetry::Record record{};\n"
        "    return record.id;\n"
        "}\n");
    outputEditor->clear();
    convertButton->click();

    require(pumpUntil(app, 60000, [&]() {
        return convertButton->isEnabled() && !outputEditor->toPlainText().isEmpty();
    }), "AI aggressive + ClangExperimental + Debug details conversion should return a result or clean fallback");

    const QString allPlainText = collectPlainText(window);
    require(contains(allPlainText, "Frontend Summary"),
            "aggressive Clang Debug conversion should render Conversion Details");
    require(contains(allPlainText, "FRONTEND requested=ClangExperimental"),
            "aggressive Clang Debug conversion should report requested frontend");
    if (clangExperimentsEnabled()) {
        require(contains(allPlainText, "selected=ClangExperimentalFrontend")
                    || contains(allPlainText, "fallback=true"),
                "Clang-enabled GUI conversion should either select Clang or report a clean fallback");
        require(!contains(allPlainText, "In-process Clang tooling disabled on worker thread for crash isolation"),
                "Clang-enabled GUI conversion should use the isolated parse service instead of the old worker-thread block");
        if (contains(allPlainText, "selected=ClangExperimentalFrontend")) {
            require(contains(allPlainText, "clang_parse_enabled=true"),
                    "Clang-selected GUI conversion should report enabled Clang parsing");
            require(contains(allPlainText, "isolated_process=true"),
                    "GUI worker Clang parsing should run through the isolated helper process");
        }
    } else {
        require(contains(allPlainText, "fallback=true"),
                "default GUI conversion should fallback cleanly when Clang support is not compiled");
    }
    require(contains(allPlainText, "AST REWRITE enabled=false"),
            "Clang GUI conversion should keep AST rewrite disabled for parse-only re-enablement");
    require(contains(allPlainText, "CLANG VALIDATION enabled=false"),
            "Clang GUI conversion should keep Clang validation disabled for parse-only re-enablement");
    require(contains(allPlainText, "Compile verification"),
            "aggressive Clang Debug conversion should report compile verification status");
    require(!contains(allPlainText, "conversion exception"),
            "aggressive Clang Debug conversion should not surface an exception result");

    inputEditor->setPlainText(
        "#include <string>\n"
        "namespace second {\n"
        "    typedef std::string Name;\n"
        "    class Holder { public: Name value{}; };\n"
        "}\n"
        "int main() { second::Holder holder{}; return holder.value.empty() ? 0 : 1; }\n");
    outputEditor->clear();
    convertButton->click();

    require(pumpUntil(app, 60000, [&]() {
        return convertButton->isEnabled() && !outputEditor->toPlainText().isEmpty();
    }), "second AI aggressive + ClangExperimental conversion should return a result or clean fallback");

    const QString secondRunText = collectPlainText(window);
    require(contains(secondRunText, "FRONTEND requested=ClangExperimental"),
            "second aggressive Clang conversion should report requested frontend");
    require(!contains(secondRunText, "conversion exception"),
            "second aggressive Clang conversion should not surface an exception result");
}

void testCanBufferConversionThroughMainWindow(QApplication& app)
{
    MainWindow window(std::make_unique<RuleBasedConverterEngine>());

    auto [inputEditor, outputEditor] = findCodeEditors(window);
    require(inputEditor != nullptr, "GUI smoke test should find input editor");
    require(outputEditor != nullptr, "GUI smoke test should find output editor");

    configureOfflineMode(window);

    QCheckBox* compileCheckBox = findByText<QCheckBox>(window, "Automatically verify converted code");
    require(compileCheckBox != nullptr, "GUI smoke test should find compile verification checkbox");
    compileCheckBox->setChecked(true);

    QPushButton* convertButton = findByText<QPushButton>(window, "Convert");
    require(convertButton != nullptr, "GUI smoke test should find Convert button");

    inputEditor->setPlainText(canBufferReproInput());
    convertButton->click();

    const bool completed = pumpUntil(app, 35000, [&]() {
        return convertButton->isEnabled() && !outputEditor->toPlainText().isEmpty();
    });

    require(completed,
            "GUI Convert button should finish or timeout in a controlled way for CanBuffer input");
    require(convertButton->isEnabled(), "GUI Convert button should be re-enabled after conversion finishes");
    require(contains(outputEditor->toPlainText(), "CanBufferManager"),
            "GUI Convert button should render converted output for CanBuffer input");
    require(contains(outputEditor->toPlainText(), "inline constexpr auto INITIAL_MAX"),
            "GUI Convert button should convert constant macros in CanBuffer input");
    require(contains(outputEditor->toPlainText(), "struct CanFrame"),
            "GUI Convert button should modernize typedef struct in CanBuffer input");
    require(contains(outputEditor->toPlainText(), "std::vector<CanFrame> backingStore"),
            "GUI Convert button should convert raw dynamic array member to std::vector");
    require(!contains(outputEditor->toPlainText(), "new CanFrame["),
            "GUI Convert button should remove raw dynamic array allocation");
    require(!contains(outputEditor->toPlainText(), "delete[] backingStore"),
            "GUI Convert button should remove manual delete[] cleanup");
    require(!contains(outputEditor->toPlainText(), "newStore"),
            "GUI Convert button should remove manual growth temporary buffers");
    require(contains(outputEditor->toPlainText(), "backingStore.push_back"),
            ("GUI Convert button should rewrite append logic to vector-native push_back\nConverted code:\n"
             + outputEditor->toPlainText()).toStdString());
    require(contains(outputEditor->toPlainText(), "return backingStore.size();"),
            "GUI Convert button should cascade count getter to vector.size()");
    require(contains(outputEditor->toPlainText(), "'\\n'"),
            "GUI Convert button should modernize simple std::endl usage to newline output");

    const QString allPlainText = collectPlainText(window);

    require(contains(allPlainText, "ModernCppConverter version=1.2.0-rc1"),
            "GUI Conversion Details should include release version metadata");
    require(contains(allPlainText, "Frontend Summary"),
            "GUI details should include a frontend summary in Normal diagnostics mode");
    require(contains(allPlainText, "Pass Summary"),
            "GUI details should include concise pass summary in Normal diagnostics mode");
    require(contains(allPlainText, "passes executed"),
            "GUI details should aggregate pass execution counts in Normal diagnostics mode");
    require(!contains(allPlainText, "START PASS VectorParadigmRewritePass"),
            "GUI details/logs should hide START PASS traces in Normal diagnostics mode");
    require(!contains(allPlainText, "END PASS VectorParadigmRewritePass"),
            "GUI details/logs should hide END PASS traces in Normal diagnostics mode");
    require(!contains(allPlainText, "iteration-limit-exceeded"),
            "GUI production conversion path should not hit the convergence guard for CanBuffer input");
    require(contains(allPlainText, "Compile verification passed"),
            "GUI production conversion path should run compile verification successfully");
}

void testDebugDiagnosticsShowRawPassTraces(QApplication& app)
{
    MainWindow window(std::make_unique<DebugDiagnosticConverterEngine>());
    configureOfflineMode(window);

    auto [inputEditor, outputEditor] = findCodeEditors(window);
    require(inputEditor != nullptr, "debug diagnostic test should find input editor");
    require(outputEditor != nullptr, "debug diagnostic test should find output editor");

    QPushButton* convertButton = findByText<QPushButton>(window, "Convert");
    require(convertButton != nullptr, "debug diagnostic test should find Convert button");

    inputEditor->setPlainText("int main() { return 0; }\n");
    convertButton->click();
    require(pumpUntil(app, 2000, [&]() {
        return convertButton->isEnabled() && !outputEditor->toPlainText().isEmpty();
    }), "debug diagnostic conversion should finish");

    const QString allPlainText = collectPlainText(window);
    require(contains(allPlainText, "Raw Diagnostics"),
            "Debug diagnostics should include raw diagnostics section");
    require(contains(allPlainText, "START PASS VectorParadigmRewritePass"),
            "Debug diagnostics should preserve START PASS traces");
    require(contains(allPlainText, "END PASS VectorParadigmRewritePass"),
            "Debug diagnostics should preserve END PASS traces");
    require(contains(allPlainText, "candidates found: 0"),
            "Debug diagnostics should preserve no-op functional diagnostics");
    require(countOccurrences(allPlainText, "START PASS VectorParadigmRewritePass") == 1,
            "Debug diagnostics should not duplicate START PASS traces across multiple sections");
    require(countOccurrences(allPlainText, "END PASS VectorParadigmRewritePass") == 1,
            "Debug diagnostics should not duplicate END PASS traces across multiple sections");
}

void testSummaryFrontendShowsOneAuthoritativeClangSuccessLine(QApplication& app)
{
    const QString allPlainText = runStaticDiagnosticConversion(app, summaryDiagnosticResult({
        "FRONTEND requested=ClangExperimental selected=ClangExperimentalFrontend clang_enabled=true clang_available=true fallback=false reason=\"\" parse=success classes=2 functions=14 enums=1 variables=18",
        "FRONTEND used=ClangExperimentalFrontend experimental=true parse=success classes=2 functions=14 enums=1 variables=18",
        "FRONTEND clang_parse=success fallback=none",
        "FRONTEND_COMPARE lightweight_classes=2 clang_classes=2 lightweight_functions=14 clang_functions=14 lightweight_enums=1 clang_enums=1",
        "PASS SUMMARY pass=\"NoOpPass\" applied=0 skipped=0 warnings=0 rollbacks=0 visited=0 modified=0 rewrites=0 time_ms=0 status=no-op",
        "COMPILE VERIFICATION status=passed stage=initial compiler=test time_ms=0",
        "FINAL RESULT status=success applied=0 suggestions=0 skipped=0 compile=passed",
    }));

    require(countOccurrences(allPlainText, "FRONTEND ") == 1,
            "Summary diagnostics should show exactly one authoritative frontend line");
    require(contains(allPlainText,
                     "FRONTEND requested=ClangExperimental selected=ClangExperimentalFrontend clang_enabled=true clang_available=true fallback=false reason=\"\" parse=success classes=2 functions=14 enums=1 variables=18"),
            "Summary diagnostics should show the selected Clang frontend line");
    require(!contains(allPlainText, "FRONTEND used=ClangExperimentalFrontend"),
            "Summary diagnostics should hide internal frontend-used diagnostics");
    require(!contains(allPlainText, "FRONTEND clang_parse=success"),
            "Summary diagnostics should hide low-level Clang parse diagnostics");
}

void testSummaryFrontendShowsOneFallbackLineWithReason(QApplication& app)
{
    const QString allPlainText = runStaticDiagnosticConversion(app, summaryDiagnosticResult({
        "FRONTEND requested=ClangExperimental selected=LightweightFrontend clang_enabled=true clang_available=true fallback=true reason=\"Clang parse failed; LightweightFrontend fallback used\" parse=success classes=1 functions=1 enums=0 variables=2",
        "FRONTEND used=ClangExperimentalFrontend experimental=true parse=fallback classes=1 functions=1 enums=0 variables=2",
        "FRONTEND clang_parse=failure fallback=LightweightFrontend",
        "COMPILE VERIFICATION status=passed stage=initial compiler=test time_ms=0",
        "FINAL RESULT status=success applied=0 suggestions=0 skipped=0 compile=passed",
    }));

    require(countOccurrences(allPlainText, "FRONTEND ") == 1,
            "Summary fallback diagnostics should show exactly one authoritative frontend line");
    require(contains(allPlainText, "selected=LightweightFrontend"),
            "Summary fallback diagnostics should report the actual selected fallback frontend");
    require(contains(allPlainText, "fallback=true"),
            "Summary fallback diagnostics should report fallback=true");
    require(contains(allPlainText, "reason=\"Clang parse failed; LightweightFrontend fallback used\""),
            "Summary fallback diagnostics should include an explicit fallback reason");
    require(!contains(allPlainText, "FRONTEND used=ClangExperimentalFrontend"),
            "Summary fallback diagnostics should not also claim Clang was used successfully");
}

void testSummaryHidesNoOpPassDiagnostics(QApplication& app)
{
    std::vector<ConversionChange> changes;
    changes.push_back(ConversionChange{
        "Functional diagnostics: FunctorToLambdaPass",
        "pass started",
        {},
        "candidates found: 0, candidates converted: 0, candidates skipped: 0",
        false,
        true,
    });

    const QString allPlainText = runStaticDiagnosticConversion(app, summaryDiagnosticResult({
        "FRONTEND requested=Lightweight selected=LightweightFrontend clang_enabled=false clang_available=false fallback=false reason=\"\" parse=success classes=0 functions=1 enums=0 variables=0",
        "START PASS FunctorToLambdaPass iteration=1 hash_before=aaa visited=0",
        "END PASS FunctorToLambdaPass iteration=1 hash_before=aaa hash_after=aaa visited=0 modified=0 rewrites=0 time_ms=0 status=no-op",
        "PASS SUMMARY pass=\"FunctorToLambdaPass\" applied=0 skipped=0 warnings=0 rollbacks=0 visited=0 modified=0 rewrites=0 time_ms=0 status=no-op",
        "PASS SUMMARY pass=\"VectorParadigmRewritePass\" applied=1 skipped=0 warnings=0 rollbacks=0 visited=4 modified=1 rewrites=1 time_ms=12 status=changed",
        "PASS SUMMARY pass=\"IncludeCleanupPass\" applied=1 skipped=0 warnings=0 rollbacks=0 visited=2 modified=1 rewrites=1 time_ms=4 status=changed",
        "Functional diagnostics: FunctorToLambdaPass candidates found: 0, candidates converted: 0, candidates skipped: 0",
        "COMPILE VERIFICATION status=passed stage=initial compiler=test time_ms=0",
        "FINAL RESULT status=success applied=0 suggestions=0 skipped=0 compile=passed",
    }, std::move(changes)));

    require(!contains(allPlainText, "Functional diagnostics"),
            "Summary diagnostics should hide no-op functional diagnostic entries");
    require(!contains(allPlainText, "FunctorToLambdaPass"),
            "Summary diagnostics should hide no-op pass names");
    require(!contains(allPlainText, "candidates found: 0"),
            "Summary diagnostics should hide zero-candidate diagnostic details");
    require(!contains(allPlainText, "START PASS"),
            "Summary diagnostics should hide START PASS traces");
    require(!contains(allPlainText, "END PASS"),
            "Summary diagnostics should hide END PASS traces");
    require(!contains(allPlainText, "hash_before="),
            "Summary diagnostics should hide raw pass hashes");
    require(contains(allPlainText, "Elapsed Time Summary"),
            "Summary diagnostics should show a concise elapsed-time summary");
    require(contains(allPlainText, "top_pass_1=VectorParadigmRewritePass"),
            "Summary diagnostics should show the slowest changed passes");
    require(contains(allPlainText, "top_pass_2=IncludeCleanupPass"),
            "Summary diagnostics should show only concise top slowest pass entries");
    require(contains(allPlainText, "Pass Summary"),
            "Summary diagnostics should keep the concise pass summary section");
}

void testSummaryRollbackRecoveryIsNotUnresolvedError(QApplication& app)
{
    const QString allPlainText = runStaticDiagnosticConversion(app, summaryDiagnosticResult({
        "FRONTEND requested=Lightweight selected=LightweightFrontend clang_enabled=false clang_available=false fallback=false reason=\"\" parse=success classes=0 functions=1 enums=0 variables=0",
        "ROLLBACK DETAIL category=Compilation reason=\"compile verification failed\" pass=\"CompileVerification\" entity=\"translation-unit\" severity=Error",
        "ROLLBACK SUMMARY Ownership=0 Semantic=0 String=0 Containers=0 Macros=0 Compilation=1 Repository=0 Infrastructure=0 Info=0 Warnings=0 Errors=1",
        "COMPILE VERIFICATION status=passed stage=retry compiler=test time_ms=0",
        "FINAL RESULT status=success applied=0 suggestions=0 skipped=0 compile=passed",
    }));

    require(contains(allPlainText, "Recovery: rollback applied successfully"),
            "Successful rollback with final compile pass should be reported as recovered");
    require(contains(allPlainText, "severity=Warning/Recovered"),
            "Successful rollback recovery should be normalized to warning/recovered severity");
    require(!contains(allPlainText, "severity=Error"),
            "Successful rollback recovery should not appear as an unresolved error in Summary diagnostics");
    require(contains(allPlainText, "FINAL RESULT status=success"),
            "Final result status should remain visible and accurate");
}

void testClearReleasesActiveConversion(QApplication& app)
{
    MainWindow window(std::make_unique<SlowFirstConverterEngine>(700));
    configureOfflineMode(window);

    auto [inputEditor, outputEditor] = findCodeEditors(window);
    require(inputEditor != nullptr, "clear test should find input editor");
    require(outputEditor != nullptr, "clear test should find output editor");

    QPushButton* convertButton = findByText<QPushButton>(window, "Convert");
    QPushButton* clearButton = findByText<QPushButton>(window, "Clear");
    require(convertButton != nullptr, "clear test should find Convert button");
    require(clearButton != nullptr, "clear test should find Clear button");

    inputEditor->setPlainText("first conversion");
    convertButton->click();
    require(pumpUntil(app, 1000, [&]() { return !convertButton->isEnabled(); }),
            "first conversion should start and disable Convert");

    clearButton->click();
    app.processEvents();
    require(convertButton->isEnabled(), "Clear should release Convert button immediately");

    inputEditor->setPlainText("second conversion");
    convertButton->click();
    require(pumpUntil(app, 2000, [&]() {
        return outputEditor->toPlainText().contains("converted-call-2");
    }), "second conversion should start immediately after Clear");

    pumpFor(app, 900);
    require(outputEditor->toPlainText().contains("converted-call-2"),
            "stale first conversion result should not overwrite second conversion output");
    require(!collectPlainText(window).contains("previous worker is still finishing"),
            "Clear path should not leave the old previous-worker warning visible");
}

void testTimeoutReleasesActiveConversion(QApplication& app)
{
    qputenv("MODERNCPP_CONVERSION_TIMEOUT_MS", "100");
    MainWindow window(std::make_unique<SlowFirstConverterEngine>(700));
    configureOfflineMode(window);

    auto [inputEditor, outputEditor] = findCodeEditors(window);
    require(inputEditor != nullptr, "timeout test should find input editor");
    require(outputEditor != nullptr, "timeout test should find output editor");

    QPushButton* convertButton = findByText<QPushButton>(window, "Convert");
    require(convertButton != nullptr, "timeout test should find Convert button");

    inputEditor->setPlainText("first timeout conversion");
    convertButton->click();
    require(pumpUntil(app, 2000, [&]() {
        return convertButton->isEnabled() && collectPlainText(window).contains("Conversion Timeout");
    }), "timeout should release Convert button and show a controlled timeout result");

    inputEditor->setPlainText("second conversion after timeout");
    convertButton->click();
    require(pumpUntil(app, 2000, [&]() {
        return outputEditor->toPlainText().contains("converted-call-2");
    }), "second conversion should start immediately after timeout");

    pumpFor(app, 900);
    require(outputEditor->toPlainText().contains("converted-call-2"),
            "stale timed-out conversion result should not overwrite later output");
    require(!collectPlainText(window).contains("previous worker is still finishing"),
            "timeout path should not leave the old previous-worker warning visible");
    qputenv("MODERNCPP_CONVERSION_TIMEOUT_MS", "30000");
}

} // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("MODERNCPP_CONVERSION_TIMEOUT_MS", "30000");
    CrashBreadcrumb::installSignalHandlers();
    QApplication app(argc, argv);

    testBalancedThenAggressiveClangDebugConversionDoesNotCrash(app);
    testCanBufferConversionThroughMainWindow(app);
    testDebugDiagnosticsShowRawPassTraces(app);
    testSummaryFrontendShowsOneAuthoritativeClangSuccessLine(app);
    testSummaryFrontendShowsOneFallbackLineWithReason(app);
    testSummaryHidesNoOpPassDiagnostics(app);
    testSummaryRollbackRecoveryIsNotUnresolvedError(app);
    testClearReleasesActiveConversion(app);
    testTimeoutReleasesActiveConversion(app);

    return EXIT_SUCCESS;
}
