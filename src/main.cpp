#include "app/MainWindow.h"
#include "converter/RuleBasedConverterEngine.h"

#include <QApplication>
#include <memory>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

    MainWindow window(std::make_unique<RuleBasedConverterEngine>());
    window.show();

    return app.exec();
}
