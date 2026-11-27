#pragma once
#include <windows.h>
#include <cstdint>

namespace grml::webview_host {

int runWindow(HINSTANCE hInstance, uint16_t serverPort);

} // namespace grml::webview_host
