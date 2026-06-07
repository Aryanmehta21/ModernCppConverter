#include <iostream>
#include <vector>

void printValues(std::vector<int>& values)
{
    for (std::vector<int>::iterator it = values.begin(); it != values.end(); ++it)
    {
        std::cout << *it << std::endl;
    }

    for (int i = 0; i < values.size(); ++i)
    {
        std::cout << values[i] << std::endl;
    }
}
