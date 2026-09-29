// Maintenance Coordinator - DCCP Physical Fleet Lifecycle
// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0.
//
// Real child processes for the multiprocess proofs.
//
// Claims about cross-process exclusion, crash recovery and concurrent readers
// are proven with independent operating-system processes, never with threads.

#ifndef MC_TEST_PROCESS_HPP
#define MC_TEST_PROCESS_HPP

#if !defined(_WIN32)
#error "The Maintenance Coordinator multiprocess tests are implemented for Windows."
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace mc::test {

[[nodiscard]] inline std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return std::wstring{};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

class Child {
public:
    Child() = default;
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    Child(Child&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
    Child& operator=(Child&& other) noexcept {
        if (this != &other) {
            close();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }
    ~Child() { close(); }

    // Windows command line quoting: backslashes are doubled only when they
    // precede a quote, which is the rule the C runtime documents.
    [[nodiscard]] static std::wstring quote(const std::wstring& argument) {
        std::wstring out;
        out.push_back(L'"');
        std::size_t backslashes = 0;
        for (const wchar_t character : argument) {
            if (character == L'\\') {
                ++backslashes;
                continue;
            }
            if (character == L'"') {
                out.append((backslashes * 2U) + 1U, L'\\');
                out.push_back(L'"');
                backslashes = 0;
                continue;
            }
            out.append(backslashes, L'\\');
            backslashes = 0;
            out.push_back(character);
        }
        out.append(backslashes * 2U, L'\\');
        out.push_back(L'"');
        return out;
    }

    // Spawns without a shell.  The command line is built explicitly rather than
    // handed to a runtime helper, so the argument vector is exactly what the
    // child sees - the tests depend on it.
    [[nodiscard]] static Child spawn(const std::string& program, const std::vector<std::string>& arguments) {
        const std::wstring wide_program = widen(program);
        std::wstring command_line = quote(wide_program);
        for (const auto& argument : arguments) {
            command_line.push_back(L' ');
            command_line += quote(widen(argument));
        }
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (CreateProcessW(wide_program.c_str(), command_line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                           &startup, &process) == 0) {
            return Child{};
        }
        CloseHandle(process.hThread);
        Child child;
        child.handle_ = process.hProcess;
        return child;
    }

    // Spawns with the child's stdout and stderr redirected to a file, so a test
    // can assert on exactly what a command line tool printed.
    [[nodiscard]] static Child spawn_capturing(const std::string& program,
                                               const std::vector<std::string>& arguments,
                                               const std::string& output_path) {
        const std::wstring wide_program = widen(program);
        std::wstring command_line = quote(wide_program);
        for (const auto& argument : arguments) {
            command_line.push_back(L' ');
            command_line += quote(widen(argument));
        }
        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength = sizeof(attributes);
        attributes.bInheritHandle = TRUE;
        const std::wstring wide_output = widen(output_path);
        HANDLE output = CreateFileW(wide_output.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        if (output == INVALID_HANDLE_VALUE) {
            return Child{};
        }
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = output;
        startup.hStdError = output;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION process{};
        const BOOL created = CreateProcessW(wide_program.c_str(), command_line.data(), nullptr, nullptr, TRUE, 0,
                                            nullptr, nullptr, &startup, &process);
        CloseHandle(output);
        if (created == 0) {
            return Child{};
        }
        CloseHandle(process.hThread);
        Child child;
        child.handle_ = process.hProcess;
        return child;
    }

    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }

    [[nodiscard]] bool exited(std::uint32_t& exit_code) const {
        if (handle_ == nullptr) {
            return true;
        }
        DWORD code = 0;
        if (WaitForSingleObject(handle_, 0) != WAIT_OBJECT_0) {
            return false;
        }
        if (GetExitCodeProcess(handle_, &code) == 0) {
            return false;
        }
        exit_code = code;
        return true;
    }

    [[nodiscard]] std::uint32_t wait() const {
        if (handle_ == nullptr) {
            return 0;
        }
        WaitForSingleObject(handle_, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(handle_, &code);
        return code;
    }

    [[nodiscard]] bool wait_for(std::uint32_t milliseconds, std::uint32_t& exit_code) const {
        if (handle_ == nullptr) {
            exit_code = 0;
            return true;
        }
        if (WaitForSingleObject(handle_, milliseconds) != WAIT_OBJECT_0) {
            return false;
        }
        DWORD code = 0;
        GetExitCodeProcess(handle_, &code);
        exit_code = code;
        return true;
    }

    void kill() const {
        if (handle_ != nullptr) {
            TerminateProcess(handle_, 99U);
        }
    }

private:
    void close() {
        if (handle_ != nullptr) {
            CloseHandle(handle_);
            handle_ = nullptr;
        }
    }

    HANDLE handle_{nullptr};
};

}  // namespace mc::test

#endif  // MC_TEST_PROCESS_HPP
