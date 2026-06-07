#include <cstring>

void copyName(const char* input)
{
    char name[50];
    std::strncpy(name, input, sizeof(name));
}
