#include "app/MainWindow.h"
#include "converter/RuleBasedConverterEngine.h"
#include "utils/AppVersion.h"
#include "utils/CrashBreadcrumb.h"

#include <QApplication>
#include <QDebug>
#include <memory>

int main(int argc, char* argv[])
{
    CrashBreadcrumb::installSignalHandlers();
    CrashBreadcrumb::ScopedStage startupStage("app startup");
    QApplication app(argc, argv);
    QApplication::setApplicationName(QString::fromStdString(AppVersion::windowTitle()));
    QApplication::setApplicationVersion(QString::fromStdString(AppVersion::version()));
    qInfo().noquote() << QString::fromStdString(AppVersion::startupLogLine());

    MainWindow window(std::make_unique<RuleBasedConverterEngine>());
    window.show();

    return app.exec();
}
