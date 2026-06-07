#include "converter/CompileVerifier.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

namespace
{
QString findCompiler()
{
    const QStringList candidates{
        QStringLiteral("g++"),
        QStringLiteral("clang++"),
        QStringLiteral("cl"),
    };

    for (const QString& candidate : candidates) {
        const QString found = QStandardPaths::findExecutable(candidate);
        if (!found.isEmpty()) {
            return found;
        }
    }
    return {};
}

QString standardFlag(CppStandard standard)
{
    return standard == CppStandard::Cpp17 ? QStringLiteral("c++17") : QStringLiteral("c++20");
}

QStringList argumentsForCompiler(const QString& compiler, const QString& sourcePath, CppStandard standard)
{
    const QString executable = QFileInfo(compiler).fileName().toLower();
    if (executable == QStringLiteral("cl") || executable == QStringLiteral("cl.exe")) {
        return {standard == CppStandard::Cpp17 ? QStringLiteral("/std:c++17") : QStringLiteral("/std:c++20"),
                QStringLiteral("/EHsc"),
                QStringLiteral("/nologo"),
                QStringLiteral("/Zs"),
                sourcePath};
    }
    return {QStringLiteral("-std=%1").arg(standardFlag(standard)),
            QStringLiteral("-Wall"),
            QStringLiteral("-Wextra"),
            QStringLiteral("-pedantic"),
            QStringLiteral("-fsyntax-only"),
            sourcePath};
}
} // namespace

CompileVerificationResult CompileVerifier::verifySyntaxOnly(const std::string& code)
{
    return verifySyntaxOnly(code, CppStandard::Cpp20);
}

CompileVerificationResult CompileVerifier::verifySyntaxOnly(const std::string& code, CppStandard standard)
{
    CompileVerificationResult result;
    result.verificationEnabled = true;

    const QString compiler = findCompiler();
    if (compiler.isEmpty()) {
        result.output = "Compiler not found. Syntax check skipped.";
        return result;
    }

    result.compilerFound = true;
    result.compilerUsed = compiler.toStdString();

    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        result.output = "Could not create a temporary directory for syntax verification.";
        return result;
    }

    const QString sourcePath = tempDir.filePath(QStringLiteral("converted.cpp"));
    QFile sourceFile(sourcePath);
    if (!sourceFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        result.output = "Could not create a temporary source file for syntax verification.";
        return result;
    }

    sourceFile.write(QByteArray::fromStdString(code));
    sourceFile.close();

    QProcess process;
    process.setProgram(compiler);
    process.setArguments(argumentsForCompiler(compiler, sourcePath, standard));
    process.start();

    if (!process.waitForStarted(5000)) {
        result.output = "Compiler process could not be started.";
        return result;
    }

    if (!process.waitForFinished(30000)) {
        process.kill();
        process.waitForFinished();
        result.output = "Compile verification timed out.";
        return result;
    }

    const QString output = QString::fromUtf8(process.readAllStandardError())
        + QString::fromUtf8(process.readAllStandardOutput());
    result.passed = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    result.output = output.trimmed().isEmpty()
        ? (result.passed ? "Compile verification passed." : "Compile verification failed with no compiler output.")
        : output.trimmed().toStdString();
    return result;
}
