# Sample Legacy Inputs

These snippets are useful for manual smoke testing and demonstrations. They are intentionally small and focus on categories the offline converter already handles conservatively.

## 1. Raw Dynamic Array Buffer

```cpp
#include <iostream>

#define DEFAULT_CAPACITY 2

struct Sample {
    int id;
};

class SampleStore {
    Sample* records;
    int count;
    int capacity;

public:
    SampleStore() {
        count = 0;
        capacity = DEFAULT_CAPACITY;
        records = new Sample[capacity];
    }

    ~SampleStore() {
        delete[] records;
    }

    void add(int id) {
        if (count >= capacity) {
            int newCapacity = capacity * 2;
            Sample* temp = new Sample[newCapacity];
            for (int i = 0; i < count; ++i) {
                temp[i] = records[i];
            }
            delete[] records;
            records = temp;
            capacity = newCapacity;
        }

        records[count].id = id;
        ++count;
    }

    int size() const {
        return count;
    }
};
```

Expected modernization categories:

- macro constant to `constexpr`
- raw dynamic array to `std::vector<Sample>`
- manual growth cleanup
- `push_back` append logic
- cleanup-only destructor removal
- count getter using `records.size()`

## 2. Owned C String Buffer

```cpp
#include <cstring>

class Label {
    char* text;

public:
    explicit Label(const char* input) {
        text = new char[std::strlen(input) + 1];
        std::strcpy(text, input);
    }

    ~Label() {
        delete[] text;
    }

    void append(const char* suffix) {
        char* combined = new char[std::strlen(text) + std::strlen(suffix) + 1];
        std::strcpy(combined, text);
        std::strcat(combined, suffix);
        delete[] text;
        text = combined;
    }

    const char* c_str() const {
        return text;
    }
};
```

Expected modernization categories:

- owned `char*` to `std::string`
- `strcpy` assignment cleanup
- `strcat` append cleanup
- `strlen` to `.size()` where relevant
- `c_str()` compatibility getter
- Rule of Zero cleanup when all raw ownership is removed

## 3. Raw Owner With Observer Alias

```cpp
struct Resource {
    void use() {}
};

void observe(Resource* value) {}

void run() {
    Resource* owner = new Resource();
    Resource* alias = owner;

    observe(owner);
    observe(alias);
    owner->use();

    delete owner;
}
```

Expected modernization categories:

- clear owner to `std::unique_ptr<Resource>`
- construction with `std::make_unique<Resource>()`
- raw observer alias preserved as `Resource*`
- owner passed to raw sink with `owner.get()`
- manual `delete` removal

## 4. Scoped Enum Output

```cpp
#include <iostream>

enum State {
    Idle,
    Running
};

State current() {
    return Running;
}

void report(State state) {
    if (state == Idle) {
        state = Running;
    }

    std::cout << state << '\n';
}
```

Expected modernization categories:

- `enum` to `enum class`
- scoped enumerator use such as `State::Running`
- stream output cast with `std::underlying_type_t<State>`
- `<type_traits>` include when needed

## 5. FILE* Text Write And Map Printing

```cpp
#include <cstdio>
#include <iostream>
#include <map>
#include <string>

template <class T>
void printValues(const T& values) {
    for (typename T::const_iterator it = values.begin(); it != values.end(); ++it) {
        std::cout << "Value: " << *it << '\n';
    }
}

void saveAndPrint(const char* path) {
    FILE* file = fopen(path, "w");
    if (file != NULL) {
        fprintf(file, "ok\n");
    }
    fclose(file);

    std::map<int, std::string> names;
    printValues(names);
}
```

Expected modernization categories:

- simple `FILE*` write to `std::ofstream`
- stream truthiness check instead of `file != NULL`
- `fprintf` to stream insertion
- `fclose` removal
- generic map print safety so `std::pair` is not streamed directly
- pair-aware key/value formatting or structured binding where safe
