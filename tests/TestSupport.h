#pragma once

#include <cstdlib>
#include <exception>
#include <format>
#include <iostream>
#include <stdexcept>

namespace desto::test {

inline void Check(bool condition, const char* expression, const char* file, int line) {
    if (!condition) {
        throw std::runtime_error(std::format("{}:{} check failed: {}", file, line, expression));
    }
}

template <typename Test>
int Run(Test&& test) noexcept {
    try {
        test();
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
    } catch (...) {
        std::cerr << "Unknown test failure.\n";
    }
    return EXIT_FAILURE;
}

} // namespace desto::test

#define DESTO_CHECK(expression) \
    ::desto::test::Check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)
