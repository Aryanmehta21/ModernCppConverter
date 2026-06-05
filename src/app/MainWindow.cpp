#include "app/MainWindow.h"

#include "backend/BackendClient.h"
#include "backend/BackendConfig.h"

#include <algorithm>
#include <stdexcept>

#include <QApplication>
#include <QClipboard>
#include <QCheckBox>
#include <QComboBox>
#include <QFontDatabase>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <utility>

namespace
{
QPlainTextEdit* createCodeEditor(const QString& placeholder, bool readOnly)
{
    auto* editor = new QPlainTextEdit;
    editor->setPlaceholderText(placeholder);
    editor->setReadOnly(readOnly);
    editor->setLineWrapMode(QPlainTextEdit::NoWrap);

    const QFont fixedFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    editor->setFont(fixedFont);
    return editor;
}

QWidget* labeledPanel(const QString& label, QWidget* child)
{
    auto* panel = new QWidget;
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto* title = new QLabel(label);
    title->setStyleSheet("font-weight: 600;");

    layout->addWidget(title);
    layout->addWidget(child);
    return panel;
}

QCheckBox* addOption(QVBoxLayout* layout, const QString& label)
{
    auto* checkBox = new QCheckBox(label);
    layout->addWidget(checkBox);
    return checkBox;
}

QGroupBox* createOptionGroup(const QString& title, QVBoxLayout*& layout)
{
    auto* group = new QGroupBox(title);
    layout = new QVBoxLayout(group);
    layout->setSpacing(3);
    return group;
}
} // namespace

MainWindow::MainWindow(std::unique_ptr<IConverterEngine> converterEngine, QWidget* parent)
    : QMainWindow(parent)
{
    if (!converterEngine) {
        throw std::invalid_argument("MainWindow requires a converter engine.");
    }
    conversionCoordinator_ = std::make_unique<ConversionCoordinator>(
        std::move(converterEngine),
        std::make_unique<BackendClient>(BackendConfig::loadFromFile("config/app_config.json")));

    buildUi();
    statusBar()->showMessage("Ready");
}

void MainWindow::buildUi()
{
    setWindowTitle("Modern C++ Converter");
    resize(1200, 800);

    auto* central = new QWidget(this);
    auto* rootLayout = new QVBoxLayout(central);
    rootLayout->setContentsMargins(10, 10, 10, 10);
    rootLayout->setSpacing(8);

    auto* buttonRow = new QHBoxLayout;
    auto* convertButton = new QPushButton("Convert");
    auto* clearButton = new QPushButton("Clear");
    auto* copyButton = new QPushButton("Copy Output");
    auto* checkBackendButton = new QPushButton("Check Backend Connection");
    conversionModeComboBox_ = new QComboBox;
    conversionModeComboBox_->addItem("Offline Rule-Based");
    conversionModeComboBox_->addItem("Online AI-Assisted");
    conversionModeComboBox_->addItem("Hybrid (Offline + AI Review)");
    modeStatusLabel_ = new QLabel("Offline Mode Active");

    buttonRow->addWidget(new QLabel("Conversion Mode"));
    buttonRow->addWidget(conversionModeComboBox_);
    buttonRow->addWidget(convertButton);
    buttonRow->addWidget(clearButton);
    buttonRow->addStretch();
    buttonRow->addWidget(modeStatusLabel_);
    buttonRow->addWidget(checkBackendButton);
    buttonRow->addWidget(copyButton);

    inputEditor_ = createCodeEditor("Paste legacy C++ code here...", false);
    outputEditor_ = createCodeEditor("Modernized code will appear here...", true);
    detailsEditor_ = createCodeEditor("Conversion details will appear here...", true);
    explanationEditor_ = createCodeEditor("Modern C++ explanation will appear here...", true);

    auto* codeSplitter = new QSplitter(Qt::Horizontal);
    codeSplitter->addWidget(labeledPanel("Input code editor", inputEditor_));
    codeSplitter->addWidget(labeledPanel("Output code editor", outputEditor_));
    codeSplitter->setStretchFactor(0, 1);
    codeSplitter->setStretchFactor(1, 1);

    auto* editorSplitter = new QSplitter(Qt::Horizontal);
    editorSplitter->addWidget(createOptionsPanel());
    editorSplitter->addWidget(codeSplitter);
    editorSplitter->setStretchFactor(0, 0);
    editorSplitter->setStretchFactor(1, 1);

    auto* bottomTabs = new QTabWidget;
    bottomTabs->addTab(detailsEditor_, "Conversion Details");
    bottomTabs->addTab(explanationEditor_, "Modern C++ Explanation");
    bottomTabs->setMinimumHeight(180);

    auto* mainSplitter = new QSplitter(Qt::Vertical);
    mainSplitter->addWidget(editorSplitter);
    mainSplitter->addWidget(bottomTabs);
    mainSplitter->setStretchFactor(0, 3);
    mainSplitter->setStretchFactor(1, 1);

    rootLayout->addLayout(buttonRow);
    rootLayout->addWidget(mainSplitter);

    setCentralWidget(central);

    connect(convertButton, &QPushButton::clicked, this, &MainWindow::convertCode);
    connect(clearButton, &QPushButton::clicked, this, &MainWindow::clearEditors);
    connect(copyButton, &QPushButton::clicked, this, &MainWindow::copyOutputToClipboard);
    connect(checkBackendButton, &QPushButton::clicked, this, &MainWindow::checkBackendConnection);
    connect(conversionModeComboBox_, &QComboBox::currentIndexChanged, this, [this]() {
        updateModeStatus(readConversionMode(), false);
    });
}

QWidget* MainWindow::createOptionsPanel()
{
    auto* panel = new QWidget;
    auto* panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    panelLayout->setSpacing(8);

    auto* title = new QLabel("Modern C++ Features");
    title->setStyleSheet("font-weight: 600;");
    panelLayout->addWidget(title);

    auto* buttonRow = new QHBoxLayout;
    auto* selectAllButton = new QPushButton("Select All");
    auto* clearAllButton = new QPushButton("Clear All");
    auto* defaultsButton = new QPushButton("Recommended Safe Defaults");
    buttonRow->addWidget(selectAllButton);
    buttonRow->addWidget(clearAllButton);
    buttonRow->addWidget(defaultsButton);
    panelLayout->addLayout(buttonRow);

    auto* scrollArea = new QScrollArea;
    scrollArea->setWidgetResizable(true);
    scrollArea->setMinimumWidth(300);

    auto* content = new QWidget;
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setSpacing(8);

    QVBoxLayout* cxx11Layout = nullptr;
    auto* cxx11Group = createOptionGroup("C++11", cxx11Layout);
    nullptrCheckBox_ = addOption(cxx11Layout, "nullptr");
    usingAliasesCheckBox_ = addOption(cxx11Layout, "using aliases");
    autoCheckBox_ = addOption(cxx11Layout, "auto");
    rangeBasedForCheckBox_ = addOption(cxx11Layout, "range-based for loops");
    lambdasCheckBox_ = addOption(cxx11Layout, "lambdas");
    overrideFinalCheckBox_ = addOption(cxx11Layout, "override/final");
    constexprCheckBox_ = addOption(cxx11Layout, "constexpr");
    smartPointersCheckBox_ = addOption(cxx11Layout, "smart pointers");
    moveSemanticsCheckBox_ = addOption(cxx11Layout, "move semantics");
    enumClassCheckBox_ = addOption(cxx11Layout, "enum class");
    contentLayout->addWidget(cxx11Group);

    QVBoxLayout* cxx14Layout = nullptr;
    auto* cxx14Group = createOptionGroup("C++14", cxx14Layout);
    genericLambdasCheckBox_ = addOption(cxx14Layout, "generic lambdas");
    makeUniqueCheckBox_ = addOption(cxx14Layout, "make_unique");
    applySafeOwnershipModernizationCheckBox_ = addOption(cxx14Layout, "Apply safe raw pointer ownership modernization");
    contentLayout->addWidget(cxx14Group);

    QVBoxLayout* cxx17Layout = nullptr;
    auto* cxx17Group = createOptionGroup("C++17", cxx17Layout);
    structuredBindingsCheckBox_ = addOption(cxx17Layout, "structured bindings");
    ifConstexprCheckBox_ = addOption(cxx17Layout, "if constexpr");
    optionalCheckBox_ = addOption(cxx17Layout, "std::optional");
    variantCheckBox_ = addOption(cxx17Layout, "std::variant");
    stringViewCheckBox_ = addOption(cxx17Layout, "std::string_view");
    applyStringViewWhenSafeCheckBox_ = addOption(cxx17Layout, "Apply safe std::string_view modernization");
    inlineVariablesCheckBox_ = addOption(cxx17Layout, "inline variables");
    contentLayout->addWidget(cxx17Group);

    QVBoxLayout* cxx20Layout = nullptr;
    auto* cxx20Group = createOptionGroup("C++20", cxx20Layout);
    conceptsCheckBox_ = addOption(cxx20Layout, "concepts");
    rangesCheckBox_ = addOption(cxx20Layout, "ranges");
    spanCheckBox_ = addOption(cxx20Layout, "span");
    designatedInitializersCheckBox_ = addOption(cxx20Layout, "designated initializers");
    constevalConstinitCheckBox_ = addOption(cxx20Layout, "consteval/constinit");
    spaceshipOperatorCheckBox_ = addOption(cxx20Layout, "spaceship operator");
    contentLayout->addWidget(cxx20Group);

    customInstructionEdit_ = new QLineEdit;
    customInstructionEdit_->setPlaceholderText("Prefer std::vector over raw arrays where possible.");
    contentLayout->addWidget(new QLabel("Custom modernization instruction"));
    contentLayout->addWidget(customInstructionEdit_);
    contentLayout->addStretch();

    scrollArea->setWidget(content);
    panelLayout->addWidget(scrollArea);

    connect(selectAllButton, &QPushButton::clicked, this, &MainWindow::selectAllOptions);
    connect(clearAllButton, &QPushButton::clicked, this, &MainWindow::clearAllOptions);
    connect(defaultsButton, &QPushButton::clicked, this, &MainWindow::applyRecommendedSafeDefaults);

    applyRecommendedSafeDefaults();
    return panel;
}

void MainWindow::convertCode()
{
    const std::string input = inputEditor_->toPlainText().toStdString();
    const CoordinatedConversionResult conversion = conversionCoordinator_->convert(input, readModernizationOptions(), readConversionMode());
    displayResult(conversion.result);
    updateModeStatus(conversion.effectiveMode, conversion.backendUnavailable);

    const bool hasSuggestions = std::any_of(conversion.result.changes.begin(), conversion.result.changes.end(), [](const ConversionChange& change) {
        return !change.applied;
    });

    if (conversion.backendUnavailable) {
        statusBar()->showMessage(QString::fromStdString(conversion.warning));
    } else {
        statusBar()->showMessage(hasSuggestions ? "Conversion warnings" : "Conversion successful");
    }
}

ConversionMode MainWindow::readConversionMode() const
{
    switch (conversionModeComboBox_->currentIndex()) {
    case 1:
        return ConversionMode::OnlineAiAssisted;
    case 2:
        return ConversionMode::HybridOfflineAiReview;
    default:
        return ConversionMode::OfflineRuleBased;
    }
}

void MainWindow::updateModeStatus(ConversionMode mode, bool backendUnavailable)
{
    if (backendUnavailable) {
        modeStatusLabel_->setText("Backend Unavailable");
        return;
    }

    switch (mode) {
    case ConversionMode::OfflineRuleBased:
        modeStatusLabel_->setText("Offline Mode Active");
        break;
    case ConversionMode::OnlineAiAssisted:
        modeStatusLabel_->setText("Online AI Mode Active");
        break;
    case ConversionMode::HybridOfflineAiReview:
        modeStatusLabel_->setText("Hybrid Mode Active");
        break;
    }
}

void MainWindow::checkBackendConnection()
{
    const bool available = conversionCoordinator_->backendAvailable();
    modeStatusLabel_->setText(available ? "Online AI Mode Active" : "Backend Unavailable");
    statusBar()->showMessage(available ? "Backend connection successful" : "AI backend unavailable. Falling back to Offline Mode.");
}

std::vector<QCheckBox*> MainWindow::allOptionCheckboxes() const
{
    return {
        nullptrCheckBox_, usingAliasesCheckBox_, autoCheckBox_, rangeBasedForCheckBox_, lambdasCheckBox_,
        overrideFinalCheckBox_, constexprCheckBox_, smartPointersCheckBox_, moveSemanticsCheckBox_,
        enumClassCheckBox_, genericLambdasCheckBox_, makeUniqueCheckBox_, applySafeOwnershipModernizationCheckBox_,
        structuredBindingsCheckBox_, ifConstexprCheckBox_, optionalCheckBox_, variantCheckBox_, stringViewCheckBox_,
        applyStringViewWhenSafeCheckBox_, inlineVariablesCheckBox_,
        conceptsCheckBox_, rangesCheckBox_, spanCheckBox_, designatedInitializersCheckBox_,
        constevalConstinitCheckBox_, spaceshipOperatorCheckBox_,
    };
}

void MainWindow::setAllOptions(bool checked)
{
    for (auto* checkBox : allOptionCheckboxes()) {
        checkBox->setChecked(checked);
    }
}

void MainWindow::selectAllOptions()
{
    setAllOptions(true);
}

void MainWindow::clearAllOptions()
{
    setAllOptions(false);
}

void MainWindow::applyRecommendedSafeDefaults()
{
    setAllOptions(false);
    nullptrCheckBox_->setChecked(true);
    usingAliasesCheckBox_->setChecked(true);
    overrideFinalCheckBox_->setChecked(true);
    rangeBasedForCheckBox_->setChecked(true);
    smartPointersCheckBox_->setChecked(true);
    enumClassCheckBox_->setChecked(true);
    makeUniqueCheckBox_->setChecked(true);
    applySafeOwnershipModernizationCheckBox_->setChecked(true);
    optionalCheckBox_->setChecked(true);
    stringViewCheckBox_->setChecked(true);
    applyStringViewWhenSafeCheckBox_->setChecked(false);
}

ModernizationOptions MainWindow::readModernizationOptions() const
{
    ModernizationOptions options;
    options.useNullptr = nullptrCheckBox_->isChecked();
    options.useUsingAliases = usingAliasesCheckBox_->isChecked();
    options.useAuto = autoCheckBox_->isChecked();
    options.useRangeBasedFor = rangeBasedForCheckBox_->isChecked();
    options.useLambdas = lambdasCheckBox_->isChecked();
    options.useOverrideFinal = overrideFinalCheckBox_->isChecked();
    options.useConstexpr = constexprCheckBox_->isChecked();
    options.useSmartPointers = smartPointersCheckBox_->isChecked();
    options.useMoveSemantics = moveSemanticsCheckBox_->isChecked();
    options.useEnumClass = enumClassCheckBox_->isChecked();
    options.useGenericLambdas = genericLambdasCheckBox_->isChecked();
    options.useMakeUnique = makeUniqueCheckBox_->isChecked();
    options.applySafeOwnershipModernization = applySafeOwnershipModernizationCheckBox_->isChecked();
    options.useStructuredBindings = structuredBindingsCheckBox_->isChecked();
    options.useIfConstexpr = ifConstexprCheckBox_->isChecked();
    options.useOptional = optionalCheckBox_->isChecked();
    options.useVariant = variantCheckBox_->isChecked();
    options.useStringView = stringViewCheckBox_->isChecked();
    options.applyStringViewWhenSafe = applyStringViewWhenSafeCheckBox_->isChecked();
    options.useInlineVariables = inlineVariablesCheckBox_->isChecked();
    options.useConcepts = conceptsCheckBox_->isChecked();
    options.useRanges = rangesCheckBox_->isChecked();
    options.useSpan = spanCheckBox_->isChecked();
    options.useDesignatedInitializers = designatedInitializersCheckBox_->isChecked();
    options.useConstevalConstinit = constevalConstinitCheckBox_->isChecked();
    options.useSpaceshipOperator = spaceshipOperatorCheckBox_->isChecked();
    options.customInstruction = customInstructionEdit_->text().toStdString();
    return options;
}

void MainWindow::clearEditors()
{
    inputEditor_->clear();
    outputEditor_->clear();
    detailsEditor_->clear();
    explanationEditor_->clear();
    statusBar()->showMessage("Ready");
}

void MainWindow::copyOutputToClipboard()
{
    QApplication::clipboard()->setText(outputEditor_->toPlainText());
    statusBar()->showMessage("Output copied");
}

void MainWindow::displayResult(const ConversionResult& result)
{
    outputEditor_->setPlainText(QString::fromStdString(result.modernCode));
    detailsEditor_->setPlainText(formatChanges(result));
    explanationEditor_->setPlainText(QString::fromStdString(result.explanation));
}

QString MainWindow::formatChanges(const ConversionResult& result) const
{
    if (result.changes.empty()) {
        return "No applied changes, suggestions, or skipped rules.";
    }

    QString details;
    auto appendChange = [&details](const ConversionChange& change, int index) {
        details += QString::number(index);
        details += ". Rule:\n";
        details += QString::fromStdString(change.ruleName);
        details += "\n\nBefore:\n";
        details += QString::fromStdString(change.before);

        details += "\n\nAfter:\n";
        if (change.skipped) {
            details += "(skipped, code unchanged)";
        } else {
            details += change.after.empty() ? QString("(suggestion only, code unchanged)") : QString::fromStdString(change.after);
        }

        details += "\n\nReason:\n";
        details += QString::fromStdString(change.reason);

        details += "\n\nApplied:\n";
        details += change.applied ? "true" : "false";
        details += "\n\nSkipped:\n";
        details += change.skipped ? "true" : "false";
        details += "\n\n---\n\n";
    };

    auto appendSection = [&result, &details, &appendChange](const QString& title, bool applied, bool skipped) {
        details += title;
        details += "\n";
        details += QString(title.size(), QChar('='));
        details += "\n\n";

        int index = 1;
        for (const ConversionChange& change : result.changes) {
            if (change.applied == applied && change.skipped == skipped) {
                appendChange(change, index);
                ++index;
            }
        }

        if (index == 1) {
            details += "None.\n\n";
        }
    };

    appendSection("Applied Changes", true, false);
    appendSection("Suggestions", false, false);
    appendSection("Skipped Rules", false, true);

    if (!result.changes.empty()) {
        details = details.trimmed();
    }

    return details;
}
