#define NULL 0

class Widget {};

void createWidget()
{
    Widget* widget = new Widget();
    delete widget;
}
