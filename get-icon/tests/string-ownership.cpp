#ifndef OWNERSHIP_PLUGIN_SOURCE
#define OWNERSHIP_PLUGIN_SOURCE "../get-icon-4dplugin.cpp"
#endif
#include OWNERSHIP_PLUGIN_SOURCE
#include "PrivateTypes.h"
#include "EntryPoints.h"

#include <algorithm>
#include <map>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

bool allocateEmpty = true;
std::map<PA_Unichar*, std::u16string> strings;
std::map<PA_Handle, PA_long32> handles;
int objects = 0;
int calls = 0;
PA_long32 expectedConstructor = 1566;
std::u16string expectedPath;
bool failConstructor = false;
bool wrongConstructorType = false;
bool nullConstructorObject = false;
bool failProperty = false;
bool wrongPropertyType = false;
bool pathPropertyOnly = false;
bool failGetter = false;
bool failHandle = false;

struct Object {
    bool file = true;
    bool folder = false;
    std::u16string path;
};

PA_Unistring makeString(const std::u16string& text) {
    if (text.empty() && !allocateEmpty)
        return {};
    PA_Unistring result{};
    result.fLength = static_cast<PA_long32>(text.size());
    result.fString = new PA_Unichar[text.size() + 1];
    std::copy(text.begin(), text.end(), result.fString);
    result.fString[text.size()] = 0;
    auto* pointer = result.fString;
    strings.emplace(pointer, text);
    return result;
}

std::u16string content(const PA_Unistring& value) {
    if (!value.fString) {
        require(value.fLength == 0, "Null string has a nonzero length");
        return {};
    }
    auto* pointer = value.fString;
    const auto found = strings.find(pointer);
    require(found != strings.end(), "String no longer belongs to a live owner");
    const auto& text = found->second;
    require(value.fLength == static_cast<PA_long32>(text.size()),
            "UTF-16 length must exclude the terminator and count surrogate pairs");
    require(std::equal(text.begin(), text.end(), value.fString) &&
            value.fString[text.size()] == 0, "String content or terminator changed");
    return text;
}

void disposeString(PA_Unistring& value) {
    content(value);
    if (value.fString) {
        auto* pointer = value.fString;
        strings.erase(pointer);
        std::fill_n(value.fString, value.fLength + 1, PA_Unichar(0xDDDD));
        delete[] value.fString;
    }
    value = {};
}

void clear(PA_Variable& value) {
    if (value.fType == eVK_Unistring)
        disposeString(value.uValue.fString);
    else if (value.fType == eVK_Object && value.uValue.fObject) {
        delete static_cast<Object*>(value.uValue.fObject);
        --objects;
    } else
        require(value.fType == 0 || value.fType == eVK_Undefined || value.fType == eVK_Longint ||
                value.fType == eVK_Boolean || value.fType == eVK_Object,
                "Unexpected owning variable type");
    value = {};
}

PA_Variable textVariable(const std::u16string& text) {
    PA_Variable result{};
    auto string = makeString(text);
    PA_SetStringVariable(&result, &string);
    return result;
}

void host(short selector, EngineBlock* block) {
    block->fError = eER_NoErr;
    switch (selector) {
        case EX_CREATE_UNISTRING: {
            const auto* text = reinterpret_cast<const char16_t*>(block->fHandle);
            require(text != nullptr, "CreateUnistring input must be terminated");
            block->fUniString1 = makeString(text);
            return;
        }
        case EX_DISPOSE_UNISTRING:
            disposeString(block->fUniString1);
            return;
        case EX_CLEAR_VARIABLE:
            clear(*static_cast<PA_Variable*>(block->fPtr1));
            return;
        case EX_VARIABLE_TO_STRING: {
            const auto& value = *reinterpret_cast<PA_Variable*>(block->fHandle);
            require(value.fType == eVK_Unistring, "Expected string conversion");
            content(value.uValue.fString);
            // Same-type conversion borrows the property's string; no new owner.
            block->fUniString1 = value.uValue.fString;
            if (failGetter)
                block->fError = eER_UnknownGestaltSelector;
            return;
        }
        case EX_GET_OBJ_VALUE: {
            require(block->fShort == eVK_Object, "Expected object property access");
            const auto& object = *static_cast<Object*>(block->fPtr1);
            const auto key = content(block->fUniString1);
            auto& value = *static_cast<PA_Variable*>(block->fPtr2);
            value = {};
            const bool pathKey = key == u"platformPath" || key == u"path";
            if (wrongPropertyType && (!pathPropertyOnly || pathKey)) {
                if (pathKey) {
                    value.fType = eVK_Object;
                    value.uValue.fObject = new Object;
                    ++objects;
                } else {
                    value = textVariable(u"owned wrong-type property");
                }
            } else if (key == u"isFile" || key == u"isFolder") {
                value.fType = eVK_Boolean;
                value.uValue.fBoolean = key == u"isFile" ? object.file : object.folder;
            } else {
                require(key == u"platformPath" || key == u"path", "Unexpected property key");
                value = textVariable(object.path);
            }
            if (failProperty && (!pathPropertyOnly || pathKey))
                block->fError = eER_UnknownGestaltSelector;
            return;
        }
        case EX_HANDLE_MANAGER: {
            PA_Handle handle = nullptr;
            if (block->fParam1 != 1)
                handle = block->fHandle;
            switch (block->fParam1) {
                case 1: {
                    if (failHandle) {
                        block->fHandle = nullptr;
                        block->fError = eER_UnknownGestaltSelector;
                        return;
                    }
                    auto handle = new char*;
                    *handle = new char[block->fParam2];
                    handles.emplace(handle, static_cast<PA_long32>(block->fParam2));
                    block->fHandle = handle;
                    return;
                }
                case 2:
                    require(handles.erase(handle) == 1, "Double handle disposal");
                    delete[] *handle;
                    delete handle;
                    return;
                case 5:
                    require(handles.count(handle) == 1, "Locking invalid handle");
                    block->fParam3 = reinterpret_cast<sLONG_PTR>(*handle);
                    return;
                case 6:
                    require(handles.count(handle) == 1, "Unlocking invalid handle");
                    return;
                default:
                    throw std::runtime_error("Unexpected handle operation");
            }
        }
        case EX_EXECUTE_COMMAND_BY_ID: {
            ++calls;
            require(block->fParam1 == expectedConstructor && block->fParam2 == 2,
                    "Wrong filesystem constructor or argument count");
            auto** arguments = static_cast<PA_Variable**>(block->fPtr1);
            require(arguments[0]->fType == eVK_Unistring &&
                    content(arguments[0]->uValue.fString) == expectedPath,
                    "Constructor path differs from exact original UTF-16 input");
            require(arguments[1]->fType == eVK_Longint &&
                    arguments[1]->uValue.fLongint == 1, "Wrong path format");
            auto& result = *static_cast<PA_Variable*>(block->fPtr2);
            if (wrongConstructorType) {
                result = textVariable(u"owned wrong-type result");
            } else {
                result = {};
                result.fType = eVK_Object;
                if (!nullConstructorObject) {
                    result.uValue.fObject = new Object{true, false, expectedPath};
                    ++objects;
                }
            }
            if (failConstructor)
                block->fError = eER_UnknownGestaltSelector;
            return;
        }
        default:
            throw std::runtime_error("Unexpected SDK callback");
    }
}

void balanced() {
    require(strings.empty(),
            "Leaked SDK string: pre-created empty variable overwritten by shallow setter");
    require(handles.empty(), "Leaked SDK argument handle");
    require(objects == 0, "Leaked reconstructed object");
}

void resolve(const std::u16string& text, bool folder = false, bool accepted = true) {
    Object object{!folder, folder, text};
    expectedPath = text;
    expectedConstructor = folder ? 1567 : 1566;
    std::u16string result;
    require(objectPath(&object, result) == accepted, "Unexpected path resolution result");
    if (accepted)
        require(result == text, "Borrowed property was not copied before owner cleanup");
    balanced();
}
}

namespace icons {
Result load(const std::u16string&, std::int32_t, OverlayPolicy) {
    throw std::runtime_error("String ownership test must not render icons");
}
}

int main() try {
    gCall4D = host;
    for (bool allocated : {false, true}) {
        allocateEmpty = allocated;
        for (const auto& text : {u"/ASCII/file.txt", u"/\u65E5\u672C/file.txt",
                                u"/\U0001F4C1/\U0001F600.txt"}) {
            for (int i = 0; i < 20; ++i) {
                resolve(text);
                resolve(text, true);
            }
        }
        resolve(u"", false, false);
        resolve(std::u16string(u"/bad\0path", 9), false, false);
        {
            Object object{true, false, u""};
            std::u16string result = u"unchanged";
            require(!readPath(&object, u"platformPath", result) && result == u"unchanged",
                    "Empty property should be rejected without modifying output");
            balanced();
        }
        failConstructor = true;
        resolve(u"/constructor-error", false, false);
        failConstructor = false;
        wrongConstructorType = true;
        resolve(u"/constructor-wrong-type", false, false);
        wrongConstructorType = false;
        nullConstructorObject = true;
        resolve(u"/constructor-null", false, false);
        nullConstructorObject = false;
        failHandle = true;
        resolve(u"/argument-handle-error", false, false);
        failHandle = false;
        failProperty = true;
        resolve(u"/property-error", false, false);
        pathPropertyOnly = true;
        resolve(u"/path-property-error", false, false);
        failProperty = false;
        wrongPropertyType = true;
        resolve(u"/path-property-wrong-type", false, false);
        pathPropertyOnly = false;
        resolve(u"/property-wrong-type", false, false);
        wrongPropertyType = false;
        failGetter = true;
        resolve(u"/getter-error", false, false);
        failGetter = false;
        require(!objectPath(nullptr, expectedPath), "Null input must be rejected");
        for (bool kind : {false, true}) {
            Object object{kind, kind, u"/ambiguous-kind"};
            require(!objectPath(&object, expectedPath), "Ambiguous object kind must be rejected");
        }
        balanced();
    }
    std::printf("PASS: %d constructors, null/allocated empties, exact ASCII/BMP/supplementary "
                "paths, borrowed copies and owning error cleanup\n", calls);
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s (live strings=%zu, handles=%zu, objects=%d)\n",
                 error.what(), strings.size(), handles.size(), objects);
    // Report leaks explicitly; reclaim fixtures so sanitizer output stays actionable.
    for (const auto& [pointer, text] : strings)
        delete[] pointer;
    return 1;
}
