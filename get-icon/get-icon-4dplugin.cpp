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

bool readBoolean(PA_ObjectRef object, const char16_t* name, bool& value) {
    Key key(name);
    Variable property(PA_GetObjectProperty(object, &key.value));
    if (PA_GetLastError() != eER_NoErr ||
        PA_GetVariableKind(property.value) != eVK_Boolean)
        return false;
    value = PA_GetBooleanVariable(property.value) != 0;
    return PA_GetLastError() == eER_NoErr;
}

bool objectPath(PA_ObjectRef object, std::u16string& path) {
    if (!object)
        return false;
    bool file = false, folder = false;
    if (!readBoolean(object, u"isFile", file) ||
        !readBoolean(object, u"isFolder", folder) || file == folder ||
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
        file ? 1566 : 1567, arguments, 2));
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
    icons::OverlayPolicy overlays;
    icons::Result result;
    PA_Picture picture = nullptr;
    bool completed = false;
};

void createIconPicture(void* context) {
    auto& request = *static_cast<Request*>(context);
    try {
        if (request.result.error.empty())
            request.result = icons::load(request.path, request.size, request.overlays);
    } catch (const std::bad_alloc&) {
        request.result.error = "Out of memory";
    } catch (const std::length_error&) {
        request.result.error = "Image allocation exceeds container limits";
    }
    if (request.result.png.size() > std::numeric_limits<PA_long32>::max()) {
        request.result.png.clear();
        request.result.error = "Encoded PNG exceeds the SDK picture length";
    }
    if (request.result) {
        request.picture = PA_CreatePicture(request.result.png.data(),
            static_cast<PA_long32>(request.result.png.size()));
        if (PA_GetLastError() != eER_NoErr || !request.picture) {
            request.result.error = "4D could not create the PNG picture";
            if (request.picture)
                PA_DisposePicture(request.picture);
            request.picture = nullptr;
        }
    }
    if (!request.picture) {
        std::fprintf(stderr, "get-icon: %s\n", request.result.error.c_str());
        request.picture = PA_CreatePicture(nullptr, 0);
        if (PA_GetLastError() != eER_NoErr || !request.picture) {
            std::fprintf(stderr, "get-icon: 4D could not create an empty picture\n");
            if (request.picture)
                PA_DisposePicture(request.picture);
            request.picture = nullptr;
        }
    }
    request.completed = true;
}

#ifdef __APPLE__
void discardIconPicture(void* context) {
    auto& request = *static_cast<Request*>(context);
    PA_DisposePicture(request.picture);
    if (PA_GetLastError() == eER_NoErr)
        request.picture = nullptr;
}
#endif

void runIconRequest(Request& request) {
#ifdef __APPLE__
    // Keep native rendering, SDK picture creation and failure cleanup in one cycle.
    // Parameter access and ReturnPicture belong to the original caller's context.
    PA_RunInMainProcess(createIconPicture, &request);
    if (PA_GetLastError() != eER_NoErr || !request.completed) {
        std::fprintf(stderr, "get-icon: 4D could not run picture construction in its main process\n");
        if (request.picture) {
            PA_RunInMainProcess(discardIconPicture, &request);
            if (PA_GetLastError() != eER_NoErr || request.picture)
                std::fprintf(stderr, "get-icon: 4D could not dispose the unreturned picture in its main process\n");
            request.picture = nullptr;
        }
    }
#else
    createIconPicture(&request);
#endif
}

void getIcon(PA_PluginParameters params) {
    icons::Result result;
    PA_long32 size = 256;
    auto overlays = icons::OverlayPolicy::system;
    std::u16string path;
    try {
        // 4D materializes omitted declared Longint parameters as zero.
        size = PA_GetLongParameter(params, 2);
        const PA_long32 option = PA_GetLongParameter(params, 3);
        overlays = option == 1 ? icons::OverlayPolicy::noShortcut :
                   option == 2 ? icons::OverlayPolicy::none :
                                 icons::OverlayPolicy::system;
        if (size == 0)
            size = 256;
        if (!icons::validSize(size)) {
            result.error = "Size must be positive and its RGBA buffer must fit WIC UINT32_MAX";
        } else if (!objectPath(PA_GetObjectParameter(params, 1), path)) {
            result.error = "Cannot resolve the object's native path";
        }
    } catch (const std::bad_alloc&) {
        result.error = "Out of memory resolving icon path";
    } catch (const std::length_error&) {
        result.error = "Path exceeds container limits";
    }
    Request request{path, size, overlays, std::move(result)};
    runIconRequest(request);
    PA_ReturnPicture(params, request.picture);
}
}

void PluginMain(PA_long32 selector, PA_PluginParameters params) {
    switch (selector) {
        case 1: getIcon(params); break;
    }
}
