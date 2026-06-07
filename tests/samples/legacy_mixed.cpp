#define NULL 0
#ifndef nullptr
#define nullptr NULL
#endif

#include <cstring>
#include <iostream>
#include <vector>

typedef unsigned long Size;

struct Doubler { int operator()(int value) const { return value * 2; } };

class Base
{
public:
    virtual void run();
};

class Derived : public Base
{
public:
    void run();
};

const int MaxCount = 10;

void process(const char* input, std::vector<int>& values)
{
    char name[50];
    std::strncpy(name, input, sizeof(name));

    for (int i = 0; i < values.size(); ++i)
    {
        std::cout << values[i] << std::endl;
    }

    int casted = (int)values.size();
}
