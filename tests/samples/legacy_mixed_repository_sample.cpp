#define NULL 0

#include <cstring>
#include <iostream>
#include <vector>

typedef unsigned long Size;

class Tool {};

bool isVisible(int value)
{
    return value > 3;
}

void process(const char* input, std::vector<int>& values)
{
    Tool* tool = new Tool();
    delete tool;

    char name[80];
    std::strncpy(name, input, sizeof(name));

    for (int value : values)
    {
        if (isVisible(value))
        {
            std::cout << value << std::endl;
        }
    }
}
