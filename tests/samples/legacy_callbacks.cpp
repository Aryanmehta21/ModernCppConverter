#include <algorithm>
#include <vector>

struct IsOdd
{
    bool operator()(int value) const
    {
        return value % 2 != 0;
    }
};

int countOdd(std::vector<int>& values)
{
    return std::count_if(values.begin(), values.end(), IsOdd());
}
