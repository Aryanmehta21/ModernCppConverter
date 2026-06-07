#include <iostream>
#include <vector>

void printAll(std::vector<int>& values)
{
    for (std::vector<int>::iterator it = values.begin(); it != values.end(); ++it)
    {
        std::cout << *it << std::endl;
    }
}
