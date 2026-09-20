/*
 * Copyright (C) 2009 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "NVOMXPlugin"
#include <utils/Log.h>

#include "omxplugin.h"
#include <dlfcn.h>

#include <map>
#include <mutex>

#include <media/hardware/HardwareAPI.h>

OMX_COMPONENTTYPE * gOMXDrmPlayComponent = 0;

namespace android {

namespace {

/*
 * What the component was asked for, when it says no.
 *
 * ACodec can only report its own reading of a refusal. Asked to set the
 * output port's buffer count, it hands back the port definition the component
 * gave it with one field changed, and if that comes back
 * OMX_ErrorUnsupportedSetting it says "setting nBufferCountActual to 19
 * failed" -- which is a description of the request, not of the objection. On
 * this board every count it offers is refused, the port disable that follows
 * times out, and libnvomx then dereferences null and takes media.codec with
 * it, so what the component actually objected to is worth knowing exactly.
 *
 * The plugin is where it can be seen. It hands the raw OMX_COMPONENTTYPE out
 * of makeComponentInstance, function pointers and all, so SetParameter can be
 * replaced by one that forwards and, on a refusal, writes down the structure
 * as it was sent. Nothing is altered on the way through.
 *
 * The originals are kept beside the component rather than in it:
 * pComponentPrivate belongs to the component, and a plugin has no business
 * there.
 */

typedef OMX_ERRORTYPE (*SetParameterFunc)(
        OMX_HANDLETYPE, OMX_INDEXTYPE, OMX_PTR);

std::mutex gOriginalsLock;
std::map<OMX_COMPONENTTYPE *, SetParameterFunc> gOriginalSetParameter;

SetParameterFunc originalSetParameter(OMX_COMPONENTTYPE *component) {
    std::lock_guard<std::mutex> lock(gOriginalsLock);

    std::map<OMX_COMPONENTTYPE *, SetParameterFunc>::const_iterator it =
            gOriginalSetParameter.find(component);

    return it == gOriginalSetParameter.end() ? NULL : it->second;
}

void reportRefusedPortDefinition(
        OMX_ERRORTYPE err, const OMX_PARAM_PORTDEFINITIONTYPE *def) {
    if (def->eDomain != OMX_PortDomainVideo) {
        ALOGE("port definition refused with 0x%08x: port %u, domain %d, "
              "buffers actual %u min %u size %u, enabled %d populated %d",
              err, def->nPortIndex, def->eDomain, def->nBufferCountActual,
              def->nBufferCountMin, def->nBufferSize,
              def->bEnabled, def->bPopulated);
        return;
    }

    ALOGE("port definition refused with 0x%08x: port %u %s, "
          "buffers actual %u min %u align %u size %u, "
          "%ux%u stride %d slice %u, colour 0x%x compression 0x%x, "
          "bitrate %u framerate 0x%x, enabled %d populated %d tunneled %d",
          err, def->nPortIndex,
          def->eDir == OMX_DirInput ? "in" : "out",
          def->nBufferCountActual, def->nBufferCountMin, def->nBufferAlignment,
          def->nBufferSize,
          def->format.video.nFrameWidth, def->format.video.nFrameHeight,
          def->format.video.nStride, def->format.video.nSliceHeight,
          def->format.video.eColorFormat, def->format.video.eCompressionFormat,
          def->format.video.nBitrate, def->format.video.xFramerate,
          def->bEnabled, def->bPopulated, def->bBuffersContiguous);
}

OMX_ERRORTYPE WatchedSetParameter(
        OMX_HANDLETYPE hComponent, OMX_INDEXTYPE nIndex, OMX_PTR pParam) {
    OMX_COMPONENTTYPE *component =
            static_cast<OMX_COMPONENTTYPE *>(hComponent);

    SetParameterFunc original = originalSetParameter(component);

    /* Nothing sensible to forward to. Saying so is better than pretending the
     * setting was taken. */
    if (original == NULL) {
        ALOGE("SetParameter on a component this plugin does not know");
        return OMX_ErrorInvalidComponent;
    }

    OMX_ERRORTYPE err = (*original)(hComponent, nIndex, pParam);

    if (err == OMX_ErrorNone) {
        return err;
    }

    if (nIndex == OMX_IndexParamPortDefinition && pParam != NULL) {
        reportRefusedPortDefinition(
                err, static_cast<const OMX_PARAM_PORTDEFINITIONTYPE *>(pParam));
    } else {
        ALOGE("SetParameter(0x%08x) refused with 0x%08x", nIndex, err);
    }

    return err;
}

void watchComponent(OMX_COMPONENTTYPE *component) {
    if (component == NULL || component->SetParameter == NULL) {
        return;
    }

    std::lock_guard<std::mutex> lock(gOriginalsLock);

    gOriginalSetParameter[component] = component->SetParameter;
    component->SetParameter = WatchedSetParameter;
}

void forgetComponent(OMX_COMPONENTTYPE *component) {
    std::lock_guard<std::mutex> lock(gOriginalsLock);

    std::map<OMX_COMPONENTTYPE *, SetParameterFunc>::iterator it =
            gOriginalSetParameter.find(component);

    if (it == gOriginalSetParameter.end()) {
        return;
    }

    /* Put the component back as it was before handing it to be freed. */
    component->SetParameter = it->second;
    gOriginalSetParameter.erase(it);
}

}  // namespace

OMXPluginBase *createOMXPlugin() {
    return new NVOMXPlugin;
}

NVOMXPlugin::NVOMXPlugin()
    : mLibHandle(dlopen("libnvomx.so", RTLD_NOW)),
      mInit(NULL),
      mDeinit(NULL),
      mComponentNameEnum(NULL),
      mGetHandle(NULL),
      mFreeHandle(NULL),
      mGetRolesOfComponentHandle(NULL) {
    if (mLibHandle != NULL) {
        mInit = (InitFunc)dlsym(mLibHandle, "OMX_Init");
        mDeinit = (DeinitFunc)dlsym(mLibHandle, "OMX_Deinit");

        mComponentNameEnum =
            (ComponentNameEnumFunc)dlsym(mLibHandle, "OMX_ComponentNameEnum");

        mGetHandle = (GetHandleFunc)dlsym(mLibHandle, "OMX_GetHandle");
        mFreeHandle = (FreeHandleFunc)dlsym(mLibHandle, "OMX_FreeHandle");

        mGetRolesOfComponentHandle =
            (GetRolesOfComponentFunc)dlsym(
                    mLibHandle, "OMX_GetRolesOfComponent");

        (*mInit)();
    }
}

NVOMXPlugin::~NVOMXPlugin() {
    if (mLibHandle != NULL) {
        (*mDeinit)();

        dlclose(mLibHandle);
        mLibHandle = NULL;
    }
}

OMX_ERRORTYPE NVOMXPlugin::makeComponentInstance(
        const char *name,
        const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData,
        OMX_COMPONENTTYPE **component)
{
    if (mLibHandle == NULL) {
        return OMX_ErrorUndefined;
    }

    OMX_ERRORTYPE err = (*mGetHandle)(
            reinterpret_cast<OMX_HANDLETYPE *>(component),
            const_cast<char *>(name),
            appData, const_cast<OMX_CALLBACKTYPE *>(callbacks));

    if (err == OMX_ErrorNone) {
        watchComponent(*component);
    }

    if (!strncmp(name, "OMX.Nvidia.drm.play", strlen("OMX.Nvidia.drm.play")))
    {
        gOMXDrmPlayComponent = *component;
    }

    return err;
}

OMX_ERRORTYPE NVOMXPlugin::destroyComponentInstance(
        OMX_COMPONENTTYPE *component) {
    if (mLibHandle == NULL) {
        return OMX_ErrorUndefined;
    }

    if (component == gOMXDrmPlayComponent)
    {
        gOMXDrmPlayComponent = 0;
    }

    forgetComponent(component);

    return (*mFreeHandle)(reinterpret_cast<OMX_HANDLETYPE *>(component));
}

OMX_ERRORTYPE NVOMXPlugin::enumerateComponents(
        OMX_STRING name,
        size_t size,
        OMX_U32 index) {
    if (mLibHandle == NULL) {
        return OMX_ErrorUndefined;
    }

    return (*mComponentNameEnum)(name, size, index);
}

OMX_ERRORTYPE NVOMXPlugin::getRolesOfComponent(
        const char *name,
        Vector<String8> *roles) {
    roles->clear();

    if (mLibHandle == NULL) {
        return OMX_ErrorUndefined;
    }

    OMX_U32 numRoles = 0;
    OMX_ERRORTYPE err = (*mGetRolesOfComponentHandle)(
            const_cast<OMX_STRING>(name), &numRoles, NULL);

    if (err != OMX_ErrorNone) {
        return err;
    }

    if (numRoles > 0) {
        OMX_U8 **array = new OMX_U8 *[numRoles];
        for (OMX_U32 i = 0; i < numRoles; ++i) {
            array[i] = new OMX_U8[OMX_MAX_STRINGNAME_SIZE];
        }

        OMX_U32 numRoles2 = numRoles;
        err = (*mGetRolesOfComponentHandle)(
                const_cast<OMX_STRING>(name), &numRoles2, array);

        if (err != OMX_ErrorNone ||
            numRoles != numRoles2) {

            if (numRoles != numRoles2 &&
                err == OMX_ErrorNone) {
                err = OMX_ErrorUndefined;
            }
            return err;
        }

        for (OMX_U32 i = 0; i < numRoles; ++i) {
            String8 s((const char *)array[i]);
            roles->push(s);

            delete[] array[i];
            array[i] = NULL;
        }

        delete[] array;
        array = NULL;
    }

    return OMX_ErrorNone;
}

}  // namespace android

