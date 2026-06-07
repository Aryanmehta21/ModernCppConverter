#pragma once

#include "app/ConversionCoordinator.h"
#include "converter/IConverterEngine.h"
#include "models/ConversionResult.h"
#include "models/ConversionMode.h"
#include "models/ModernizationOptions.h"

#include <memory>
#include <filesystem>
#include <vector>

#include <QMainWindow>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QWidget;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(std::unique_ptr<IConverterEngine> converterEngine, QWidget* parent = nullptr);

private slots:
    void convertCode();
    void clearEditors();
    void copyOutputToClipboard();
    void selectAllOptions();
    void clearAllOptions();
    void applyRecommendedSafeDefaults();
    void checkBackendConnection();
    void verifyConvertedCode();
    void cloneRepository();
    void scanRepository();
    void modernizeRepository();
    void openReportFolder();

private:
    void buildUi();
    void configureScreenAwareWindowSize();
    [[nodiscard]] QWidget* createCodeConverterTab();
    [[nodiscard]] QWidget* createOptionsPanel();
    [[nodiscard]] QWidget* createRepositoryPanel();
    [[nodiscard]] QWidget* createDiagnosticsPanel();
    [[nodiscard]] std::vector<QCheckBox*> allOptionCheckboxes() const;
    [[nodiscard]] ModernizationOptions readModernizationOptions() const;
    [[nodiscard]] ConversionMode readConversionMode() const;
    void updateModeStatus(ConversionMode mode, bool backendUnavailable);
    void updateStatusBarMetadata(const ConversionResult& result);
    void appendDiagnostic(const QString& message);
    void setAllOptions(bool checked);
    void displayResult(const ConversionResult& result);
    [[nodiscard]] QString formatChanges(const ConversionResult& result) const;
    [[nodiscard]] QString formatCompileVerification(const ConversionResult& result) const;

    std::unique_ptr<ConversionCoordinator> conversionCoordinator_;
    QComboBox* conversionModeComboBox_ = nullptr;
    QComboBox* offlineModernizationLevelComboBox_ = nullptr;
    QComboBox* offlineRewriteStyleComboBox_ = nullptr;
    QLabel* modeStatusLabel_ = nullptr;
    QLabel* statusModeLabel_ = nullptr;
    QLabel* statusBackendLabel_ = nullptr;
    QLabel* statusCompileLabel_ = nullptr;
    QLabel* statusSourceLabel_ = nullptr;
    QCheckBox* nullptrCheckBox_ = nullptr;
    QCheckBox* usingAliasesCheckBox_ = nullptr;
    QCheckBox* autoCheckBox_ = nullptr;
    QCheckBox* rangeBasedForCheckBox_ = nullptr;
    QCheckBox* lambdasCheckBox_ = nullptr;
    QCheckBox* overrideFinalCheckBox_ = nullptr;
    QCheckBox* constexprCheckBox_ = nullptr;
    QCheckBox* smartPointersCheckBox_ = nullptr;
    QCheckBox* moveSemanticsCheckBox_ = nullptr;
    QCheckBox* enumClassCheckBox_ = nullptr;
    QCheckBox* genericLambdasCheckBox_ = nullptr;
    QCheckBox* makeUniqueCheckBox_ = nullptr;
    QCheckBox* applySafeOwnershipModernizationCheckBox_ = nullptr;
    QCheckBox* structuredBindingsCheckBox_ = nullptr;
    QCheckBox* ifConstexprCheckBox_ = nullptr;
    QCheckBox* optionalCheckBox_ = nullptr;
    QCheckBox* variantCheckBox_ = nullptr;
    QCheckBox* stringViewCheckBox_ = nullptr;
    QCheckBox* applyStringViewWhenSafeCheckBox_ = nullptr;
    QCheckBox* inlineVariablesCheckBox_ = nullptr;
    QCheckBox* conceptsCheckBox_ = nullptr;
    QCheckBox* rangesCheckBox_ = nullptr;
    QCheckBox* spanCheckBox_ = nullptr;
    QCheckBox* designatedInitializersCheckBox_ = nullptr;
    QCheckBox* constevalConstinitCheckBox_ = nullptr;
    QCheckBox* spaceshipOperatorCheckBox_ = nullptr;
    QCheckBox* stdFormatCheckBox_ = nullptr;
    QCheckBox* automaticCompileVerificationCheckBox_ = nullptr;
    QLineEdit* customInstructionEdit_ = nullptr;
    QPlainTextEdit* inputEditor_ = nullptr;
    QPlainTextEdit* outputEditor_ = nullptr;
    QPlainTextEdit* detailsEditor_ = nullptr;
    QPlainTextEdit* explanationEditor_ = nullptr;
    QPlainTextEdit* compileVerificationEditor_ = nullptr;
    QPlainTextEdit* diagnosticsEditor_ = nullptr;
    QLineEdit* repositoryUrlEdit_ = nullptr;
    QLineEdit* repositoryBranchEdit_ = nullptr;
    QLineEdit* repositoryWorkspaceEdit_ = nullptr;
    QComboBox* repositoryModernizationLevelComboBox_ = nullptr;
    QLabel* repositoryStatusLabel_ = nullptr;
    QPlainTextEdit* repositoryReportEditor_ = nullptr;
    std::filesystem::path currentRepositoryPath_;
    std::filesystem::path currentRepositoryReportFolder_;
    std::vector<std::filesystem::path> currentRepositoryFiles_;
};
