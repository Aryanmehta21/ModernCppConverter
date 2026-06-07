#include <iostream>

class DiagnosticTool
{
public:
    void run() {}
};

void process()
{
    DiagnosticTool* tool = new DiagnosticTool();
    tool->run();
    delete tool;
}
