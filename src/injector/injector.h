#pragma once

#include <Windows.h> // for LPTHREAD_START_ROUTINE, DWORD, HANDLE, LPVOID
#include <optional>  // for optional
#include <stddef.h>  // for size_t
#include <string>    // for string

struct MemoryMap
{
    size_t addr;
    std::string name;
};

struct ProcessInfo
{
    std::string name;
    DWORD pid;
};

struct Process
{
    HANDLE handle;
    ProcessInfo info;
};

void inject_dll(const Process& proc, const std::string& name);
LPTHREAD_START_ROUTINE find_function(const Process& proc, const std::string& library, const std::string& function);
void call(const Process& proc, LPTHREAD_START_ROUTINE addr, LPVOID args);
std::optional<Process> find_process(std::string name);