#include "native_bridge.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <iostream>
#include <vector>

namespace
{
bool verifyEnumeration(int(__cdecl* enumerate)(wchar_t*, int), const char* label)
{
    const int required = enumerate(nullptr, 0);
    if (required <= 1) {
        std::cerr << label << " did not report a valid JSON buffer size\n";
        return false;
    }

    std::vector<wchar_t> buffer(static_cast<std::size_t>(required));
    if (enumerate(buffer.data(), static_cast<int>(buffer.size())) != required) {
        std::cerr << label << " changed its required buffer size\n";
        return false;
    }

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(
        QString::fromWCharArray(buffer.data()).toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        std::cerr << label << " returned invalid JSON\n";
        return false;
    }

    const QJsonObject object = document.object();
    if (!object.value(QStringLiteral("devices")).isArray()
        || !object.value(QStringLiteral("error")).isString()) {
        std::cerr << label << " returned an incomplete response\n";
        return false;
    }
    return true;
}
}

int main()
{
    void* handle = VS_Create(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    if (handle == nullptr) {
        std::cerr << "VS_Create failed\n";
        return 1;
    }

    const bool renderValid = verifyEnumeration(&VS_GetRenderDevicesJson, "render enumeration");
    const bool captureValid = verifyEnumeration(&VS_GetCaptureDevicesJson, "capture enumeration");
    VS_Destroy(handle);
    return renderValid && captureValid ? 0 : 1;
}
