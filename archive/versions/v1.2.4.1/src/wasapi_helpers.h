#pragma once

#include <Windows.h>
#include <audioclient.h>
#include <avrt.h>

#include <QString>

#include <stdexcept>
#include <string>
#include <vector>

class ComInitializer
{
public:
    explicit ComInitializer(DWORD apartmentType);
    ~ComInitializer();

    ComInitializer(const ComInitializer&) = delete;
    ComInitializer& operator=(const ComInitializer&) = delete;

private:
    bool shouldUninitialize_ = false;
};

class UniqueHandle
{
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle);
    ~UniqueHandle();

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept;
    UniqueHandle& operator=(UniqueHandle&& other) noexcept;

    HANDLE get() const;
    explicit operator bool() const;

private:
    void reset();
    HANDLE handle_ = nullptr;
};

class MmcssRegistration
{
public:
    MmcssRegistration();
    ~MmcssRegistration();

    MmcssRegistration(const MmcssRegistration&) = delete;
    MmcssRegistration& operator=(const MmcssRegistration&) = delete;

private:
    HANDLE handle_ = nullptr;
};

void checkHresult(HRESULT result, const char* operation);
QString hresultToQString(HRESULT result);
std::wstring toWideString(const QString& value);
std::vector<BYTE> copyWaveFormat(const WAVEFORMATEX* format);

