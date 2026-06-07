#include "app/MainWindow.h"

#include "backend/BackendClient.h"
#include "backend/BackendConfig.h"
#include "converter/CompileVerifier.h"
#include "converter/RuleBasedConverterEngine.h"
#include "repository/RepositoryCloneService.h"
#include "repository/RepositoryModernizationService.h"

#include <algorithm>
#include <stdexcept>

#include <QApplication>
#include <QClipboard>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDebug>
#include <QDir>
#include <QFileDialog>
#include <QFontDatabase>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QGuiApplication>
#include <QScreen>
#include <QScrollArea>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolBar>
#include <QUrl>
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

OfflineModernizationLevel levelFromIndex(int index)
{
    switch (index) {
    case 0:
        return OfflineModernizationLevel::Conservative;
    case 2:
        return OfflineModernizationLevel::AggressiveSafe;
    case 3:
        return OfflineModernizationLevel::AiStyleAggressiveRewrite;
    default:
        return OfflineModernizationLevel::Balanced;
    }
}

QString defaultRepositoryWorkspace()
{
    return QDir::home().filePath("ModernCppConverterWorkspaces");
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

    auto* toolbar = addToolBar("Conversion Controls");
    toolbar->setMovable(false);
    toolbar->setFloatable(false);

    auto* convertButton = new QPushButton("Convert");
    auto* verifyButton = new QPushButton("Verify");
    auto* clearButton = new QPushButton("Clear");
    auto* copyButton = new QPushButton("Copy Output");
    auto* checkBackendButton = new QPushButton("Check Backend");
    conversionModeComboBox_ = new QComboBox;
    conversionModeComboBox_->addItem("Offline Rule-Based");
    conversionModeComboBox_->addItem("Online AI-Assisted");
    conversionModeComboBox_->addItem("Hybrid");
    conversionModeComboBox_->setMinimumWidth(165);
    offlineModernizationLevelComboBox_ = new QComboBox;
    offlineModernizationLevelComboBox_->addItem("Conservative");
    offlineModernizationLevelComboBox_->addItem("Balanced");
    offlineModernizationLevelComboBox_->addItem("Aggressive Safe");
    offlineModernizationLevelComboBox_->addItem("AI-Style Aggressive Rewrite");
    offlineModernizationLevelComboBox_->setCurrentIndex(1);
    offlineModernizationLevelComboBox_->setMinimumWidth(200);
    modeStatusLabel_ = new QLabel("Offline Mode Active");

    toolbar->addWidget(new QLabel("Mode"));
    toolbar->addWidget(conversionModeComboBox_);
    toolbar->addSeparator();
    toolbar->addWidget(new QLabel("Level"));
    toolbar->addWidget(offlineModernizationLevelComboBox_);
    toolbar->addSeparator();
    toolbar->addWidget(convertButton);
    toolbar->addWidget(verifyButton);
    toolbar->addWidget(clearButton);
    toolbar->addWidget(copyButton);
    toolbar->addSeparator();
    toolbar->addWidget(checkBackendButton);
    toolbar->addSeparator();
    toolbar->addWidget(modeStatusLabel_);

    auto* tabs = new QTabWidget(this);
    tabs->addTab(createCodeConverterTab(), "Code Converter");
    tabs->addTab(createRepositoryPanel(), "Repository Modernization");
    tabs->addTab(createOptionsPanel(), "Options");
    tabs->addTab(createDiagnosticsPanel(), "Logs / Diagnostics");
    setCentralWidget(tabs);

    statusModeLabel_ = new QLabel("Mode: Offline");
    statusBackendLabel_ = new QLabel("Backend: Not checked");
    statusCompileLabel_ = new QLabel("Compile: Not run");
    statusSourceLabel_ = new QLabel("Source: None");
    statusBar()->addPermanentWidget(statusModeLabel_);
    statusBar()->addPermanentWidget(statusBackendLabel_);
    statusBar()->addPermanentWidget(statusCompileLabel_);
    statusBar()->addPermanentWidget(statusSourceLabel_);

    connect(convertButton, &QPushButton::clicked, this, &MainWindow::convertCode);
    connect(clearButton, &QPushButton::clicked, this, &MainWindow::clearEditors);
    connect(copyButton, &QPushButton::clicked, this, &MainWindow::copyOutputToClipboard);
    connect(checkBackendButton, &QPushButton::clicked, this, &MainWindow::checkBackendConnection);
    connect(verifyButton, &QPushButton::clicked, this, &MainWindow::verifyConvertedCode);
    connect(conversionModeComboBox_, &QComboBox::currentIndexChanged, this, [this]() {
        qInfo() << "Conversion mode selected:" << conversionModeComboBox_->currentText();
        updateModeStatus(readConversionMode(), false);
    });

    configureScreenAwareWindowSize();
    appendDiagnostic("Application ready.");
}

void MainWindow::configureScreenAwareWindowSize()
{
    QSize minimumSize(1100, 700);
    QSize desiredSize(1200, 800);

    if (const QScreen* screen = QGuiApplication::primaryScreen()) {
        const QRect available = screen->availableGeometry();
        const QSize availableSize = available.size();
        minimumSize = QSize(std::min(minimumSize.width(), availableSize.width()),
                            std::min(minimumSize.height(), availableSize.height()));
        desiredSize = QSize(std::min(desiredSize.width(), static_cast<int>(availableSize.width() * 0.9)),
                            std::min(desiredSize.height(), static_cast<int>(availableSize.height() * 0.9)));
        desiredSize = desiredSize.expandedTo(minimumSize).boundedTo(availableSize);

        setMinimumSize(minimumSize);
        resize(desiredSize);
        move(available.center() - rect().center());
        return;
    }

    setMinimumSize(minimumSize);
    resize(desiredSize);
}

QWidget* MainWindow::createCodeConverterTab()
{
    auto* panel = new QWidget;
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    inputEditor_ = createCodeEditor("Paste legacy C++ code here...", false);
    outputEditor_ = createCodeEditor("Modernized code will appear here...", true);
    detailsEditor_ = createCodeEditor("Conversion details will appear here...", true);
    explanationEditor_ = createCodeEditor("Modern C++ explanation will appear here...", true);
    compileVerificationEditor_ = createCodeEditor("Compile verification results will appear here...", true);

    auto* codeSplitter = new QSplitter(Qt::Horizontal);
    codeSplitter->addWidget(labeledPanel("Input Code", inputEditor_));
    codeSplitter->addWidget(labeledPanel("Output Code", outputEditor_));
    codeSplitter->setStretchFactor(0, 1);
    codeSplitter->setStretchFactor(1, 1);

    auto* resultTabs = new QTabWidget;
    resultTabs->addTab(detailsEditor_, "Conversion Details");
    resultTabs->addTab(explanationEditor_, "Modern C++ Explanation");
    resultTabs->addTab(compileVerificationEditor_, "Compile Verification");

    auto* splitter = new QSplitter(Qt::Vertical);
    splitter->addWidget(codeSplitter);
    splitter->addWidget(resultTabs);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);
    splitter->setCollapsible(0, false);
    splitter->setCollapsible(1, true);

    layout->addWidget(splitter);
    return panel;
}

QWidget* MainWindow::createOptionsPanel()
{
    auto* panel = new QWidget;
    auto* panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(8, 8, 8, 8);
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

    auto* content = new QWidget;
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setSpacing(8);

    QVBoxLayout* advancedLayout = nullptr;
    auto* advancedGroup = createOptionGroup("Advanced Rewrite Options", advancedLayout);
    offlineRewriteStyleComboBox_ = new QComboBox;
    offlineRewriteStyleComboBox_->addItem("Safe Modernization");
    offlineRewriteStyleComboBox_->addItem("Aggressive AI-like Rewrite");
    automaticCompileVerificationCheckBox_ = addOption(advancedLayout, "Automatically verify converted code");
    customInstructionEdit_ = new QLineEdit;
    customInstructionEdit_->setPlaceholderText("Prefer std::vector over raw arrays where possible.");
    advancedLayout->addWidget(new QLabel("Offline rewrite style"));
    advancedLayout->addWidget(offlineRewriteStyleComboBox_);
    advancedLayout->addWidget(new QLabel("Custom modernization instruction"));
    advancedLayout->addWidget(customInstructionEdit_);
    contentLayout->addWidget(advancedGroup);

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
    stdFormatCheckBox_ = addOption(cxx20Layout, "Use std::format for simple stream formatting");
    contentLayout->addWidget(cxx20Group);

    QVBoxLayout* backendLayout = nullptr;
    auto* backendGroup = createOptionGroup("Backend Settings", backendLayout);
    auto* backendInfo = new QLabel("Backend URL and timeout are loaded from config/app_config.json. AI provider keys stay server-side only.");
    backendInfo->setWordWrap(true);
    backendLayout->addWidget(backendInfo);
    contentLayout->addWidget(backendGroup);

    contentLayout->addStretch();

    scrollArea->setWidget(content);
    panelLayout->addWidget(scrollArea);

    connect(selectAllButton, &QPushButton::clicked, this, &MainWindow::selectAllOptions);
    connect(clearAllButton, &QPushButton::clicked, this, &MainWindow::clearAllOptions);
    connect(defaultsButton, &QPushButton::clicked, this, &MainWindow::applyRecommendedSafeDefaults);

    applyRecommendedSafeDefaults();
    return panel;
}

QWidget* MainWindow::createRepositoryPanel()
{
    auto* panel = new QWidget;
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    auto* sourceGroup = new QGroupBox("Repository Source");
    auto* sourceLayout = new QVBoxLayout(sourceGroup);
    sourceLayout->setSpacing(6);

    repositoryUrlEdit_ = new QLineEdit;
    repositoryUrlEdit_->setPlaceholderText("https://github.com/owner/repository");
    repositoryBranchEdit_ = new QLineEdit;
    repositoryBranchEdit_->setPlaceholderText("Target branch (optional)");
    repositoryWorkspaceEdit_ = new QLineEdit(defaultRepositoryWorkspace());

    repositoryModernizationLevelComboBox_ = new QComboBox;
    repositoryModernizationLevelComboBox_->addItem("Conservative");
    repositoryModernizationLevelComboBox_->addItem("Balanced");
    repositoryModernizationLevelComboBox_->addItem("Aggressive Safe");
    repositoryModernizationLevelComboBox_->addItem("AI-Style Aggressive Rewrite");
    repositoryModernizationLevelComboBox_->setCurrentIndex(1);

    auto* browseButton = new QPushButton("Browse Workspace");
    auto* cloneButton = new QPushButton("Clone Repository");
    auto* scanButton = new QPushButton("Scan C++ Files");
    auto* modernizeButton = new QPushButton("Modernize Repository");
    auto* openReportButton = new QPushButton("Open Report Folder");

    repositoryStatusLabel_ = new QLabel("Repository mode ready");
    repositoryReportEditor_ = createCodeEditor("Repository modernization status and report path will appear here...", true);

    sourceLayout->addWidget(new QLabel("GitHub repository URL"));
    sourceLayout->addWidget(repositoryUrlEdit_);
    sourceLayout->addWidget(new QLabel("Target branch"));
    sourceLayout->addWidget(repositoryBranchEdit_);
    sourceLayout->addWidget(new QLabel("Output workspace folder"));
    sourceLayout->addWidget(repositoryWorkspaceEdit_);
    sourceLayout->addWidget(browseButton);
    sourceLayout->addWidget(new QLabel("Modernization level"));
    sourceLayout->addWidget(repositoryModernizationLevelComboBox_);

    auto* actionRow = new QHBoxLayout;
    actionRow->addWidget(cloneButton);
    actionRow->addWidget(scanButton);
    actionRow->addWidget(modernizeButton);
    actionRow->addWidget(openReportButton);
    actionRow->addStretch();

    layout->addWidget(sourceGroup);
    layout->addLayout(actionRow);
    layout->addWidget(repositoryStatusLabel_);
    layout->addWidget(labeledPanel("Repository Status / Report", repositoryReportEditor_), 1);

    connect(browseButton, &QPushButton::clicked, this, [this]() {
        const QString folder = QFileDialog::getExistingDirectory(this, "Select output workspace", repositoryWorkspaceEdit_->text());
        if (!folder.isEmpty()) {
            repositoryWorkspaceEdit_->setText(folder);
        }
    });
    connect(cloneButton, &QPushButton::clicked, this, &MainWindow::cloneRepository);
    connect(scanButton, &QPushButton::clicked, this, &MainWindow::scanRepository);
    connect(modernizeButton, &QPushButton::clicked, this, &MainWindow::modernizeRepository);
    connect(openReportButton, &QPushButton::clicked, this, &MainWindow::openReportFolder);

    return panel;
}

QWidget* MainWindow::createDiagnosticsPanel()
{
    auto* panel = new QWidget;
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    diagnosticsEditor_ = createCodeEditor("Application logs, backend status, compiler output, and conversion metadata will appear here...", true);
    layout->addWidget(labeledPanel("Logs / Diagnostics", diagnosticsEditor_));
    return panel;
}

void MainWindow::convertCode()
{
    appendDiagnostic("Conversion request started. Mode: " + conversionModeComboBox_->currentText());
    const std::string input = inputEditor_->toPlainText().toStdString();
    const CoordinatedConversionResult conversion = conversionCoordinator_->convert(input, readModernizationOptions(), readConversionMode());
    displayResult(conversion.result);
    updateModeStatus(conversion.effectiveMode, conversion.backendUnavailable);
    updateStatusBarMetadata(conversion.result);

    const bool hasSuggestions = std::any_of(conversion.result.changes.begin(), conversion.result.changes.end(), [](const ConversionChange& change) {
        return !change.applied;
    });

    if (conversion.backendUnavailable) {
        appendDiagnostic("Backend unavailable. Offline fallback was used.");
        statusBar()->showMessage(QString::fromStdString(conversion.warning + " Source: " + conversion.result.conversionSource));
    } else {
        const QString status = hasSuggestions ? "Conversion warnings" : "Conversion successful";
        appendDiagnostic(status + ". Source: " + QString::fromStdString(conversion.result.conversionSource));
        statusBar()->showMessage(status + " - " + QString::fromStdString(conversion.result.conversionSource));
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
        modeStatusLabel_->setText("Offline Fallback after AI failure");
        if (statusModeLabel_ != nullptr) {
            statusModeLabel_->setText("Mode: Fallback");
        }
        if (statusBackendLabel_ != nullptr) {
            statusBackendLabel_->setText("Backend: Unavailable");
        }
        return;
    }

    switch (mode) {
    case ConversionMode::OfflineRuleBased:
        modeStatusLabel_->setText("Offline Mode Active");
        if (statusModeLabel_ != nullptr) {
            statusModeLabel_->setText("Mode: Offline");
        }
        break;
    case ConversionMode::OnlineAiAssisted:
        modeStatusLabel_->setText("Online AI Mode Active");
        if (statusModeLabel_ != nullptr) {
            statusModeLabel_->setText("Mode: Online AI");
        }
        break;
    case ConversionMode::HybridOfflineAiReview:
        modeStatusLabel_->setText("Hybrid Mode Active");
        if (statusModeLabel_ != nullptr) {
            statusModeLabel_->setText("Mode: Hybrid");
        }
        break;
    }
}

void MainWindow::updateStatusBarMetadata(const ConversionResult& result)
{
    if (statusBackendLabel_ != nullptr) {
        statusBackendLabel_->setText("Backend: " + QString::fromStdString(result.backendStatus.empty() ? "Not used" : result.backendStatus));
    }
    if (statusCompileLabel_ != nullptr) {
        if (!result.compileVerificationEnabled) {
            statusCompileLabel_->setText("Compile: Not run");
        } else {
            statusCompileLabel_->setText(result.compileVerificationPassed ? "Compile: Passed" : "Compile: Failed/Skipped");
        }
    }
    if (statusSourceLabel_ != nullptr) {
        statusSourceLabel_->setText("Source: " + QString::fromStdString(result.conversionSource.empty() ? "Unknown" : result.conversionSource));
    }
}

void MainWindow::appendDiagnostic(const QString& message)
{
    if (diagnosticsEditor_ == nullptr) {
        return;
    }

    const QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    diagnosticsEditor_->appendPlainText("[" + timestamp + "] " + message);
}

void MainWindow::checkBackendConnection()
{
    qInfo() << "Manual backend health check starts";
    appendDiagnostic("Backend health check started.");
    const bool available = conversionCoordinator_->backendAvailable();
    qInfo() << "Manual backend health check" << (available ? "succeeded" : "failed");
    modeStatusLabel_->setText(available ? "Online AI Mode Active" : "Backend Unavailable");
    if (statusBackendLabel_ != nullptr) {
        statusBackendLabel_->setText(available ? "Backend: Connected" : "Backend: Unavailable");
    }
    appendDiagnostic(available ? "Backend health check succeeded." : "Backend health check failed.");
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
        constevalConstinitCheckBox_, spaceshipOperatorCheckBox_, stdFormatCheckBox_, automaticCompileVerificationCheckBox_,
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
    autoCheckBox_->setChecked(true);
    overrideFinalCheckBox_->setChecked(true);
    rangeBasedForCheckBox_->setChecked(true);
    lambdasCheckBox_->setChecked(true);
    constexprCheckBox_->setChecked(true);
    smartPointersCheckBox_->setChecked(true);
    enumClassCheckBox_->setChecked(true);
    makeUniqueCheckBox_->setChecked(true);
    applySafeOwnershipModernizationCheckBox_->setChecked(true);
    optionalCheckBox_->setChecked(true);
    stringViewCheckBox_->setChecked(true);
    applyStringViewWhenSafeCheckBox_->setChecked(false);
    stdFormatCheckBox_->setChecked(false);
    automaticCompileVerificationCheckBox_->setChecked(false);
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
    options.useStdFormatForStreams = stdFormatCheckBox_->isChecked();
    options.compileVerificationEnabled = automaticCompileVerificationCheckBox_->isChecked();
    options.offlineModernizationLevel = levelFromIndex(offlineModernizationLevelComboBox_->currentIndex());
    options.offlineRewriteStyle = offlineRewriteStyleComboBox_->currentIndex() == 1
        ? OfflineRewriteStyle::AggressiveAiLikeRewrite
        : OfflineRewriteStyle::SafeModernization;
    options.customInstruction = customInstructionEdit_->text().toStdString();
    return options;
}

void MainWindow::clearEditors()
{
    inputEditor_->clear();
    outputEditor_->clear();
    detailsEditor_->clear();
    explanationEditor_->clear();
    compileVerificationEditor_->clear();
    if (diagnosticsEditor_ != nullptr) {
        diagnosticsEditor_->clear();
        appendDiagnostic("Editors cleared.");
    }
    if (statusCompileLabel_ != nullptr) {
        statusCompileLabel_->setText("Compile: Not run");
    }
    if (statusSourceLabel_ != nullptr) {
        statusSourceLabel_->setText("Source: None");
    }
    statusBar()->showMessage("Ready");
}

void MainWindow::copyOutputToClipboard()
{
    QApplication::clipboard()->setText(outputEditor_->toPlainText());
    appendDiagnostic("Modernized output copied to clipboard.");
    statusBar()->showMessage("Output copied");
}

void MainWindow::displayResult(const ConversionResult& result)
{
    outputEditor_->setPlainText(QString::fromStdString(result.modernCode));
    detailsEditor_->setPlainText(formatChanges(result));
    explanationEditor_->setPlainText(QString::fromStdString(result.explanation));
    compileVerificationEditor_->setPlainText(formatCompileVerification(result));
    updateStatusBarMetadata(result);
}

void MainWindow::verifyConvertedCode()
{
    appendDiagnostic("Manual compile verification started.");
    const CompileVerificationResult verification = CompileVerifier::verifySyntaxOnly(outputEditor_->toPlainText().toStdString());

    ConversionResult result;
    result.compileVerificationEnabled = verification.verificationEnabled;
    result.compileVerificationPassed = verification.passed;
    result.compilerUsed = verification.compilerUsed;
    result.compilerOutput = verification.output;
    compileVerificationEditor_->setPlainText(formatCompileVerification(result));

    if (!verification.compilerFound) {
        if (statusCompileLabel_ != nullptr) {
            statusCompileLabel_->setText("Compile: Skipped");
        }
        appendDiagnostic("Compile verification skipped because no supported compiler was found.");
        statusBar()->showMessage("Compiler not found");
    } else {
        if (statusCompileLabel_ != nullptr) {
            statusCompileLabel_->setText(verification.passed ? "Compile: Passed" : "Compile: Failed");
        }
        appendDiagnostic(verification.passed ? "Compile verification passed." : "Compile verification failed.");
        statusBar()->showMessage(verification.passed ? "Compile verification passed" : "Compile verification failed");
    }
}

void MainWindow::cloneRepository()
{
    appendDiagnostic("Repository clone started.");
    RepositoryModernizationOptions options;
    options.repositoryUrl = repositoryUrlEdit_->text().trimmed().toStdString();
    options.branch = repositoryBranchEdit_->text().trimmed().toStdString();
    options.outputWorkspaceFolder = repositoryWorkspaceEdit_->text().trimmed().toStdString();
    options.modernizationLevel = levelFromIndex(repositoryModernizationLevelComboBox_->currentIndex());

    RepositoryCloneService cloneService;
    RepositoryCloneResult result = cloneService.cloneRepository(options);
    if (!result.success && result.message == "Target clone folder already exists.") {
        const QMessageBox::StandardButton answer = QMessageBox::question(
            this,
            "Overwrite Existing Clone Folder",
            "The target clone folder already exists. Clone will not overwrite it unless you confirm. Continue?");
        if (answer == QMessageBox::Yes) {
            options.allowOverwrite = true;
            result = cloneService.cloneRepository(options);
        }
    }

    currentRepositoryPath_ = result.success ? result.clonePath : std::filesystem::path{};
    currentRepositoryFiles_.clear();
    currentRepositoryReportFolder_.clear();
    repositoryStatusLabel_->setText(result.success ? "Clone successful" : "Clone failed");
    repositoryReportEditor_->setPlainText(QString::fromStdString(result.message + "\nClone path: " + result.clonePath.string()));
    appendDiagnostic(result.success ? "Repository clone successful." : "Repository clone failed.");
    statusBar()->showMessage(result.success ? "Repository cloned" : "Repository clone failed");
}

void MainWindow::scanRepository()
{
    if (currentRepositoryPath_.empty()) {
        repositoryStatusLabel_->setText("Scan failed");
        repositoryReportEditor_->setPlainText("Clone a repository before scanning.");
        appendDiagnostic("Repository scan failed: no cloned repository selected.");
        return;
    }

    appendDiagnostic("Repository scan started.");
    RepositoryModernizationService service(std::make_unique<RuleBasedConverterEngine>());
    currentRepositoryFiles_ = service.scanFiles(currentRepositoryPath_);
    repositoryStatusLabel_->setText("Scan complete");
    repositoryReportEditor_->setPlainText(QString("Files found: %1\nClone path: %2")
        .arg(static_cast<qulonglong>(currentRepositoryFiles_.size()))
        .arg(QString::fromStdString(currentRepositoryPath_.string())));
    appendDiagnostic(QString("Repository scan complete. Files found: %1").arg(static_cast<qulonglong>(currentRepositoryFiles_.size())));
    statusBar()->showMessage("Repository scan complete");
}

void MainWindow::modernizeRepository()
{
    if (currentRepositoryPath_.empty()) {
        repositoryStatusLabel_->setText("Modernization failed");
        repositoryReportEditor_->setPlainText("Clone a repository before modernization.");
        appendDiagnostic("Repository modernization failed: no cloned repository selected.");
        return;
    }

    appendDiagnostic("Repository modernization started.");
    RepositoryModernizationOptions options;
    options.repositoryUrl = repositoryUrlEdit_->text().trimmed().toStdString();
    options.branch = repositoryBranchEdit_->text().trimmed().toStdString();
    options.outputWorkspaceFolder = repositoryWorkspaceEdit_->text().trimmed().toStdString();
    options.modernizationLevel = levelFromIndex(repositoryModernizationLevelComboBox_->currentIndex());
    options.compileVerificationEnabled = true;

    RepositoryModernizationService service(std::make_unique<RuleBasedConverterEngine>());
    const RepositoryModernizationResult result = service.modernizeRepository(options, currentRepositoryPath_);
    currentRepositoryReportFolder_ = result.clonePath;

    repositoryStatusLabel_->setText("Repository modernization complete");
    repositoryReportEditor_->setPlainText(QString(
        "Files scanned: %1\n"
        "Files modified: %2\n"
        "Applied changes: %3\n"
        "Suggestions: %4\n"
        "Verification: %5\n"
        "Report: %6")
        .arg(static_cast<qulonglong>(result.filesScanned))
        .arg(static_cast<qulonglong>(result.filesModified))
        .arg(static_cast<qulonglong>(result.totalAppliedChanges))
        .arg(static_cast<qulonglong>(result.totalSuggestions))
        .arg(QString::fromStdString(result.verificationSummary))
        .arg(QString::fromStdString(result.textReportPath.string())));
    appendDiagnostic(QString("Repository modernization complete. Files modified: %1. Report: %2")
        .arg(static_cast<qulonglong>(result.filesModified))
        .arg(QString::fromStdString(result.textReportPath.string())));
    statusBar()->showMessage("Repository modernization complete");
}

void MainWindow::openReportFolder()
{
    if (currentRepositoryReportFolder_.empty()) {
        repositoryReportEditor_->setPlainText("No report folder is available yet.");
        appendDiagnostic("Open report folder requested before a report was available.");
        return;
    }
    appendDiagnostic("Opening repository report folder.");
    QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(currentRepositoryReportFolder_.string())));
}

QString MainWindow::formatChanges(const ConversionResult& result) const
{
    QString details;
    details += "Conversion Source:\n";
    details += QString::fromStdString(result.conversionSource.empty() ? "Unknown" : result.conversionSource);
    details += "\n\nBackend Status:\n";
    details += QString::fromStdString(result.backendStatus.empty() ? "Unknown" : result.backendStatus);
    details += "\n\nAI Provider:\n";
    details += QString::fromStdString(result.aiProvider.empty() ? "Not used" : result.aiProvider);
    details += "\n\nModel:\n";
    details += QString::fromStdString(result.aiModel.empty() ? "Not used" : result.aiModel);
    details += "\n\nFallback Used:\n";
    details += result.fallbackUsed ? "true" : "false";
    details += "\n\nConverted At:\n";
    details += QString::fromStdString(result.convertedAt.empty() ? "Unknown" : result.convertedAt);
    details += "\n\n";
    details += "Rewrite Level:\n";
    details += QString::fromStdString(result.rewriteLevel.empty() ? "Standard Rule-Based" : result.rewriteLevel);
    details += "\n\n";

    if (!result.diagnosticMessages.empty()) {
        details += "Diagnostics\n";
        details += "===========\n\n";
        for (const std::string& message : result.diagnosticMessages) {
            details += "- ";
            details += QString::fromStdString(message);
            details += "\n";
        }
        details += "\n";
    }

    details += "Compile Verification:\n";
    if (result.compileVerificationEnabled) {
        details += result.compileVerificationPassed ? "passed" : "failed/skipped";
        details += "\nCompiler:\n";
        details += QString::fromStdString(result.compilerUsed.empty() ? "not found" : result.compilerUsed);
        details += "\nInclude Auto-Fix Attempted:\n";
        details += result.compileVerificationAutoFixAttempted ? "true" : "false";
    } else {
        details += "not run";
    }
    details += "\n\n";

    if (result.changes.empty()) {
        details += "No applied changes, suggestions, or skipped rules.";
        return details;
    }

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

QString MainWindow::formatCompileVerification(const ConversionResult& result) const
{
    if (!result.compileVerificationEnabled) {
        return "Syntax check skipped.";
    }

    QString output;
    if (result.compilerUsed.empty()) {
        output += "Compiler not found\n";
        output += "==================\n\n";
        output += QString::fromStdString(result.compilerOutput.empty() ? "Syntax check skipped because no supported compiler was found." : result.compilerOutput);
        return output;
    }

    output += result.compileVerificationPassed ? "Compile verification passed\n" : "Compile verification failed\n";
    output += "===========================\n\n";
    output += "Compiler Used:\n";
    output += QString::fromStdString(result.compilerUsed);
    output += "\n\nInclude Auto-Fix Attempted:\n";
    output += result.compileVerificationAutoFixAttempted ? "true" : "false";
    output += "\n\nCompiler Output:\n";
    output += QString::fromStdString(result.compilerOutput);
    return output;
}
