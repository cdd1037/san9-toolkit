#include "documents_overlay.h"

#include "documents_path.h"
#include "import_hook.h"

#include <windows.h>

#include <cstring>
#include <string>

namespace san9::documents_overlay {
namespace {

constexpr char kImportedModule[] = "mydoc.dll";
constexpr char kImportedFunction[] = "?GetMyDocumentPath@@YAXPAD@Z";
using GetMyDocumentPathFunction = void(__cdecl*)(char*);

GetMyDocumentPathFunction g_originalGetMyDocumentPath = nullptr;
std::string g_documentsRoot;

void __cdecl RedirectedGetMyDocumentPath(char* destination) {
    if (destination) {
        std::memcpy(destination, g_documentsRoot.c_str(), g_documentsRoot.size() + 1);
    }
}

} // namespace

bool Install(const std::wstring& documentsRoot) {
    if (documentsRoot.empty()) {
        return true;
    }
    if (!documents_path::EncodeRoot(documentsRoot, g_documentsRoot)) {
        return false;
    }
    return import_hook::Install(kImportedModule, kImportedFunction,
                                &RedirectedGetMyDocumentPath, g_originalGetMyDocumentPath);
}

} // namespace san9::documents_overlay
