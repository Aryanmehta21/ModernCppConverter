#include <iostream>
#include <vector>

bool isSmall(int value)
{
    return value < 10;
}

void printSmallValues(const std::vector<int>& values)
{
    for (int value : values)
    {
        if (isSmall(value))
        {
            std::cout << value << std::endl;
        }
    }
}
