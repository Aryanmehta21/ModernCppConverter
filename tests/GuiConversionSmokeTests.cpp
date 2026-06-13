#include "app/MainWindow.h"
#include "converter/IConverterEngine.h"
#include "converter/RuleBasedConverterEngine.h"
#include "editor/CppCodeEditor.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <thread>
#include <utility>

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

void configureOfflineMode(MainWindow& window)
{
    QComboBox* modeCombo = findComboWithItem(window, "Offline Rule-Based");
    require(modeCombo != nullptr, "GUI smoke test should find conversion mode combo");
    modeCombo->setCurrentIndex(modeCombo->findText("Offline Rule-Based"));
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

    require(contains(allPlainText, "START PASS VectorParadigmRewritePass"),
            "GUI details/logs should include START PASS diagnostics from production conversion path");
    require(contains(allPlainText, "END PASS VectorParadigmRewritePass"),
            "GUI details/logs should include END PASS diagnostics from production conversion path");
    require(!contains(allPlainText, "iteration-limit-exceeded"),
            "GUI production conversion path should not hit the convergence guard for CanBuffer input");
    require(contains(allPlainText, "Compile verification passed"),
            "GUI production conversion path should run compile verification successfully");
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
    QApplication app(argc, argv);

    testCanBufferConversionThroughMainWindow(app);
    testClearReleasesActiveConversion(app);
    testTimeoutReleasesActiveConversion(app);

    return EXIT_SUCCESS;
}
