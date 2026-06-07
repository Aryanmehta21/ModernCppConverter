#include <algorithm>
#include <vector>

struct IsPositive
{
    bool operator()(int value) const
    {
        return value > 0;
    }
};

int countPositive(std::vector<int>& values)
{
    return std::count_if(values.begin(), values.end(), IsPositive());
}
