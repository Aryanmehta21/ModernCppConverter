#define NULL 0

#include <cstring>
#include <iostream>
#include <vector>

typedef unsigned long Size;

struct IsPositive
{
    bool operator()(int value) const
    {
        return value > 0;
    }
};

bool isPalindrome(long long x)
{
    long long original{x};
    long long temp{0};
    while (x > 0)
    {
        temp = temp * 10 + x % 10;
        x /= 10;
    }
    return original == temp;
}

bool isEven(int value)
{
    return value % 2 == 0;
}

class Tool {};

void run(const char* input, std::vector<int>& values)
{
    Tool* tool = new Tool();
    delete tool;

    char name[50];
    std::strncpy(name, input, sizeof(name));

    for (std::vector<int>::iterator it = values.begin(); it != values.end(); ++it)
    {
        std::cout << *it << std::endl;
    }

    std::count_if(values.begin(), values.end(), IsPositive());
}

void printEvenNumbers(const std::vector<int>& values)
{
    for (int value : values)
    {
        if (isEven(value))
        {
            std::cout << value << std::endl;
        }
    }
}
