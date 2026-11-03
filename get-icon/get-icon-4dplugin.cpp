#include "4DPluginAPI.h"
#include "icon.h"

#include <cstdio>
#include <limits>
#include <new>
#include <stdexcept>

namespace {
struct Variable {
    PA_Variable value;
    explicit Variable(PA_Variable v) : value(v) {}
    ~Variable() { PA_ClearVariable(&value); }
    Variable(const Variable&) = delete;
    Variable& operator=(const Variable&) = delete;
};

struct Key {
    PA_Unistring value;
    explicit Key(const char16_t* text)
        : value(PA_CreateUnistring(reinterpret_cast<PA_Unichar*>(const_cast<char16_t*>(text)))) {}
    ~Key() { PA_DisposeUnistring(&value); }
};

bool readPath(PA_ObjectRef object, const char16_t* name, std::u16string& path) {
    Key key(name);
    Variable property(PA_GetObjectProperty(object, &key.value));
    if (PA_GetLastError() != eER_NoErr ||
        PA_GetVariableKind(property.value) != eVK_Unistring)
        return false;
    PA_Unistring string = PA_GetStringVariable(property.value);
    if (PA_GetLastError() != eER_NoErr || string.fLength <= 0 || !string.fString)
        return false;
    path.assign(reinterpret_cast<const char16_t*>(string.fString), string.fLength);
    return path.find(u'\0') == std::u16string::npos;
}

bool objectPath(PA_ObjectRef object, icons::Kind kind, std::u16string& path) {
    if (!object)
        return false;
    Key kindKey(kind == icons::Kind::file ? u"isFile" : u"isFolder");
    Variable objectKind(PA_GetObjectProperty(object, &kindKey.value));
    if (PA_GetLastError() != eER_NoErr ||
        PA_GetVariableKind(objectKind.value) != eVK_Boolean ||
        !PA_GetBooleanVariable(objectKind.value) ||
        !readPath(object, u"platformPath", path))
        return false;
    Variable argument(PA_CreateVariable(eVK_Unistring));
    Key ownedPath(path.c_str());
    PA_SetStringVariable(&argument.value, &ownedPath.value);
    // SetStringVariable transfers the string; only the variable may clear it.
    ownedPath.value = {};
    Variable format(PA_CreateVariable(eVK_Longint));
    PA_SetLongintVariable(&format.value, 1); // fk platform path
    PA_Variable arguments[] = {argument.value, format.value};
    Variable reconstructed(PA_ExecuteCommandByID(
        kind == icons::Kind::file ? 1566 : 1567, arguments, 2));
    if (PA_GetLastError() != eER_NoErr ||
        PA_GetVariableKind(reconstructed.value) != eVK_Object)
        return false;
    PA_ObjectRef nativeObject = PA_GetObjectVariable(reconstructed.value);
    if (!nativeObject)
        return false;
#ifdef __APPLE__
    return readPath(nativeObject, u"path", path);
#else
    return readPath(nativeObject, u"platformPath", path);
#endif
}

struct Request {
    const std::u16string& path;
    std::int32_t size;
    icons::Kind kind;
    icons::Result result;
};

void nativeLoad(void* context) {
    auto& request = *static_cast<Request*>(context);
    try {
        request.result = icons::load(request.path, request.size, request.kind);
    } catch (const std::bad_alloc&) {
        request.result.error = "Out of memory";
    } catch (const std::length_error&) {
        request.result.error = "Image allocation exceeds container limits";
    }
}

void getIcon(PA_PluginParameters params, icons::Kind kind) {
    icons::Result result;
    try {
        // 4D materializes omitted declared Longint parameters as zero.
        PA_long32 size = PA_GetLongParameter(params, 2);
        if (size == 0)
            size = 256;
        std::u16string path;
        if (!icons::validSize(size)) {
            result.error = "Size must be positive and its RGBA buffer must fit WIC UINT32_MAX";
        } else if (!objectPath(PA_GetObjectParameter(params, 1), kind, path)) {
            result.error = "Cannot resolve the object's native path";
        } else {
            Request request{path, size, kind, {{}, "Native icon extraction did not run"}};
#ifdef __APPLE__
            // Ask 4D to switch context, never synchronously dispatch onto Cocoa.
            PA_RunInMainProcess(nativeLoad, &request);
            if (PA_GetLastError() != eER_NoErr)
                request.result.error = "4D could not run icon extraction in its main process";
#else
            nativeLoad(&request);
#endif
            result = std::move(request.result);
        }
    } catch (const std::bad_alloc&) {
        result.error = "Out of memory resolving icon path";
    } catch (const std::length_error&) {
        result.error = "Path exceeds container limits";
    }
    if (result.png.size() > std::numeric_limits<PA_long32>::max()) {
        result.png.clear();
        result.error = "Encoded PNG exceeds the SDK picture length";
    }
    PA_Picture picture = nullptr;
    if (result) {
        picture = PA_CreatePicture(result.png.data(), static_cast<PA_long32>(result.png.size()));
        if (PA_GetLastError() != eER_NoErr || !picture) {
            result.error = "4D could not create the PNG picture";
            if (picture)
                PA_DisposePicture(picture);
            picture = nullptr;
        }
    }
    if (!picture) {
        std::fprintf(stderr, "get-icon: %s\n", result.error.c_str());
        picture = PA_CreatePicture(nullptr, 0);
    }
    PA_ReturnPicture(params, picture);
}
}

void PluginMain(PA_long32 selector, PA_PluginParameters params) {
    switch (selector) {
        case 1: getIcon(params, icons::Kind::file); break;
        case 2: getIcon(params, icons::Kind::folder); break;
    }
}
