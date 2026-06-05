#pragma once

#include "app/ConversionCoordinator.h"
#include "converter/IConverterEngine.h"
#include "models/ConversionResult.h"
#include "models/ConversionMode.h"
#include "models/ModernizationOptions.h"

#include <memory>
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

private:
    void buildUi();
    [[nodiscard]] QWidget* createOptionsPanel();
    [[nodiscard]] std::vector<QCheckBox*> allOptionCheckboxes() const;
    [[nodiscard]] ModernizationOptions readModernizationOptions() const;
    [[nodiscard]] ConversionMode readConversionMode() const;
    void updateModeStatus(ConversionMode mode, bool backendUnavailable);
    void setAllOptions(bool checked);
    void displayResult(const ConversionResult& result);
    [[nodiscard]] QString formatChanges(const ConversionResult& result) const;

    std::unique_ptr<ConversionCoordinator> conversionCoordinator_;
    QComboBox* conversionModeComboBox_ = nullptr;
    QLabel* modeStatusLabel_ = nullptr;
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
    QLineEdit* customInstructionEdit_ = nullptr;
    QPlainTextEdit* inputEditor_ = nullptr;
    QPlainTextEdit* outputEditor_ = nullptr;
    QPlainTextEdit* detailsEditor_ = nullptr;
    QPlainTextEdit* explanationEditor_ = nullptr;
};
