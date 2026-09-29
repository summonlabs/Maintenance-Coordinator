// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.
//
// A deliberately small test harness: no external dependency, deterministic
// ordering (registration order), no hidden global state beyond the registry,
// and one process per test executable so that a crash is a test failure rather
// than a cascade.

#ifndef MC_TEST_HPP
#define MC_TEST_HPP

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "mc/plan.hpp"
#include "mc/status.hpp"

namespace mc::test {

using TestFn = void (*)();

struct Case {
    const char* name;
    TestFn function;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failure_count() {
    static int count = 0;
    return count;
}

inline int& check_count() {
    static int count = 0;
    return count;
}

inline const char*& current_case() {
    static const char* name = "";
    return name;
}

struct Registrar {
    Registrar(const char* name, TestFn function) { registry().push_back(Case{name, function}); }
};

// Thrown by MC_REQUIRE to abandon the current case without running the rest of
// it; the failure is already recorded.
struct AbortCase : std::exception {
    [[nodiscard]] const char* what() const noexcept override { return "case aborted"; }
};

template <typename T, typename = void>
struct is_streamable : std::false_type {};

template <typename T>
struct is_streamable<T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>>
    : std::true_type {};

// Accepts either a Status or a Result<T> so that the macros read the same at
// every call site.
[[nodiscard]] inline const Status& status_of(const Status& status) noexcept { return status; }

template <typename T>
[[nodiscard]] const Status& status_of(const Result<T>& result) noexcept {
    return result.status();
}

template <typename T>
[[nodiscard]] std::string display(const T& value) {
    if constexpr (is_streamable<T>::value) {
        std::ostringstream stream;
        stream << value;
        return stream.str();
    } else {
        return "<value>";
    }
}

inline void record_failure(const char* file, int line, const std::string& text) {
    ++failure_count();
    std::cout << "  FAIL " << current_case() << " (" << file << ":" << line << "): " << text << '\n';
}

inline int run_all(int argc, char** argv) {
    std::string filter;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--filter" && index + 1 < argc) {
            filter = argv[index + 1];
        }
    }
    int passed = 0;
    int skipped = 0;
    for (const auto& test : registry()) {
        if (!filter.empty() && std::string(test.name).find(filter) == std::string::npos) {
            ++skipped;
            continue;
        }
        current_case() = test.name;
        const int before = failure_count();
        try {
            test.function();
        } catch (const AbortCase&) {
            // Failure already recorded.
        } catch (const std::exception& error) {
            record_failure(__FILE__, __LINE__, std::string("unhandled exception: ") + error.what());
        } catch (...) {
            record_failure(__FILE__, __LINE__, "unhandled non-standard exception");
        }
        if (failure_count() == before) {
            ++passed;
        }
    }
    std::cout << (failure_count() == 0 ? "PASS" : "FAIL") << ": " << passed << " cases passed, " << failure_count()
              << " checks failed, " << check_count() << " checks run, " << skipped << " skipped\n";
    return failure_count() == 0 ? 0 : 1;
}

// Deterministic temporary directory: the same tag maps to the same directory
// inside the test process, and every test that needs isolation asks for its own
// tag.  Nothing is written outside the build tree's temporary area.
[[nodiscard]] inline std::filesystem::path temp_root() {
    const auto base = std::filesystem::temp_directory_path() / "mc-tests";
    std::filesystem::create_directories(base);
    return base;
}

// The process id is part of every temporary path: two test processes running at
// the same time - a developer's run and a continuous-integration run, say - must
// never share a store directory, because they would then fight over the
// single-writer lock and over each other's files.
[[nodiscard]] inline unsigned long process_identity() noexcept {
#if defined(_WIN32)
    return static_cast<unsigned long>(::_getpid());
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

[[nodiscard]] inline std::filesystem::path make_temp_dir(const std::string& tag) {
    const auto path = temp_root() / (tag + "-p" + std::to_string(process_identity()));
    std::error_code error;
    std::filesystem::remove_all(path, error);
    std::filesystem::create_directories(path);
    return path;
}

inline void remove_tree(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove_all(path, error);
}

[[nodiscard]] inline std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

inline void write_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << text;
}

// Deterministic pseudo random generator for property tests.  A fixed algorithm
// means a seed reproduced on another day or another machine gives the same
// sequence, which is what makes a printed counterexample useful.
class Random {
public:
    explicit Random(std::uint64_t seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ULL : seed) {}

    [[nodiscard]] std::uint64_t next_u64() {
        // SplitMix64.
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t value = state_;
        value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
        return value ^ (value >> 31U);
    }

    [[nodiscard]] std::uint64_t next_below(std::uint64_t bound) {
        return bound == 0 ? 0 : next_u64() % bound;
    }

    [[nodiscard]] bool next_bool() { return (next_u64() & 1U) != 0U; }

private:
    std::uint64_t state_;
};

}  // namespace mc::test

#define MC_TEST(name)                                                              \
    static void name();                                                            \
    static const ::mc::test::Registrar name##_registrar(#name, &name);             \
    static void name()

#define MC_CHECK(expression)                                                                                \
    do {                                                                                                    \
        ++::mc::test::check_count();                                                                        \
        if (!(expression)) {                                                                                \
            ::mc::test::record_failure(__FILE__, __LINE__, "expected: " #expression);                       \
        }                                                                                                   \
    } while (false)

#define MC_CHECK_MSG(expression, message)                                                                   \
    do {                                                                                                    \
        ++::mc::test::check_count();                                                                        \
        if (!(expression)) {                                                                                \
            ::mc::test::record_failure(__FILE__, __LINE__,                                                  \
                                       std::string("expected: " #expression " -- ") + (message));           \
        }                                                                                                   \
    } while (false)

#define MC_CHECK_EQ(actual, expected)                                                                       \
    do {                                                                                                    \
        ++::mc::test::check_count();                                                                        \
        const auto& mc_actual_value = (actual);                                                             \
        const auto& mc_expected_value = (expected);                                                         \
        if (!(mc_actual_value == mc_expected_value)) {                                                      \
            ::mc::test::record_failure(__FILE__, __LINE__,                                                  \
                                       std::string(#actual " == " #expected "\n         actual: ") +       \
                                           ::mc::test::display(mc_actual_value) +                       \
                                           "\n       expected: " + ::mc::test::display(mc_expected_value)); \
        }                                                                                                   \
    } while (false)

#define MC_REQUIRE(expression)                                                                              \
    do {                                                                                                    \
        ++::mc::test::check_count();                                                                        \
        if (!(expression)) {                                                                                \
            ::mc::test::record_failure(__FILE__, __LINE__, "required: " #expression);                       \
            throw ::mc::test::AbortCase{};                                                                  \
        }                                                                                                   \
    } while (false)

#define MC_REQUIRE_OK(expression)                                                                           \
    do {                                                                                                    \
        ++::mc::test::check_count();                                                                        \
        const ::mc::Status mc_status_value = ::mc::test::status_of(expression);                                    \
        if (!mc_status_value.ok()) {                                                                        \
            ::mc::test::record_failure(__FILE__, __LINE__,                                                  \
                                       std::string("expected success from " #expression ", got ") +         \
                                           std::string(mc_status_value.render()));                          \
            throw ::mc::test::AbortCase{};                                                                  \
        }                                                                                                   \
    } while (false)

#define MC_CHECK_CODE(expression, expected_code)                                                            \
    do {                                                                                                    \
        ++::mc::test::check_count();                                                                        \
        const ::mc::Status mc_status_value = ::mc::test::status_of(expression);                                    \
        if (mc_status_value.code() != (expected_code)) {                                                    \
            ::mc::test::record_failure(                                                                     \
                __FILE__, __LINE__,                                                                         \
                std::string(#expression " expected code " #expected_code ", got ") +                        \
                    std::string(::mc::to_string(mc_status_value.code())) + " -- " +                         \
                    std::string(mc_status_value.render()));                                                 \
        }                                                                                                   \
    } while (false)

#define MC_TEST_MAIN()                                                                                      \
    int main(int argc, char** argv) { return ::mc::test::run_all(argc, argv); }

namespace mc {

inline std::ostream& operator<<(std::ostream& stream, const Status& status) { return stream << status.render(); }

inline std::ostream& operator<<(std::ostream& stream, const Digest& digest) { return stream << digest.hex(); }

inline std::ostream& operator<<(std::ostream& stream, PlanPhase phase) { return stream << to_string(phase); }

}  // namespace mc

#endif  // MC_TEST_HPP
