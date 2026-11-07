#include "../get-icon-4dplugin.cpp"
#include "PrivateTypes.h"
#include "EntryPoints.h"

#include <stdexcept>

namespace {
bool inMain = false;
bool failDispatch = false;
bool failAfterCallback = false;
bool failPNG = false;
bool failEmpty = false;
bool failLoad = false;
int dispatches = 0;
int loads = 0;
int creates = 0;
int disposals = 0;
int pngToken = 0;
int emptyToken = 0;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void host(short selector, EngineBlock* block) {
    block->fError = eER_NoErr;
    switch (selector) {
        case EX_RUN_IN_MAIN_PROCESS: {
            require(!inMain, "Nested main-process dispatch");
            ++dispatches;
            if (failDispatch) {
                block->fError = eER_UnknownGestaltSelector;
                return;
            }
            inMain = true;
            auto callback = reinterpret_cast<PA_RunInMainProcessProcPtr>(block->fPtr1);
            callback(block->fPtr2);
            inMain = false;
            if (failAfterCallback && dispatches == 1)
                block->fError = eER_UnknownGestaltSelector;
            return;
        }
        case EX_CREATE_PICTURE:
            require(inMain, "SDK picture creation escaped main-process callback");
            ++creates;
            // Also exercise SDK failures that return a non-null, unowned picture.
            block->fPicture = block->fParam1 ? &pngToken : &emptyToken;
            if ((block->fParam1 && failPNG) || (!block->fParam1 && failEmpty))
                block->fError = eER_UnknownGestaltSelector;
            return;
        case EX_DISPOSE_PICTURE:
            require(inMain, "SDK picture disposal escaped main-process callback");
            ++disposals;
            return;
        default:
            throw std::runtime_error("Unexpected SDK callback");
    }
}

void reset() {
    inMain = failDispatch = failAfterCallback = failPNG = failEmpty = failLoad = false;
    dispatches = loads = creates = disposals = 0;
}

Request request(const std::u16string& path, const char* error = "") {
    return {path, 32, icons::Kind::folder, icons::OverlayPolicy::system, {{}, error}};
}
}

namespace icons {
Result load(const std::u16string&, std::int32_t, Kind, OverlayPolicy) {
    require(inMain, "Native extraction escaped main-process callback");
    ++loads;
    return failLoad ? Result{{}, "Simulated native failure"} : Result{{1, 2, 3}, {}};
}
}

int main() try {
    gCall4D = host;
    const std::u16string path = u"/fixture";
    for (int i = 0; i < 100; ++i) {
        reset();
        auto value = request(path);
        runIconRequest(value);
        require(value.completed && value.picture == &pngToken, "Valid picture lost");
        require(dispatches == 1 && loads == 1 && creates == 1 && disposals == 0,
                "Successful graphics lifecycle must use exactly one callback");
        PA_Picture returned = nullptr;
        PluginBlock params{};
        params.fResult = &returned;
        PA_ReturnPicture(&params, value.picture);
        require(!inMain && returned == &pngToken && disposals == 0,
                "Return must transfer the picture without disposal in caller context");
    }
    reset();
    auto invalid = request(path, "Invalid input");
    runIconRequest(invalid);
    require(loads == 0 && creates == 1 && invalid.picture == &emptyToken,
            "Invalid input must construct its empty result in the main process");

    reset();
    failLoad = true;
    auto missing = request(path);
    runIconRequest(missing);
    require(loads == 1 && creates == 1 && missing.picture == &emptyToken,
            "Native failure must construct an empty picture");

    reset();
    failPNG = true;
    auto broken = request(path);
    runIconRequest(broken);
    require(dispatches == 1 && creates == 2 && disposals == 1 &&
            broken.picture == &emptyToken, "PNG failure cleanup escaped the callback");

    reset();
    failEmpty = true;
    auto empty = request(path, "Invalid input");
    runIconRequest(empty);
    require(creates == 1 && disposals == 1 && !empty.picture,
            "Failed empty picture must not be returned");

    reset();
    failDispatch = true;
    auto blocked = request(path);
    runIconRequest(blocked);
    require(!blocked.completed && !blocked.picture && loads == 0 && creates == 0,
            "Dispatch failure must not run graphics in the caller");

    reset();
    failAfterCallback = true;
    auto rejected = request(path);
    runIconRequest(rejected);
    require(dispatches == 2 && disposals == 1 && !rejected.picture,
            "Dispatch error must discard its unreturned picture in the main process");
    std::puts("PASS: main-process graphics lifecycle and caller-context ownership transfer");
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
