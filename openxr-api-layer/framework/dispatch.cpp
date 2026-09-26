// MIT License
//
// Copyright(c) 2021-2023 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "pch.h"

#include <layer.h>

#include "dispatch.h"
#include "log.h"
#include "logic/config.h"

using namespace openxr_api_layer::log;

namespace openxr_api_layer {

    // Track Vive dummy instance that was intentionally leaked (Vive runtime doesn't like mid-init destruction).
    XrInstance g_leakedDummyInstance = XR_NULL_HANDLE;

    namespace {
        // True when the active application requested a full layer bypass via
        // bypass_api_layer=1 in settings.cfg. Evaluated once, before any
        // instance is created, so a bypassed application is never disturbed by
        // the layer (no dummy instance, no extension filtering, no interception).
        bool g_bypassApiLayer = false;

        // When bypassing, the loader still routes every xrGetInstanceProcAddr()
        // request through this layer, because it is the topmost link of the
        // chain. This saves the next link's resolver (the next API layer, or
        // the loader terminator that reaches the runtime) so bypassed requests
        // can be forwarded instead of being answered from our dispatch table.
        PFN_xrGetInstanceProcAddr g_nextGetInstanceProcAddr = nullptr;

        // Extract the executable file name from a full path (no extension
        // stripping: config sections match the file name, e.g. "[exe:Game.exe]").
        std::string GetExecutableNameFromPath() {
            char path[_MAX_PATH];
            if (GetModuleFileNameA(nullptr, path, sizeof(path)) == 0) {
                return {};
            }
            const std::string_view fullPath(path);
            const size_t offset = fullPath.rfind('\\');
            return std::string(offset != std::string::npos ? fullPath.substr(offset + 1) : fullPath);
        }

        // Parse the layer configuration early (before xrCreateInstance) to
        // discover per-application bypass requests. This is a lightweight,
        // best-effort read: the full configuration is still parsed later in
        // xrGetSystem() with runtime/system context available.
        bool CheckEarlyBypassRequest() {
            const std::string executableName = GetExecutableNameFromPath();
            if (executableName.empty()) {
                return false;
            }

            FoveationConfig config;
            config.m_applicationExecutableName = executableName;
            config.LoadConfiguration(dllHome / "settings.cfg");
            config.LoadConfiguration(localAppData / "settings.cfg");
            return config.m_bypassApiLayer;
        }
    } // namespace

    // Entry point for creating the layer.
    XrResult XRAPI_CALL xrCreateApiLayerInstance(const XrInstanceCreateInfo* const instanceCreateInfo,
                                                 const struct XrApiLayerCreateInfo* const apiLayerInfo,
                                                 XrInstance* const instance) {
        TraceLocalActivity(local);
        QVF_TRACE_START(local, "xrCreateApiLayerInstance");

        if (!apiLayerInfo || apiLayerInfo->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO ||
            apiLayerInfo->structVersion != XR_API_LAYER_CREATE_INFO_STRUCT_VERSION ||
            apiLayerInfo->structSize != sizeof(XrApiLayerCreateInfo) || !apiLayerInfo->nextInfo ||
            apiLayerInfo->nextInfo->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO ||
            apiLayerInfo->nextInfo->structVersion != XR_API_LAYER_NEXT_INFO_STRUCT_VERSION ||
            apiLayerInfo->nextInfo->structSize != sizeof(XrApiLayerNextInfo) || !apiLayerInfo->nextInfo->layerName ||
            std::string_view(apiLayerInfo->nextInfo->layerName) != LAYER_NAME ||
            !apiLayerInfo->nextInfo->nextGetInstanceProcAddr || !apiLayerInfo->nextInfo->nextCreateApiLayerInstance) {
            ErrorLog("xrCreateApiLayerInstance validation failed\n");
            return XR_ERROR_INITIALIZATION_FAILED;
        }

        // Per-application early bypass: some applications crash during
        // instance/session setup when the layer is active. For those, become
        // fully transparent BEFORE creating any instance: skip the
        // dummy-instance probe, extension filtering and layer instantiation,
        // and forward the loader's request unchanged.
        g_bypassApiLayer = CheckEarlyBypassRequest();

        if (g_bypassApiLayer) {
            // Diagnostics: flush every line so the log survives a crash.
            // (Flush-per-line is enabled for the whole session when the log
            // opens - see xrNegotiateLoaderApiLayerInterface in entry.cpp.)
            Log(fmt::format("Bypassing layer for this application (bypass_api_layer=1)\n"));
            Log(fmt::format("Bypass: app requested OpenXR API version {}.{}.{}\n",
                            XR_VERSION_MAJOR(instanceCreateInfo->applicationInfo.apiVersion),
                            XR_VERSION_MINOR(instanceCreateInfo->applicationInfo.apiVersion),
                            XR_VERSION_PATCH(instanceCreateInfo->applicationInfo.apiVersion)));
            Log(fmt::format("Bypass: app name '{}' engine '{}'\n",
                            instanceCreateInfo->applicationInfo.applicationName,
                            instanceCreateInfo->applicationInfo.engineName));
            // Remember how to resolve functions below us before dropping out
            // of the chain (see xrGetInstanceProcAddr() below).
            g_nextGetInstanceProcAddr = apiLayerInfo->nextInfo->nextGetInstanceProcAddr;

            // The loader merges the extensions declared in our manifest into
            // the application's view of the runtime, even while we bypass.
            // An application that saw XR_VARJO_quad_views or
            // XR_VARJO_foveated_rendering in
            // xrEnumerateInstanceExtensionProperties() will request them, but
            // the runtime does not implement them (our layer does).
            // Forwarding such a request unchanged makes instance creation
            // fail with XR_ERROR_EXTENSION_NOT_PRESENT. Strip the extensions
            // we advertise so the request we forward matches a run with the
            // layer uninstalled.
            XrInstanceCreateInfo bypassCreateInfo = *instanceCreateInfo;
            std::vector<const char*> bypassEnabledExtensions;
            for (uint32_t i = 0; i < bypassCreateInfo.enabledExtensionCount; i++) {
                const std::string_view ext(bypassCreateInfo.enabledExtensionNames[i]);
                Log(fmt::format("Bypass: app requested extension: {}\n", ext));
                if (ext == XR_VARJO_QUAD_VIEWS_EXTENSION_NAME ||
                    ext == XR_VARJO_FOVEATED_RENDERING_EXTENSION_NAME) {
                    Log(fmt::format("Bypass: dropping extension request: {}\n", ext));
                    continue;
                }
                bypassEnabledExtensions.push_back(bypassCreateInfo.enabledExtensionNames[i]);
            }
            bypassCreateInfo.enabledExtensionNames = bypassEnabledExtensions.data();
            bypassCreateInfo.enabledExtensionCount = (uint32_t)bypassEnabledExtensions.size();

            // Skip our own layer in the chain: hand the filtered createInfo
            // to the next layer/runtime directly.
            XrApiLayerCreateInfo chainApiLayerInfo = *apiLayerInfo;
            chainApiLayerInfo.nextInfo = apiLayerInfo->nextInfo->next;
            XrResult bypassResult = apiLayerInfo->nextInfo->nextCreateApiLayerInstance(
                &bypassCreateInfo, &chainApiLayerInfo, instance);
            Log(fmt::format("Bypass: forwarded xrCreateApiLayerInstance -> {}\n", xr::ToCString(bypassResult)));

            if (XR_FAILED(bypassResult)) {
                // Identify the next link in the chain (the runtime below us).
                Log(fmt::format("Bypass: next chain link: {}\n",
                                apiLayerInfo->nextInfo->next && apiLayerInfo->nextInfo->next->layerName
                                    ? apiLayerInfo->nextInfo->next->layerName
                                    : "(end of chain - loader terminator)"));

                // Decisive experiment: retry with the API version downgraded
                // to 1.0. If this succeeds, the rejector is a 1.0-only
                // runtime (or loader) that cannot accept the app's 1.1.45
                // request - and the game gets a working instance either way.
                XrInstanceCreateInfo retryCreateInfo = bypassCreateInfo;
                retryCreateInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
                Log("Bypass: retrying create with API version 1.0.0\n");
                bypassResult = apiLayerInfo->nextInfo->nextCreateApiLayerInstance(
                    &retryCreateInfo, &chainApiLayerInfo, instance);
                Log(fmt::format("Bypass: retry (api 1.0.0) -> {}\n", xr::ToCString(bypassResult)));
            }
            return bypassResult;
        }

        // Dump the other layers.
        {
            auto info = apiLayerInfo->nextInfo;
            while (info) {
                QVF_TRACE_TAGGED(local, "xrCreateApiLayerInstance", TLArg(info->layerName, "LayerName"));
                Log(fmt::format("Using layer: {}\n", info->layerName));
                info = info->next;
            }
        }

        // Only request implicit extensions that are supported.
        //
        // While the OpenXR standard states that xrEnumerateInstanceExtensionProperties() can be queried without an
        // instance, this does not stand for API layers, since API layers implementation might rely on the next
        // xrGetInstanceProcAddr() pointer, which is not (yet) populated if no instance is created.
        // We create a dummy instance in order to do these checks.
        std::vector<std::string> filteredImplicitExtensions;
        if (!implicitExtensions.empty()) {
            XrInstance dummyInstance = XR_NULL_HANDLE;

            // Call the chain to create a dummy instance. Request no extensions in order to speed things up.
            XrInstanceCreateInfo dummyCreateInfo = *instanceCreateInfo;
            dummyCreateInfo.enabledExtensionCount = 0;

            XrApiLayerCreateInfo chainApiLayerInfo = *apiLayerInfo;
            chainApiLayerInfo.nextInfo = apiLayerInfo->nextInfo->next;

            if (XR_SUCCEEDED(apiLayerInfo->nextInfo->nextCreateApiLayerInstance(
                    &dummyCreateInfo, &chainApiLayerInfo, &dummyInstance))) {
                PFN_xrDestroyInstance xrDestroyInstance;
                CHECK_XRCMD(apiLayerInfo->nextInfo->nextGetInstanceProcAddr(
                    dummyInstance, "xrDestroyInstance", reinterpret_cast<PFN_xrVoidFunction*>(&xrDestroyInstance)));
                PFN_xrGetSystem xrGetSystem = nullptr;
                CHECK_XRCMD(apiLayerInfo->nextInfo->nextGetInstanceProcAddr(
                    dummyInstance, "xrGetSystem", reinterpret_cast<PFN_xrVoidFunction*>(&xrGetSystem)));
                PFN_xrGetSystemProperties xrGetSystemProperties = nullptr;
                CHECK_XRCMD(apiLayerInfo->nextInfo->nextGetInstanceProcAddr(
                    dummyInstance,
                    "xrGetSystemProperties",
                    reinterpret_cast<PFN_xrVoidFunction*>(&xrGetSystemProperties)));

                // Check the available extensions.
                PFN_xrEnumerateInstanceExtensionProperties xrEnumerateInstanceExtensionProperties;
                CHECK_XRCMD(apiLayerInfo->nextInfo->nextGetInstanceProcAddr(
                    dummyInstance,
                    "xrEnumerateInstanceExtensionProperties",
                    reinterpret_cast<PFN_xrVoidFunction*>(&xrEnumerateInstanceExtensionProperties)));

                uint32_t extensionsCount = 0;
                CHECK_XRCMD(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionsCount, nullptr));
                std::vector<XrExtensionProperties> extensions(extensionsCount, {XR_TYPE_EXTENSION_PROPERTIES});
                CHECK_XRCMD(xrEnumerateInstanceExtensionProperties(
                    nullptr, extensionsCount, &extensionsCount, extensions.data()));

                for (const std::string& extensionName : implicitExtensions) {
                    const auto matchExtensionName = [&](const XrExtensionProperties& properties) {
                        return properties.extensionName == extensionName;
                    };
                    if (std::find_if(extensions.cbegin(), extensions.cend(), matchExtensionName) != extensions.cend()) {
                        filteredImplicitExtensions.push_back(extensionName);
                    } else {
                        Log(fmt::format("Cannot satisfy implicit extension request: {}\n", extensionName));
                    }
                }

                // Workaround: the Vive runtime does not seem to like our flow of destroying the instance
                // mid-initialization. We skip destruction and track the dummy instance for later cleanup.
                if (xrGetSystem && xrGetSystemProperties) {
                    XrSystemGetInfo getInfo{XR_TYPE_SYSTEM_GET_INFO};
                    getInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
                    XrSystemId systemId;
                    if (XR_SUCCEEDED(xrGetSystem(dummyInstance, &getInfo, &systemId))) {
                        XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES};
                        CHECK_XRCMD(xrGetSystemProperties(dummyInstance, systemId, &systemProperties));
                        if (std::string(systemProperties.systemName).find("Vive Reality system") != std::string::npos) {
                            g_leakedDummyInstance = dummyInstance;
                            xrDestroyInstance = nullptr;
                        }
                    }
                }

                if (xrDestroyInstance) {
                    xrDestroyInstance(dummyInstance);
                }
            }
        }

        // Dump the requested extensions.
        XrInstanceCreateInfo chainInstanceCreateInfo = *instanceCreateInfo;
        std::vector<const char*> newEnabledExtensionNames;
        for (uint32_t i = 0; i < chainInstanceCreateInfo.enabledExtensionCount; i++) {
            const std::string_view ext(chainInstanceCreateInfo.enabledExtensionNames[i]);
            QVF_TRACE_TAGGED(local, "xrCreateApiLayerInstance", TLArg(ext.data(), "ExtensionName"));

            if (std::find(blockedExtensions.cbegin(), blockedExtensions.cend(), ext) == blockedExtensions.cend()) {
                Log(fmt::format("Requested extension: {}\n", ext));
                newEnabledExtensionNames.push_back(ext.data());
            } else {
                Log(fmt::format("Blocking extension: {}\n", ext));
            }
        }
        for (const auto& ext : filteredImplicitExtensions) {
            Log(fmt::format("Requesting extension: {}\n", ext));
            newEnabledExtensionNames.push_back(ext.c_str());
        }
        chainInstanceCreateInfo.enabledExtensionNames = newEnabledExtensionNames.data();
        chainInstanceCreateInfo.enabledExtensionCount = (uint32_t)newEnabledExtensionNames.size();

        // Call the chain to create the instance.
        XrApiLayerCreateInfo chainApiLayerInfo = *apiLayerInfo;
        chainApiLayerInfo.nextInfo = apiLayerInfo->nextInfo->next;
        XrResult result =
            apiLayerInfo->nextInfo->nextCreateApiLayerInstance(&chainInstanceCreateInfo, &chainApiLayerInfo, instance);
        if (XR_FAILED(result) && result == XR_ERROR_API_VERSION_UNSUPPORTED &&
            chainInstanceCreateInfo.applicationInfo.apiVersion > XR_MAKE_VERSION(1, 0, 0)) {
            // Automatic API-version fallback: some runtimes (e.g. SteamVR on
            // Windows) negotiate a 1.0-era API and reject newer requests with
            // XR_ERROR_API_VERSION_UNSUPPORTED. Retry once with the request
            // downgraded to 1.0 - the smallest possible downgrade, since
            // compatibility is decided at major.minor granularity. This only
            // runs after the original request already failed, so it can only
            // convert a hard failure into a working session.
            Log(fmt::format(
                "Instance creation failed with XR_ERROR_API_VERSION_UNSUPPORTED for API version {}.{}.{} - retrying with API version 1.0.0\n",
                XR_VERSION_MAJOR(chainInstanceCreateInfo.applicationInfo.apiVersion),
                XR_VERSION_MINOR(chainInstanceCreateInfo.applicationInfo.apiVersion),
                XR_VERSION_PATCH(chainInstanceCreateInfo.applicationInfo.apiVersion)));
            chainInstanceCreateInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
            result = apiLayerInfo->nextInfo->nextCreateApiLayerInstance(
                &chainInstanceCreateInfo, &chainApiLayerInfo, instance);
        }
        if (result == XR_SUCCESS) {
            // Create our layer.
            openxr_api_layer::GetInstance()->SetGetInstanceProcAddr(apiLayerInfo->nextInfo->nextGetInstanceProcAddr,
                                                                    *instance);
            openxr_api_layer::GetInstance()->SetGrantedExtensions(filteredImplicitExtensions);

            // Forward the xrCreateInstance() call to the layer.
            try {
                result = openxr_api_layer::GetInstance()->xrCreateInstance(instanceCreateInfo);
            } catch (std::exception& exc) {
                QVF_TRACE_TAGGED(local, "xrCreateInstance_Error", TLArg(exc.what(), "Error"));
                ErrorLog(fmt::format("xrCreateInstance: {}\n", exc.what()));
                result = XR_ERROR_RUNTIME_FAILURE;
            } catch (...) {
                QVF_TRACE_TAGGED(local, "xrCreateInstance_Error", TLArg("Unknown exception", "Error"));
                ErrorLog(fmt::format("xrCreateInstance: Unknown exception\n"));
                result = XR_ERROR_RUNTIME_FAILURE;
            }

            // Cleanup attempt before returning an error.
            if (XR_FAILED(result)) {
                PFN_xrDestroyInstance xrDestroyInstance = nullptr;
                if (XR_SUCCEEDED(apiLayerInfo->nextInfo->nextGetInstanceProcAddr(
                        *instance, "xrDestroyInstance", reinterpret_cast<PFN_xrVoidFunction*>(&xrDestroyInstance)))) {
                    xrDestroyInstance(*instance);
                }

                // If the real instance creation failed, also destroy the temporary
                // dummy instance created for Vive detection so it doesn't leak.
                if (g_leakedDummyInstance != XR_NULL_HANDLE) {
                    PFN_xrDestroyInstance destroyDummy = nullptr;
                    apiLayerInfo->nextInfo->nextGetInstanceProcAddr(
                        *instance, "xrDestroyInstance", reinterpret_cast<PFN_xrVoidFunction*>(&destroyDummy));
                    if (destroyDummy) {
                        destroyDummy(g_leakedDummyInstance);
                    }
                    g_leakedDummyInstance = XR_NULL_HANDLE;
                }
            }
        }

        // Clean up leaked Vive dummy instance now that the real instance is created.
        if (XR_SUCCEEDED(result) && g_leakedDummyInstance != XR_NULL_HANDLE) {
            PFN_xrDestroyInstance destroyDummy = nullptr;
            apiLayerInfo->nextInfo->nextGetInstanceProcAddr(
                *instance, "xrDestroyInstance", reinterpret_cast<PFN_xrVoidFunction*>(&destroyDummy));
            if (destroyDummy) {
                destroyDummy(g_leakedDummyInstance);
            }
            g_leakedDummyInstance = XR_NULL_HANDLE;
        }

        QVF_TRACE_STOP(local, "xrCreateApiLayerInstance", TLArg(xr::ToCString(result), "Result"));
        if (XR_FAILED(result)) {
            ErrorLog(fmt::format("xrCreateApiLayerInstance failed with {}\n", xr::ToCString(result)));
        }

        return result;
    }

    // Forward the xrGetInstanceProcAddr() call to the dispatcher.
    XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
        TraceLocalActivity(local);
        QVF_TRACE_START(local, "xrGetInstanceProcAddr");

        // When the layer is bypassed for this application, forward the request
        // to the next layer/runtime without touching our dispatcher: the
        // application never created a layer instance, so our dispatch table is
        // not populated and must not be consulted.
        if (g_bypassApiLayer) {
            // The loader routes every xrGetInstanceProcAddr() request through
            // the topmost layer of the chain, which is us. Our dispatch table
            // is not populated in bypass mode, so forward the request to the
            // next link (the next API layer, or the loader terminator that
            // resolves from the runtime). This is what makes the layer fully
            // transparent: the application gets the runtime's own functions.
            if (g_nextGetInstanceProcAddr != nullptr) {
                const XrResult bypassGipaResult = g_nextGetInstanceProcAddr(instance, name, function);
                LogDebug("Bypass: xrGetInstanceProcAddr({}) -> {}\n", name ? name : "(null)", xr::ToCString(bypassGipaResult));
                return bypassGipaResult;
            }
            // Fallback when the chain was never wired up (should not happen:
            // xrCreateApiLayerInstance() always runs first). Only
            // xrGetInstanceProcAddr itself can be answered without a dispatch
            // table.
            if (std::string_view(name) == "xrGetInstanceProcAddr") {
                *function = reinterpret_cast<PFN_xrVoidFunction>(xrGetInstanceProcAddr);
                return XR_SUCCESS;
            }
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }

        XrResult result;
        try {
            if (std::string_view(name) != "xrEnumerateInstanceExtensionProperties") {
                result = openxr_api_layer::GetInstance()->xrGetInstanceProcAddr(instance, name, function);
                LogDebug("gipa: {} -> {}\n", name ? name : "(null)", xr::ToCString(result));
            } else {
                // We must always call our xrEnumerateInstanceExtensionProperties() override in order to be consistent
                // with the list of extensions defined in our JSON.
                result = openxr_api_layer::GetInstance()->xrGetInstanceProcAddrInternal(instance, name, function);
            }
        } catch (std::exception& exc) {
            QVF_TRACE_TAGGED(local, "xrGetInstanceProcAddr_Error", TLArg(exc.what(), "Error"));
            ErrorLog(fmt::format("xrGetInstanceProcAddr: {}\n", exc.what()));
            result = XR_ERROR_RUNTIME_FAILURE;
        } catch (...) {
            QVF_TRACE_TAGGED(local, "xrGetInstanceProcAddr_Error", TLArg("Unknown exception", "Error"));
            ErrorLog(fmt::format("xrGetInstanceProcAddr: Unknown exception\n"));
            result = XR_ERROR_RUNTIME_FAILURE;
        }

        QVF_TRACE_STOP(local, "xrGetInstanceProcAddr", TLArg(xr::ToCString(result), "Result"));

        return result;
    }

} // namespace openxr_api_layer
