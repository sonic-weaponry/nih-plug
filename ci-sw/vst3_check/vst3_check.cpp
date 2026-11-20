/*
 * Crisp CI checks: a small VST3 host (ci-sw/vst3_check)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Copyright (C) 2026 Sonic Weaponry
 *
 * Adapted from the host we wrote for the Wolf Shaper Mac fix (github.com/sonic-weaponry/wolf-shaper, ci/vst3_check).
 */

// Loads a VST3 bundle the way a host does, runs one check, and exits. One check per process, so a crash in one can't
// hide the other results. Crisp's code is never changed by these checks; they only pin down what a host sees.
//
//   vst3_check ids <bundle> <class-ids.txt>
//       The class list, read through the VST3 SDK's own FUID code (what hosts store in projects): one class, its
//       canonical ID and the raw bytes this platform hands the host, against the frozen table's line for this OS.
//   vst3_check info <bundle> <plugin-info.json> [<write to>]
//       What a host sees besides the parameters: factory, class info (vendor, version, categories), how the
//       component and controller are put together, buses, latency and tail.
//   vst3_check params <bundle> <parameters.json> [<write to>]
//       Every parameter the controller lists: ID, title, units, steps, flags, default, and the plain value and text
//       at five points of its range.
//   vst3_check states <bundle> <state file>...
//       Each state loads into a fresh instance and saves back byte for byte. A file named default.json must also be
//       exactly what a fresh instance saves.
//   vst3_check savestate <bundle> <out file>
//       Writes what a fresh instance saves (to make default.json).
//   vst3_check render <bundle> <state file or -> <input.wav> <block size> <output.wav>
//       Loads the state as a host restores a saved device, plays 1 s of silence and then the input (32-bit float,
//       mono or stereo) through the plugin in blocks of the given size, and writes a 32-bit float stereo WAV.
//   vst3_check nulltest <a.wav> <b.wav> <limit dBFS>
//       Subtracts b from a, sample by sample, and passes when the largest difference is at or below the limit.
//
// Exit code: 0 when the check passes, 1 when it fails, 2 for a usage error. The last line printed is always
// "RESULT <check> PASS|FAIL <detail>".

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#endif

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#define CHECK_OS "windows"
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#define CHECK_OS "macos"
#else
#include <dlfcn.h>
#define CHECK_OS "linux"
#endif

#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
#define CHECK_CPU "arm64"
#elif defined(__x86_64__) || defined(_M_X64)
#define CHECK_CPU "x86_64"
#else
#define CHECK_CPU "other"
#endif

using namespace Steinberg;
using namespace Steinberg::Vst;

// --------------------------------------------------------------------------------------------------------------------
// Small helpers

static bool readFile(const std::string& path, std::string& data)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

static bool writeFile(const std::string& path, const std::string& data)
{
    std::ofstream out(path, std::ios::binary);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

static std::string withoutCarriageReturns(std::string text)
{
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}

static std::string baseName(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    while (!path.empty() && path.back() == '/')
        path.pop_back();
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

static std::string toUtf8(const char16* text, size_t capacity)
{
    std::string out;
    for (size_t i = 0; i < capacity && text[i] != 0; ++i)
    {
        uint32 c = static_cast<uint16>(text[i]);
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < capacity)
        {
            const uint32 low = static_cast<uint16>(text[i + 1]);
            if (low >= 0xDC00 && low <= 0xDFFF)
            {
                c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            }
        }
        if (c < 0x80)
            out += static_cast<char>(c);
        else if (c < 0x800)
        {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
        else if (c < 0x10000)
        {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
        else
        {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

static std::string cString(const char8* text, size_t capacity)
{
    size_t length = 0;
    while (length < capacity && text[length] != 0)
        ++length;
    return std::string(text, length);
}

static std::string rawBytes(const TUID id)
{
    char text[16 * 3 + 1] = {};
    for (int b = 0; b < 16; ++b)
        std::snprintf(text + b * 3, 4, b < 15 ? "%02X " : "%02X", static_cast<unsigned>(static_cast<unsigned char>(id[b])));
    return text;
}

// The ID the VST3 SDK makes of these 16 bytes on this platform (FUID::toString), the form hosts compare and store.
static std::string canonicalId(const TUID id)
{
    char text[33] = {};
    FUID::fromTUID(id).toString(text);
    return text;
}

static const char* resultName(tresult result)
{
    if (result == kResultOk)
        return "ok";
    if (result == kResultFalse)
        return "false";
    if (result == kNoInterface)
        return "no interface";
    if (result == kInvalidArgument)
        return "invalid argument";
    if (result == kNotImplemented)
        return "not implemented";
    if (result == kInternalError)
        return "internal error";
    if (result == kNotInitialized)
        return "not initialized";
    if (result == kOutOfMemory)
        return "out of memory";
    return "unknown result";
}

static std::string jsonString(const std::string& text)
{
    std::string out = "\"";
    for (unsigned char c : text)
    {
        if (c == '"')
            out += "\\\"";
        else if (c == '\\')
            out += "\\\\";
        else if (c == '\n')
            out += "\\n";
        else if (c < 0x20)
        {
            char escaped[8];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
            out += escaped;
        }
        else
            out += static_cast<char>(c);
    }
    return out + "\"";
}

// Values a plugin computes in floating point can differ in their last bits between platforms' math libraries. Six
// significant digits are what the frozen tables pin; the sound null test catches anything smaller that matters.
static std::string jsonNumber(double value)
{
    if (!std::isfinite(value))
        return "null";
    if (value == 0.0)
        return "0";
    char text[40];
    std::snprintf(text, sizeof(text), "%.6g", value);
    return text;
}

static bool matchesFrozen(const std::string& frozenPath, const std::string& generated, std::string& detail)
{
    std::string frozen;
    if (!readFile(frozenPath, frozen))
    {
        detail = "could not read the frozen table " + frozenPath;
        return false;
    }
    frozen = withoutCarriageReturns(frozen);
    if (frozen == generated)
        return true;
    std::istringstream a(frozen), b(generated);
    std::string lineA, lineB;
    for (int line = 1;; ++line)
    {
        const bool moreA = static_cast<bool>(std::getline(a, lineA));
        const bool moreB = static_cast<bool>(std::getline(b, lineB));
        if (!moreA && !moreB)
            break;
        if (!moreA || !moreB || lineA != lineB)
        {
            std::printf("DIFF line %d\n  frozen: %s\n  got:    %s\n", line, moreA ? lineA.c_str() : "(end)",
                        moreB ? lineB.c_str() : "(end)");
            detail = "differs from " + baseName(frozenPath) + " at line " + std::to_string(line);
            return false;
        }
    }
    detail = "differs from " + baseName(frozenPath);
    return false;
}

// --------------------------------------------------------------------------------------------------------------------
// Host side objects. They live for the whole process, so reference counting is a no-op.

class HostApplication : public IHostApplication
{
public:
    tresult PLUGIN_API getName(String128 name) override
    {
        const char* text = "vst3_check";
        size_t i = 0;
        for (; text[i] != 0 && i < 127; ++i)
            name[i] = static_cast<char16>(text[i]);
        name[i] = 0;
        return kResultOk;
    }

    tresult PLUGIN_API createInstance(TUID, TUID, void** obj) override
    {
        *obj = nullptr;
        return kResultFalse;
    }

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
    {
        if (FUnknownPrivate::iidEqual(iid, FUnknown_iid) || FUnknownPrivate::iidEqual(iid, IHostApplication_iid))
        {
            *obj = static_cast<IHostApplication*>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef() override { return 1000; }
    uint32 PLUGIN_API release() override { return 1000; }
};

class ComponentHandler : public IComponentHandler
{
public:
    tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID, ParamValue) override { return kResultOk; }
    tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32) override { return kResultOk; }

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
    {
        if (FUnknownPrivate::iidEqual(iid, FUnknown_iid) || FUnknownPrivate::iidEqual(iid, IComponentHandler_iid))
        {
            *obj = static_cast<IComponentHandler*>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef() override { return 1000; }
    uint32 PLUGIN_API release() override { return 1000; }
};

// The stream a host hands to setState / getState: a plain growable byte buffer.
class MemoryStream : public IBStream
{
public:
    std::string data;
    int64 position = 0;

    MemoryStream() {}
    explicit MemoryStream(const std::string& bytes) : data(bytes) {}

    tresult PLUGIN_API read(void* buffer, int32 numBytes, int32* numBytesRead) override
    {
        if (numBytes < 0)
            return kInvalidArgument;
        const int64 available = std::max<int64>(0, static_cast<int64>(data.size()) - position);
        const int32 count = static_cast<int32>(std::min<int64>(numBytes, available));
        if (count > 0)
            std::memcpy(buffer, data.data() + position, static_cast<size_t>(count));
        position += count;
        if (numBytesRead != nullptr)
            *numBytesRead = count;
        return kResultOk;
    }

    tresult PLUGIN_API write(void* buffer, int32 numBytes, int32* numBytesWritten) override
    {
        if (numBytes < 0)
            return kInvalidArgument;
        const size_t end = static_cast<size_t>(position) + static_cast<size_t>(numBytes);
        if (end > data.size())
            data.resize(end);
        if (numBytes > 0)
            std::memcpy(&data[static_cast<size_t>(position)], buffer, static_cast<size_t>(numBytes));
        position += numBytes;
        if (numBytesWritten != nullptr)
            *numBytesWritten = numBytes;
        return kResultOk;
    }

    tresult PLUGIN_API seek(int64 pos, int32 mode, int64* result) override
    {
        int64 target = pos;
        if (mode == kIBSeekCur)
            target = position + pos;
        else if (mode == kIBSeekEnd)
            target = static_cast<int64>(data.size()) + pos;
        else if (mode != kIBSeekSet)
            return kInvalidArgument;
        if (target < 0)
            return kInvalidArgument;
        position = target;
        if (result != nullptr)
            *result = position;
        return kResultOk;
    }

    tresult PLUGIN_API tell(int64* pos) override
    {
        if (pos == nullptr)
            return kInvalidArgument;
        *pos = position;
        return kResultOk;
    }

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
    {
        if (FUnknownPrivate::iidEqual(iid, FUnknown_iid) || FUnknownPrivate::iidEqual(iid, IBStream_iid))
        {
            *obj = static_cast<IBStream*>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef() override { return 1000; }
    uint32 PLUGIN_API release() override { return 1000; }
};

static HostApplication gHost;
static ComponentHandler gHandler;

// --------------------------------------------------------------------------------------------------------------------
// The module, loaded with the platform's entry and exit functions, like a host does.

typedef IPluginFactory*(PLUGIN_API* GetFactoryProc)();
typedef bool (*ExitProc)();

struct Module
{
    IPluginFactory* factory = nullptr;
    ExitProc exitProc = nullptr;

    bool load(const std::string& bundle)
    {
#if defined(_WIN32)
        std::string binary = bundle + "/Contents/x86_64-win/" + baseName(bundle);
        std::replace(binary.begin(), binary.end(), '/', '\\');
        HMODULE module = LoadLibraryA(binary.c_str());
        if (module == nullptr)
        {
            std::printf("ERROR LoadLibrary failed for %s (error %lu)\n", binary.c_str(), GetLastError());
            return false;
        }
        typedef bool (*InitProc)();
        if (InitProc init = reinterpret_cast<InitProc>(GetProcAddress(module, "InitDll")))
            init();
        exitProc = reinterpret_cast<ExitProc>(GetProcAddress(module, "ExitDll"));
        GetFactoryProc proc = reinterpret_cast<GetFactoryProc>(GetProcAddress(module, "GetPluginFactory"));
#elif defined(__APPLE__)
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(bundle.c_str()),
                                                               static_cast<CFIndex>(bundle.size()), true);
        CFBundleRef module = url != nullptr ? CFBundleCreate(nullptr, url) : nullptr;
        if (url != nullptr)
            CFRelease(url);
        if (module == nullptr || !CFBundleLoadExecutable(module))
        {
            std::printf("ERROR could not load the bundle %s\n", bundle.c_str());
            return false;
        }
        typedef bool (*EntryProc)(CFBundleRef);
        if (EntryProc entry = reinterpret_cast<EntryProc>(CFBundleGetFunctionPointerForName(module, CFSTR("bundleEntry"))))
            entry(module);
        exitProc = reinterpret_cast<ExitProc>(CFBundleGetFunctionPointerForName(module, CFSTR("bundleExit")));
        GetFactoryProc proc =
            reinterpret_cast<GetFactoryProc>(CFBundleGetFunctionPointerForName(module, CFSTR("GetPluginFactory")));
#else
        std::string name = baseName(bundle);
        const size_t dot = name.rfind('.');
        if (dot != std::string::npos)
            name = name.substr(0, dot);
        const std::string binary = bundle + "/Contents/" CHECK_CPU "-linux/" + name + ".so";
        void* module = dlopen(binary.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (module == nullptr)
        {
            std::printf("ERROR dlopen failed: %s\n", dlerror());
            return false;
        }
        typedef bool (*EntryProc)(void*);
        if (EntryProc entry = reinterpret_cast<EntryProc>(dlsym(module, "ModuleEntry")))
            entry(module);
        exitProc = reinterpret_cast<ExitProc>(dlsym(module, "ModuleExit"));
        GetFactoryProc proc = reinterpret_cast<GetFactoryProc>(dlsym(module, "GetPluginFactory"));
#endif
        if (proc == nullptr)
        {
            std::printf("ERROR no GetPluginFactory in %s\n", bundle.c_str());
            return false;
        }
        factory = proc();
        if (factory == nullptr)
        {
            std::printf("ERROR GetPluginFactory returned nothing\n");
            return false;
        }
        IPluginFactory3* factory3 = nullptr;
        if (factory->queryInterface(IPluginFactory3_iid, reinterpret_cast<void**>(&factory3)) == kResultOk &&
            factory3 != nullptr)
        {
            factory3->setHostContext(&gHost);
            factory3->release();
        }
        return true;
    }

    void unload()
    {
        if (factory != nullptr)
        {
            factory->release();
            factory = nullptr;
        }
        if (exitProc != nullptr)
        {
            exitProc();
            exitProc = nullptr;
        }
    }
};

struct ClassIds
{
    bool haveComponent = false;
    bool haveController = false;
    TUID component = {};
    TUID controller = {};
};

static ClassIds findClasses(IPluginFactory* factory)
{
    ClassIds ids;
    for (int32 i = 0, count = factory->countClasses(); i < count; ++i)
    {
        PClassInfo info{};
        if (factory->getClassInfo(i, &info) != kResultOk)
            continue;
        if (std::strcmp(info.category, kVstAudioEffectClass) == 0 && !ids.haveComponent)
        {
            std::memcpy(ids.component, info.cid, sizeof(TUID));
            ids.haveComponent = true;
        }
        else if (std::strcmp(info.category, kVstComponentControllerClass) == 0 && !ids.haveController)
        {
            std::memcpy(ids.controller, info.cid, sizeof(TUID));
            ids.haveController = true;
        }
    }
    return ids;
}

// --------------------------------------------------------------------------------------------------------------------
// One plugin instance, opened as the VST3 SDK's own hosting code does: the component, then either the controller
// class it names (getControllerClassId) or, for a single-component plugin, the component's own IEditController.

struct Session
{
    IComponent* component = nullptr;
    IEditController* controller = nullptr;
    bool singleComponent = false;
    IConnectionPoint* componentPoint = nullptr;
    IConnectionPoint* controllerPoint = nullptr;
    bool connected = false;
    bool ok = true;
    std::vector<std::string> steps;
    std::string error;

    bool step(const std::string& what, tresult result)
    {
        steps.push_back(what + ": " + resultName(result));
        if (result != kResultOk)
        {
            ok = false;
            if (error.empty())
                error = what + " returned " + resultName(result);
        }
        return result == kResultOk;
    }

    bool fail(const std::string& why)
    {
        steps.push_back("FAILED: " + why);
        ok = false;
        if (error.empty())
            error = why;
        return false;
    }

    bool open(IPluginFactory* factory, const ClassIds& ids)
    {
        if (!ids.haveComponent)
            return fail("the factory lists no Audio Module Class");
        if (!step("create the component from its class ID",
                  factory->createInstance(ids.component, IComponent_iid, reinterpret_cast<void**>(&component))))
            return false;
        if (component == nullptr)
            return fail("createInstance returned no component");
        if (!step("initialize the component", component->initialize(&gHost)))
            return false;

        TUID controllerId = {};
        const tresult named = component->getControllerClassId(controllerId);
        steps.push_back(std::string("getControllerClassId: ") + resultName(named));
        if (named == kResultOk && ids.haveController)
        {
            if (std::memcmp(controllerId, ids.controller, sizeof(TUID)) != 0)
                return fail("getControllerClassId names " + canonicalId(controllerId) + ", the factory lists " +
                            canonicalId(ids.controller));
            if (!step("create the controller from that class ID",
                      factory->createInstance(controllerId, IEditController_iid, reinterpret_cast<void**>(&controller))))
                return false;
            if (controller == nullptr)
                return fail("createInstance returned no controller");
            if (!step("initialize the controller", controller->initialize(&gHost)))
                return false;
        }
        else
        {
            if (!step("the component's own IEditController (single component)",
                      component->queryInterface(IEditController_iid, reinterpret_cast<void**>(&controller))))
                return false;
            if (controller == nullptr)
                return fail("queryInterface returned no controller");
            singleComponent = true;
        }
        if (!step("setComponentHandler", controller->setComponentHandler(&gHandler)))
            return false;

        if (!singleComponent)
        {
            if (component->queryInterface(IConnectionPoint_iid, reinterpret_cast<void**>(&componentPoint)) != kResultOk)
                componentPoint = nullptr;
            if (controller->queryInterface(IConnectionPoint_iid, reinterpret_cast<void**>(&controllerPoint)) != kResultOk)
                controllerPoint = nullptr;
            if (componentPoint != nullptr && controllerPoint != nullptr)
            {
                if (!step("connect component to controller", componentPoint->connect(controllerPoint)))
                    return false;
                if (!step("connect controller to component", controllerPoint->connect(componentPoint)))
                    return false;
                connected = true;
            }
        }
        return true;
    }

    void close()
    {
        if (connected)
        {
            step("disconnect component from controller", componentPoint->disconnect(controllerPoint));
            step("disconnect controller from component", controllerPoint->disconnect(componentPoint));
            connected = false;
        }
        if (componentPoint != nullptr)
            componentPoint->release();
        if (controllerPoint != nullptr)
            controllerPoint->release();
        componentPoint = controllerPoint = nullptr;
        if (controller != nullptr)
        {
            step("clear the component handler", controller->setComponentHandler(nullptr));
            if (!singleComponent)
                step("terminate the controller", controller->terminate());
            controller->release();
            controller = nullptr;
        }
        if (component != nullptr)
        {
            step("terminate the component", component->terminate());
            component->release();
            component = nullptr;
        }
    }

    void printSteps(const char* prefix) const
    {
        for (const std::string& line : steps)
            std::printf("%s%s\n", prefix, line.c_str());
    }
};

static tresult loadState(Session& session, const std::string& state)
{
    MemoryStream forComponent(state);
    const tresult result = session.component->setState(&forComponent);
    if (result != kResultOk)
        return result;
    MemoryStream forController(state);
    return session.controller->setComponentState(&forController);
}

static tresult saveState(Session& session, std::string& state)
{
    MemoryStream stream;
    const tresult result = session.component->getState(&stream);
    state = stream.data;
    return result;
}

// --------------------------------------------------------------------------------------------------------------------
// Check: ids

static bool checkIds(Module& module, const std::string& frozenPath, std::string& detail)
{
    std::string frozen;
    if (!readFile(frozenPath, frozen))
    {
        detail = "could not read " + frozenPath;
        return false;
    }
    std::string wantCanonical, wantRaw, wantName, wantCategory;
    std::istringstream lines(withoutCarriageReturns(frozen));
    for (std::string line; std::getline(lines, line);)
    {
        if (line.empty() || line[0] == '#')
            continue;
        // <os> <canonical> <16 raw bytes> | <name> | <category>
        std::istringstream fields(line);
        std::string os;
        fields >> os;
        if (os != CHECK_OS)
            continue;
        fields >> wantCanonical;
        std::string rest;
        std::getline(fields, rest);
        const size_t bar1 = rest.find('|'), bar2 = rest.rfind('|');
        if (bar1 == std::string::npos || bar2 == bar1)
        {
            detail = "bad line in " + baseName(frozenPath) + ": " + line;
            return false;
        }
        const auto trim = [](std::string s) {
            s.erase(0, s.find_first_not_of(' '));
            s.erase(s.find_last_not_of(' ') + 1);
            return s;
        };
        wantRaw = trim(rest.substr(0, bar1));
        wantName = trim(rest.substr(bar1 + 1, bar2 - bar1 - 1));
        wantCategory = trim(rest.substr(bar2 + 1));
    }
    if (wantCanonical.empty())
    {
        detail = "no line for " CHECK_OS " in " + baseName(frozenPath);
        return false;
    }

    const int32 count = module.factory->countClasses();
    std::printf("CLASSES %d\n", count);
    bool ok = count == 1;
    if (!ok)
        detail = "expected exactly 1 class, the factory lists " + std::to_string(count);
    for (int32 i = 0; i < count; ++i)
    {
        PClassInfo info{};
        if (module.factory->getClassInfo(i, &info) != kResultOk)
        {
            detail = "getClassInfo(" + std::to_string(i) + ") failed";
            return false;
        }
        const std::string name = cString(info.name, sizeof(info.name));
        const std::string category = cString(info.category, sizeof(info.category));
        std::printf("CLASS %d name %s category %s canonical %s raw %s\n", i, name.c_str(), category.c_str(),
                    canonicalId(info.cid).c_str(), rawBytes(info.cid).c_str());
        if (i == 0)
        {
            const bool same = canonicalId(info.cid) == wantCanonical && rawBytes(info.cid) == wantRaw &&
                              name == wantName && category == wantCategory;
            if (!same)
            {
                std::printf("WANT  name %s category %s canonical %s raw %s\n", wantName.c_str(), wantCategory.c_str(),
                            wantCanonical.c_str(), wantRaw.c_str());
                ok = false;
                if (detail.empty())
                    detail = "class 0 differs from the frozen " CHECK_OS " line";
            }
        }
    }
    if (ok)
        detail = "1 class, canonical " + wantCanonical + " (" CHECK_OS " " CHECK_CPU ")";
    return ok;
}

// --------------------------------------------------------------------------------------------------------------------
// Check: info

static std::string busesJson(IComponent* component, MediaType media, BusDirection direction)
{
    std::string out = "[";
    const int32 count = component->getBusCount(media, direction);
    for (int32 i = 0; i < count; ++i)
    {
        BusInfo bus{};
        component->getBusInfo(media, direction, i, bus);
        out += std::string(i ? ", " : "") + "{\"name\": " + jsonString(toUtf8(bus.name, 128)) +
               ", \"channels\": " + std::to_string(bus.channelCount) + ", \"type\": " + std::to_string(bus.busType) +
               ", \"flags\": " + std::to_string(bus.flags) + "}";
    }
    return out + "]";
}

static bool checkInfo(Module& module, const std::string& frozenPath, const std::string& writePath, std::string& detail)
{
    std::string json = "{\n";
    PFactoryInfo factoryInfo{};
    module.factory->getFactoryInfo(&factoryInfo);
    json += "  \"factory\": {\"vendor\": " + jsonString(cString(factoryInfo.vendor, sizeof(factoryInfo.vendor))) +
            ", \"url\": " + jsonString(cString(factoryInfo.url, sizeof(factoryInfo.url))) +
            ", \"email\": " + jsonString(cString(factoryInfo.email, sizeof(factoryInfo.email))) +
            ", \"flags\": " + std::to_string(factoryInfo.flags) + "},\n";

    IPluginFactory2* factory2 = nullptr;
    if (module.factory->queryInterface(IPluginFactory2_iid, reinterpret_cast<void**>(&factory2)) == kResultOk &&
        factory2 != nullptr)
    {
        PClassInfo2 info{};
        if (factory2->getClassInfo2(0, &info) == kResultOk)
            json += "  \"class\": {\"name\": " + jsonString(cString(info.name, sizeof(info.name))) +
                    ", \"category\": " + jsonString(cString(info.category, sizeof(info.category))) +
                    ", \"cardinality\": " + std::to_string(info.cardinality) +
                    ", \"subCategories\": " + jsonString(cString(info.subCategories, sizeof(info.subCategories))) +
                    ", \"vendor\": " + jsonString(cString(info.vendor, sizeof(info.vendor))) +
                    ", \"version\": " + jsonString(cString(info.version, sizeof(info.version))) +
                    ", \"sdkVersion\": " + jsonString(cString(info.sdkVersion, sizeof(info.sdkVersion))) +
                    ", \"classFlags\": " + std::to_string(info.classFlags) + "},\n";
        factory2->release();
    }
    else
        json += "  \"class\": null,\n";

    const ClassIds ids = findClasses(module.factory);
    Session session;
    IAudioProcessor* processor = nullptr;
    bool ok = session.open(module.factory, ids);
    if (ok && (session.component->queryInterface(IAudioProcessor_iid, reinterpret_cast<void**>(&processor)) != kResultOk ||
               processor == nullptr))
    {
        processor = nullptr;
        ok = session.fail("the component has no IAudioProcessor");
    }
    if (ok)
    {
        json += std::string("  \"singleComponent\": ") + (session.singleComponent ? "true" : "false") + ",\n";
        json += "  \"audioInputs\": " + busesJson(session.component, kAudio, kInput) + ",\n";
        json += "  \"audioOutputs\": " + busesJson(session.component, kAudio, kOutput) + ",\n";
        json += "  \"eventInputs\": " + std::to_string(session.component->getBusCount(kEvent, kInput)) + ",\n";
        json += "  \"eventOutputs\": " + std::to_string(session.component->getBusCount(kEvent, kOutput)) + ",\n";
        json += std::string("  \"float32\": ") + (processor->canProcessSampleSize(kSample32) == kResultOk ? "true" : "false") +
                ",\n";
        json += std::string("  \"float64\": ") + (processor->canProcessSampleSize(kSample64) == kResultOk ? "true" : "false") +
                ",\n";
        ProcessSetup setup{kRealtime, kSample32, 512, 48000.0};
        ok = session.step("setupProcessing (48 kHz, 512)", processor->setupProcessing(setup));
        if (ok)
        {
            json += "  \"latencySamples\": " + std::to_string(processor->getLatencySamples()) + ",\n";
            json += "  \"tailSamples\": " + std::to_string(processor->getTailSamples()) + ",\n";
        }
        json += "  \"parameterCount\": " + std::to_string(session.controller->getParameterCount()) + "\n}\n";
    }
    if (processor != nullptr)
        processor->release();
    session.close();
    if (!session.ok)
    {
        session.printSteps("  ");
        detail = session.error;
        return false;
    }
    std::printf("%s", json.c_str());
    if (!writePath.empty())
        writeFile(writePath, json);
    if (!matchesFrozen(frozenPath, json, detail))
        return false;
    detail = "matches " + baseName(frozenPath);
    return true;
}

// --------------------------------------------------------------------------------------------------------------------
// Check: params

static const double kProbePoints[] = {0.0, 0.25, 0.5, 0.75, 1.0};

static std::string valueText(IEditController* controller, ParamID id, ParamValue normalized)
{
    String128 text = {};
    if (controller->getParamStringByValue(id, normalized, text) != kResultOk)
        return "(none)";
    return toUtf8(text, 128);
}

static bool checkParams(Module& module, const std::string& frozenPath, const std::string& writePath, std::string& detail)
{
    const ClassIds ids = findClasses(module.factory);
    Session session;
    std::string json = "[\n";
    if (session.open(module.factory, ids))
    {
        const int32 count = session.controller->getParameterCount();
        for (int32 i = 0; i < count; ++i)
        {
            ParameterInfo info{};
            if (!session.step("getParameterInfo(" + std::to_string(i) + ")", session.controller->getParameterInfo(i, info)))
                break;
            std::string points = "[";
            for (size_t p = 0; p < sizeof(kProbePoints) / sizeof(kProbePoints[0]); ++p)
            {
                const double at = kProbePoints[p];
                points += std::string(p ? ", " : "") + "[" + jsonNumber(at) + ", " +
                          jsonNumber(session.controller->normalizedParamToPlain(info.id, at)) + ", " +
                          jsonString(valueText(session.controller, info.id, at)) + "]";
            }
            points += "]";
            json += std::string(i ? ",\n" : "") + "  {\"index\": " + std::to_string(i) +
                    ", \"id\": " + std::to_string(info.id) + ", \"title\": " + jsonString(toUtf8(info.title, 128)) +
                    ", \"shortTitle\": " + jsonString(toUtf8(info.shortTitle, 128)) +
                    ", \"units\": " + jsonString(toUtf8(info.units, 128)) +
                    ", \"stepCount\": " + std::to_string(info.stepCount) +
                    ", \"default\": " + jsonNumber(info.defaultNormalizedValue) +
                    ", \"defaultText\": " + jsonString(valueText(session.controller, info.id, info.defaultNormalizedValue)) +
                    ", \"unitId\": " + std::to_string(info.unitId) + ", \"flags\": " + std::to_string(info.flags) +
                    ", \"points\": " + points + "}";
        }
    }
    json += "\n]\n";
    session.close();
    if (!session.ok)
    {
        session.printSteps("  ");
        detail = session.error;
        return false;
    }
    std::printf("%s", json.c_str());
    if (!writePath.empty())
        writeFile(writePath, json);
    if (!matchesFrozen(frozenPath, json, detail))
        return false;
    detail = "matches " + baseName(frozenPath);
    return true;
}

// --------------------------------------------------------------------------------------------------------------------
// Check: states, savestate

static bool freshState(Module& module, std::string& state, std::string& detail)
{
    const ClassIds ids = findClasses(module.factory);
    Session session;
    if (session.open(module.factory, ids))
        session.step("getState of a fresh instance", saveState(session, state));
    session.close();
    if (!session.ok)
    {
        session.printSteps("  ");
        detail = session.error;
        return false;
    }
    return true;
}

static bool checkStates(Module& module, const std::vector<std::string>& files, std::string& detail)
{
    int passed = 0;
    for (const std::string& file : files)
    {
        std::string want;
        if (!readFile(file, want))
        {
            detail = "could not read " + file;
            return false;
        }
        const ClassIds ids = findClasses(module.factory);
        Session session;
        std::string got;
        if (session.open(module.factory, ids) && session.step("setState", loadState(session, want)))
            session.step("getState", saveState(session, got));
        session.close();
        if (!session.ok)
        {
            session.printSteps("  ");
            detail = baseName(file) + ": " + session.error;
            return false;
        }
        if (got != want)
        {
            std::printf("STATE %s does not save back the same:\n  loaded: %s\n  saved:  %s\n", baseName(file).c_str(),
                        want.c_str(), got.c_str());
            detail = baseName(file) + " does not save back byte for byte";
            return false;
        }
        if (baseName(file) == "default.json")
        {
            std::string fresh;
            if (!freshState(module, fresh, detail))
                return false;
            if (fresh != want)
            {
                std::printf("STATE a fresh instance saves:\n  %s\n  default.json:\n  %s\n", fresh.c_str(), want.c_str());
                detail = "a fresh instance doesn't save default.json";
                return false;
            }
        }
        std::printf("STATE %s: %zu bytes, loads and saves back identical\n", baseName(file).c_str(), want.size());
        ++passed;
    }
    detail = std::to_string(passed) + " state(s) save back byte for byte";
    return true;
}

// --------------------------------------------------------------------------------------------------------------------
// Audio files: 32-bit float WAV, mono or stereo. Every runner is little-endian.

struct Audio
{
    double rate = 0;
    int channels = 0;
    std::vector<float> samples; // interleaved

    size_t frames() const { return channels > 0 ? samples.size() / static_cast<size_t>(channels) : 0; }
};

static uint32_t readLE(const std::string& data, size_t at, int bytes)
{
    uint32_t value = 0;
    for (int i = bytes - 1; i >= 0; --i)
        value = (value << 8) | static_cast<unsigned char>(data[at + static_cast<size_t>(i)]);
    return value;
}

static bool readWav(const std::string& path, Audio& audio, std::string& error)
{
    std::string data;
    if (!readFile(path, data))
    {
        error = "could not read " + path;
        return false;
    }
    if (data.size() < 12 || data.compare(0, 4, "RIFF") != 0 || data.compare(8, 4, "WAVE") != 0)
    {
        error = path + " is not a WAV file";
        return false;
    }
    bool haveFormat = false;
    uint32_t format = 0, channels = 0, rate = 0, bits = 0;
    for (size_t at = 12; at + 8 <= data.size();)
    {
        const std::string id = data.substr(at, 4);
        const size_t size = readLE(data, at + 4, 4);
        const size_t body = at + 8;
        if (body + size > data.size())
        {
            error = path + ": the " + id + " chunk runs past the end of the file";
            return false;
        }
        if (id == "fmt " && size >= 16)
        {
            format = readLE(data, body, 2);
            channels = readLE(data, body + 2, 2);
            rate = readLE(data, body + 4, 4);
            bits = readLE(data, body + 14, 2);
            if (format == 0xFFFE && size >= 40)
                format = readLE(data, body + 24, 2);
            haveFormat = true;
        }
        else if (id == "data")
        {
            if (!haveFormat || format != 3 || bits != 32 || channels < 1 || channels > 2 || rate == 0)
            {
                error = path + " is not a 32-bit float mono or stereo WAV file";
                return false;
            }
            audio.rate = rate;
            audio.channels = static_cast<int>(channels);
            audio.samples.resize(size / 4);
            std::memcpy(audio.samples.data(), data.data() + body, audio.samples.size() * 4);
            return true;
        }
        at = body + size + (size & 1);
    }
    error = path + " has no data chunk";
    return false;
}

static bool writeWav(const std::string& path, const Audio& audio)
{
    std::string out;
    const auto put = [&out](uint32_t value, int bytes) {
        for (int i = 0; i < bytes; ++i)
            out += static_cast<char>((value >> (8 * i)) & 0xFF);
    };
    const uint32_t channels = static_cast<uint32_t>(audio.channels);
    const uint32_t rate = static_cast<uint32_t>(std::lround(audio.rate));
    const uint32_t dataBytes = static_cast<uint32_t>(audio.samples.size() * 4);
    out += "RIFF";
    put(4 + (8 + 18) + (8 + 4) + (8 + dataBytes), 4);
    out += "WAVE";
    out += "fmt ";
    put(18, 4);
    put(3, 2); // WAVE_FORMAT_IEEE_FLOAT
    put(channels, 2);
    put(rate, 4);
    put(rate * channels * 4, 4);
    put(channels * 4, 2);
    put(32, 2);
    put(0, 2);
    out += "fact";
    put(4, 4);
    put(static_cast<uint32_t>(audio.frames()), 4);
    out += "data";
    put(dataBytes, 4);
    out.append(reinterpret_cast<const char*>(audio.samples.data()), dataBytes);
    return writeFile(path, out);
}

static std::string decibels(double level)
{
    if (level <= 0)
        return "-inf";
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f", 20.0 * std::log10(level));
    return text;
}

// --------------------------------------------------------------------------------------------------------------------
// Parameter changes a host hands to process(): none going in, and a list that collects what the plugin reports back.

class HostParamValueQueue final : public IParamValueQueue
{
public:
    ParamID id = 0;
    std::vector<std::pair<int32, ParamValue>> points;

    ParamID PLUGIN_API getParameterId() override { return id; }
    int32 PLUGIN_API getPointCount() override { return static_cast<int32>(points.size()); }

    tresult PLUGIN_API getPoint(int32 index, int32& sampleOffset, ParamValue& value) override
    {
        if (index < 0 || index >= getPointCount())
            return kInvalidArgument;
        sampleOffset = points[static_cast<size_t>(index)].first;
        value = points[static_cast<size_t>(index)].second;
        return kResultOk;
    }

    tresult PLUGIN_API addPoint(int32 sampleOffset, ParamValue value, int32& index) override
    {
        index = getPointCount();
        points.push_back(std::make_pair(sampleOffset, value));
        return kResultOk;
    }

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
    {
        if (FUnknownPrivate::iidEqual(iid, FUnknown_iid) || FUnknownPrivate::iidEqual(iid, IParamValueQueue_iid))
        {
            *obj = static_cast<IParamValueQueue*>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef() override { return 1000; }
    uint32 PLUGIN_API release() override { return 1000; }
};

class HostParameterChanges final : public IParameterChanges
{
public:
    std::deque<HostParamValueQueue> queues;

    int32 PLUGIN_API getParameterCount() override { return static_cast<int32>(queues.size()); }

    IParamValueQueue* PLUGIN_API getParameterData(int32 index) override
    {
        if (index < 0 || index >= getParameterCount())
            return nullptr;
        return &queues[static_cast<size_t>(index)];
    }

    IParamValueQueue* PLUGIN_API addParameterData(const ParamID& id, int32& index) override
    {
        for (size_t i = 0; i < queues.size(); ++i)
        {
            if (queues[i].id == id)
            {
                index = static_cast<int32>(i);
                return &queues[i];
            }
        }
        queues.emplace_back();
        queues.back().id = id;
        index = getParameterCount() - 1;
        return &queues.back();
    }

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
    {
        if (FUnknownPrivate::iidEqual(iid, FUnknown_iid) || FUnknownPrivate::iidEqual(iid, IParameterChanges_iid))
        {
            *obj = static_cast<IParameterChanges*>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef() override { return 1000; }
    uint32 PLUGIN_API release() override { return 1000; }
};

// --------------------------------------------------------------------------------------------------------------------
// Check: render. Crisp's smoothers glide to loaded values, so 1 s of silence plays first (the same on every platform,
// and Crisp's noise generator, with its fixed seed, runs through it the same way everywhere).

static const double kSettleSeconds = 1.0;

static bool checkRender(Module& module, const std::string& statePath, const std::string& inputPath,
                        const std::string& blockText, const std::string& outputPath, std::string& detail)
{
    std::string state;
    if (statePath != "-" && !readFile(statePath, state))
    {
        detail = "could not read " + statePath;
        return false;
    }
    Audio input;
    if (!readWav(inputPath, input, detail))
        return false;
    const int block = std::atoi(blockText.c_str());
    if (block < 2 || block > 8192)
    {
        detail = "the block size must be 2 to 8192";
        return false;
    }
    std::printf("RENDER state %s (%zu bytes); input %s: %zu frames, %d channel(s), %.0f Hz; blocks of %d; %.1f s of "
                "silence first\n",
                statePath == "-" ? "(default)" : baseName(statePath).c_str(), state.size(), baseName(inputPath).c_str(),
                input.frames(), input.channels, input.rate, block, kSettleSeconds);

    const ClassIds ids = findClasses(module.factory);
    Session session;
    IAudioProcessor* processor = nullptr;
    bool active = false, processing = false;

    bool ok = session.open(module.factory, ids);
    if (ok && (session.component->queryInterface(IAudioProcessor_iid, reinterpret_cast<void**>(&processor)) != kResultOk ||
               processor == nullptr))
    {
        processor = nullptr;
        ok = session.fail("the component has no IAudioProcessor");
    }
    if (ok)
        ok = session.step("canProcessSampleSize(32-bit float)", processor->canProcessSampleSize(kSample32));
    SpeakerArrangement inputArrangement = SpeakerArr::kStereo;
    SpeakerArrangement outputArrangement = SpeakerArr::kStereo;
    if (ok)
        ok = session.step("setBusArrangements(stereo in, stereo out)",
                          processor->setBusArrangements(&inputArrangement, 1, &outputArrangement, 1));
    if (ok)
        ok = session.step("activate the audio input bus", session.component->activateBus(kAudio, kInput, 0, true));
    if (ok)
        ok = session.step("activate the audio output bus", session.component->activateBus(kAudio, kOutput, 0, true));
    ProcessSetup setup{kRealtime, kSample32, block, input.rate};
    if (ok)
        ok = session.step("setupProcessing", processor->setupProcessing(setup));
    if (ok && !state.empty())
        ok = session.step("load the state (component, then controller)", loadState(session, state));
    if (ok)
        ok = active = session.step("setActive(true)", session.component->setActive(true));
    if (ok)
        ok = processing = session.step("setProcessing(true)", processor->setProcessing(true));

    Audio output;
    output.rate = input.rate;
    output.channels = 2;
    if (ok)
    {
        const size_t settleFrames = static_cast<size_t>(std::llround(kSettleSeconds * input.rate));
        const size_t totalFrames = settleFrames + input.frames();
        std::vector<float> inL(static_cast<size_t>(block)), inR(inL.size()), outL(inL.size()), outR(inL.size());
        float* inChannels[2] = {inL.data(), inR.data()};
        float* outChannels[2] = {outL.data(), outR.data()};
        AudioBusBuffers inBus{}, outBus{};
        inBus.numChannels = 2;
        inBus.channelBuffers32 = inChannels;
        outBus.numChannels = 2;
        outBus.channelBuffers32 = outChannels;
        HostParameterChanges inChanges, outChanges;
        ProcessContext context{};
        context.sampleRate = input.rate;
        context.tempo = 120;
        context.timeSigNumerator = 4;
        context.timeSigDenominator = 4;
        output.samples.reserve(input.frames() * 2);

        for (size_t position = 0; ok && position < totalFrames;)
        {
            const bool settling = position < settleFrames;
            const size_t end = settling ? settleFrames : totalFrames;
            const int32 count = static_cast<int32>(std::min<size_t>(static_cast<size_t>(block), end - position));
            for (int32 i = 0; i < count; ++i)
            {
                float left = 0, right = 0;
                if (!settling)
                {
                    const size_t frame = position - settleFrames + static_cast<size_t>(i);
                    left = input.samples[frame * static_cast<size_t>(input.channels)];
                    right = input.channels == 2 ? input.samples[frame * 2 + 1] : left;
                }
                inL[static_cast<size_t>(i)] = left;
                inR[static_cast<size_t>(i)] = right;
            }
            std::fill(outL.begin(), outL.end(), 0.0f);
            std::fill(outR.begin(), outR.end(), 0.0f);

            const int64 played = settling ? 0 : static_cast<int64>(position - settleFrames);
            context.state = ProcessContext::kTempoValid | ProcessContext::kTimeSigValid | ProcessContext::kContTimeValid |
                            ProcessContext::kProjectTimeMusicValid | (settling ? 0 : ProcessContext::kPlaying);
            context.projectTimeSamples = played;
            context.continousTimeSamples = static_cast<int64>(position);
            context.projectTimeMusic = static_cast<double>(played) / input.rate * (context.tempo / 60.0);

            ProcessData data;
            data.processMode = kRealtime;
            data.symbolicSampleSize = kSample32;
            data.numSamples = count;
            data.numInputs = 1;
            data.numOutputs = 1;
            data.inputs = &inBus;
            data.outputs = &outBus;
            data.inputParameterChanges = &inChanges;
            data.outputParameterChanges = &outChanges;
            data.processContext = &context;
            outChanges.queues.clear();
            const tresult result = processor->process(data);
            if (result != kResultOk)
            {
                ok = session.fail(std::string("process returned ") + resultName(result) + " at frame " +
                                  std::to_string(position));
                break;
            }
            if (!settling)
            {
                for (int32 i = 0; i < count; ++i)
                {
                    output.samples.push_back(outL[static_cast<size_t>(i)]);
                    output.samples.push_back(outR[static_cast<size_t>(i)]);
                }
            }
            position += static_cast<size_t>(count);
        }
    }

    if (processing)
        session.step("setProcessing(false)", processor->setProcessing(false));
    if (active)
        session.step("setActive(false)", session.component->setActive(false));
    if (processor != nullptr)
        processor->release();
    session.close();
    if (!session.ok)
    {
        session.printSteps("  ");
        detail = session.error;
        return false;
    }

    size_t nonFinite = 0;
    double peak = 0, sumOfSquares = 0;
    for (const float value : output.samples)
    {
        if (!std::isfinite(value))
        {
            ++nonFinite;
            continue;
        }
        peak = std::max(peak, static_cast<double>(std::fabs(value)));
        sumOfSquares += static_cast<double>(value) * value;
    }
    const double rms = output.samples.empty() ? 0 : std::sqrt(sumOfSquares / static_cast<double>(output.samples.size()));
    std::printf("OUTPUT %zu frames, peak %s dBFS, RMS %s dBFS\n", output.frames(), decibels(peak).c_str(),
                decibels(rms).c_str());
    if (nonFinite > 0)
    {
        detail = std::to_string(nonFinite) + " samples are NaN or infinite";
        return false;
    }
    if (!writeWav(outputPath, output))
    {
        detail = "could not write " + outputPath;
        return false;
    }
    detail = baseName(outputPath) + ": " + std::to_string(output.frames()) + " frames, peak " + decibels(peak) + " dBFS";
    return true;
}

// --------------------------------------------------------------------------------------------------------------------
// Check: nulltest

static bool checkNull(const std::string& pathA, const std::string& pathB, const std::string& limitText, std::string& detail)
{
    Audio a, b;
    if (!readWav(pathA, a, detail) || !readWav(pathB, b, detail))
        return false;
    if (a.rate != b.rate || a.channels != b.channels || a.samples.size() != b.samples.size())
    {
        detail = "the files differ in rate, channels or length";
        return false;
    }
    const double limit = std::atof(limitText.c_str());
    double worst = 0, sumOfSquares = 0, signalPeak = 0;
    size_t worstAt = 0, identical = 0;
    for (size_t i = 0; i < a.samples.size(); ++i)
    {
        const double difference = std::fabs(static_cast<double>(a.samples[i]) - static_cast<double>(b.samples[i]));
        if (difference > worst)
        {
            worst = difference;
            worstAt = i / static_cast<size_t>(a.channels);
        }
        if (a.samples[i] == b.samples[i])
            ++identical;
        sumOfSquares += difference * difference;
        signalPeak = std::max(signalPeak, static_cast<double>(std::fabs(a.samples[i])));
    }
    const double rms = a.samples.empty() ? 0 : std::sqrt(sumOfSquares / static_cast<double>(a.samples.size()));
    char text[256];
    std::snprintf(text, sizeof(text), "%s vs %s: residual peak %s dBFS (frame %zu), RMS %s dBFS, signal peak %s dBFS, "
                                      "%zu of %zu samples bit-identical",
                  baseName(pathA).c_str(), baseName(pathB).c_str(), decibels(worst).c_str(), worstAt,
                  decibels(rms).c_str(), decibels(signalPeak).c_str(), identical, a.samples.size());
    detail = text;
    return worst == 0 || 20.0 * std::log10(worst) <= limit;
}

// --------------------------------------------------------------------------------------------------------------------

static int usage()
{
    std::printf("usage:\n"
                "  vst3_check ids <bundle> <class-ids.txt>\n"
                "  vst3_check info <bundle> <plugin-info.json> [<write to>]\n"
                "  vst3_check params <bundle> <parameters.json> [<write to>]\n"
                "  vst3_check states <bundle> <state file>...\n"
                "  vst3_check savestate <bundle> <out file>\n"
                "  vst3_check render <bundle> <state file or -> <input.wav> <block size> <output.wav>\n"
                "  vst3_check nulltest <a.wav> <b.wav> <limit dBFS>\n");
    return 2;
}

int main(int argc, char* argv[])
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3)
        return usage();
    const std::string check = argv[1];
    std::string detail;
    bool ok = false;

    if (check == "nulltest")
    {
        if (argc != 5)
            return usage();
        ok = checkNull(argv[2], argv[3], argv[4], detail);
        std::printf("RESULT nulltest %s %s\n", ok ? "PASS" : "FAIL", detail.c_str());
        return ok ? 0 : 1;
    }

    const std::string bundle = argv[2];
    std::printf("HOST vst3_check on %s %s, bundle %s\n", CHECK_OS, CHECK_CPU, bundle.c_str());
    Module module;
    if (!module.load(bundle))
    {
        std::printf("RESULT %s FAIL the module did not load\n", check.c_str());
        return 1;
    }

    if (check == "ids" && argc == 4)
        ok = checkIds(module, argv[3], detail);
    else if (check == "info" && (argc == 4 || argc == 5))
        ok = checkInfo(module, argv[3], argc == 5 ? argv[4] : "", detail);
    else if (check == "params" && (argc == 4 || argc == 5))
        ok = checkParams(module, argv[3], argc == 5 ? argv[4] : "", detail);
    else if (check == "states" && argc >= 4)
        ok = checkStates(module, std::vector<std::string>(argv + 3, argv + argc), detail);
    else if (check == "savestate" && argc == 4)
    {
        std::string state;
        ok = freshState(module, state, detail) && writeFile(argv[3], state);
        if (ok)
            detail = std::to_string(state.size()) + " bytes to " + baseName(argv[3]) + ": " + state;
    }
    else if (check == "render" && argc == 7)
        ok = checkRender(module, argv[3], argv[4], argv[5], argv[6], detail);
    else
    {
        module.unload();
        return usage();
    }
    module.unload();
    std::printf("RESULT %s %s %s\n", check.c_str(), ok ? "PASS" : "FAIL", detail.c_str());
    return ok ? 0 : 1;
}
