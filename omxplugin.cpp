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
#include <string>
#include <strings.h>

#include <media/hardware/HardwareAPI.h>

OMX_COMPONENTTYPE * gOMXDrmPlayComponent = 0;

namespace android {

namespace {

/*
 * The component's own answer, corrected in the one place it will not take it
 * back.
 *
 * Its output port accepts a buffer count in a window of three: the minimum it
 * reports, and two above. Measured on this board, that is thirteen to fifteen
 * -- sixteen, seventeen, eighteen and nineteen are each refused with
 * OMX_ErrorUnsupportedSetting, and fifteen is taken.
 *
 * ACodec cannot reach it. It offers nBufferCountMin plus what the window holds
 * undequeued plus three, two, one and none, so the lowest it will ever offer
 * is min plus undequeued. A SurfaceView holds two, which lands exactly on
 * fifteen and plays. An ImageReader with two images holds three, which lands
 * on sixteen and never fits, whatever the resolution or the stream -- one
 * buffer short, every time. That is every attempt to play video in a browser:
 * the count is refused, the port disable that follows times out, and libnvomx
 * then dereferences null and takes media.codec with it.
 *
 * Nothing raises the ceiling. The blob carries two hundred and thirty-four
 * vendor indices and not one of them names a maximum buffer count; the ones
 * that touch buffers at all lower what is asked for rather than raise what is
 * allowed. And lowering it would not help: the window moves whole, so the
 * offer stays one above the top of it.
 *
 * What is left is the number ACodec starts its arithmetic from. Reporting one
 * less minimum puts its lowest offer on fifteen, which the component takes --
 * not an invented figure but the top of its own range, and the same count the
 * SurfaceView path already runs on.
 *
 * It has to be done on both sides. The component checks the minimum it is
 * handed for equality against its own, not as a bound, so a lowered figure
 * coming back would be refused for a second reason. It is therefore lowered on
 * the way out and put back on the way in, and the component is given exactly
 * what it reported.
 *
 * The originals are kept beside the component rather than in it:
 * pComponentPrivate belongs to the component, and a plugin has no business
 * there.
 */

typedef OMX_ERRORTYPE (*ParameterFunc)(
        OMX_HANDLETYPE, OMX_INDEXTYPE, OMX_PTR);

struct Watched {
    std::string name;
    ParameterFunc getParameter;
    ParameterFunc setParameter;

    /* Per port, the minimum as the component reported it, before one was
     * taken off for the caller. */
    std::map<OMX_U32, OMX_U32> reportedMin;
};

std::mutex gWatchedLock;
std::map<OMX_COMPONENTTYPE *, Watched> gWatched;

bool isAvcComponent(OMX_COMPONENTTYPE *component) {
    std::lock_guard<std::mutex> lock(gWatchedLock);

    std::map<OMX_COMPONENTTYPE *, Watched>::const_iterator it =
            gWatched.find(component);

    return it != gWatched.end()
            && strcasestr(it->second.name.c_str(), "h264") != NULL;
}

ParameterFunc originalGetParameter(OMX_COMPONENTTYPE *component) {
    std::lock_guard<std::mutex> lock(gWatchedLock);

    std::map<OMX_COMPONENTTYPE *, Watched>::const_iterator it =
            gWatched.find(component);

    return it == gWatched.end() ? NULL : it->second.getParameter;
}

ParameterFunc originalSetParameter(OMX_COMPONENTTYPE *component) {
    std::lock_guard<std::mutex> lock(gWatchedLock);

    std::map<OMX_COMPONENTTYPE *, Watched>::const_iterator it =
            gWatched.find(component);

    return it == gWatched.end() ? NULL : it->second.setParameter;
}

void rememberReportedMin(
        OMX_COMPONENTTYPE *component, OMX_U32 port, OMX_U32 min) {
    std::lock_guard<std::mutex> lock(gWatchedLock);

    std::map<OMX_COMPONENTTYPE *, Watched>::iterator it =
            gWatched.find(component);

    if (it != gWatched.end()) {
        it->second.reportedMin[port] = min;
    }
}

bool reportedMin(
        OMX_COMPONENTTYPE *component, OMX_U32 port, OMX_U32 *min) {
    std::lock_guard<std::mutex> lock(gWatchedLock);

    std::map<OMX_COMPONENTTYPE *, Watched>::const_iterator it =
            gWatched.find(component);

    if (it == gWatched.end()) {
        return false;
    }

    std::map<OMX_U32, OMX_U32>::const_iterator port_it =
            it->second.reportedMin.find(port);

    if (port_it == it->second.reportedMin.end()) {
        return false;
    }

    *min = port_it->second;
    return true;
}

bool isVideoOutputPort(const OMX_PARAM_PORTDEFINITIONTYPE *def) {
    return def->eDomain == OMX_PortDomainVideo && def->eDir == OMX_DirOutput;
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
          "enabled %d populated %d",
          err, def->nPortIndex,
          def->eDir == OMX_DirInput ? "in" : "out",
          def->nBufferCountActual, def->nBufferCountMin, def->nBufferAlignment,
          def->nBufferSize,
          def->format.video.nFrameWidth, def->format.video.nFrameHeight,
          def->format.video.nStride, def->format.video.nSliceHeight,
          def->format.video.eColorFormat, def->format.video.eCompressionFormat,
          def->bEnabled, def->bPopulated);
}

OMX_ERRORTYPE WatchedGetParameter(
        OMX_HANDLETYPE hComponent, OMX_INDEXTYPE nIndex, OMX_PTR pParam) {
    OMX_COMPONENTTYPE *component =
            static_cast<OMX_COMPONENTTYPE *>(hComponent);

    ParameterFunc original = originalGetParameter(component);

    if (original == NULL) {
        ALOGE("GetParameter on a component this plugin does not know");
        return OMX_ErrorInvalidComponent;
    }

    OMX_ERRORTYPE err = (*original)(hComponent, nIndex, pParam);

    /*
     * How large a frame the framework will let this decoder have.
     *
     * MediaCodecList takes the levels a component advertises, works a maximum
     * frame size out of them, and intersects that with whatever
     * media_codecs.xml declares -- the smaller of the two wins, so the XML
     * cannot lift a level that is set too low. A decoder ruled out that way is
     * never offered to the caller at all: at 640x360 the browser uses this one
     * and plays, and the moment the quality goes up it drops it without trying
     * and takes c2.android.avc.decoder, which then fails outright.
     *
     * The part is capable of more than it says. The chip's own manual has it
     * at "full motion playback of up to 1440P high-definition video", and
     * 2560x1440 is 14400 macroblocks -- past level 4.2, which allows 8704 and
     * therefore stops at 1080p, and inside level 5, which allows 22080. So
     * level 5 is what is reported, and the XML limits of 3840x2176 and 783360
     * blocks a second stay the real ceiling, since they are the smaller.
     *
     * Only the level is touched, and only for this codec. The profiles are
     * left exactly as the component lists them, and the original is written
     * down beside the change.
     */
    if (err == OMX_ErrorNone
            && nIndex == OMX_IndexParamVideoProfileLevelQuerySupported
            && pParam != NULL) {
        OMX_VIDEO_PARAM_PROFILELEVELTYPE *pl =
                static_cast<OMX_VIDEO_PARAM_PROFILELEVELTYPE *>(pParam);

        if (isAvcComponent(component) && pl->eLevel < OMX_VIDEO_AVCLevel5) {
            ALOGI("port %u entry %u: profile 0x%x level 0x%x, raised to 0x%x",
                  pl->nPortIndex, pl->nProfileIndex, pl->eProfile, pl->eLevel,
                  OMX_VIDEO_AVCLevel5);
            pl->eLevel = OMX_VIDEO_AVCLevel5;
        } else {
            ALOGI("port %u entry %u: profile 0x%x level 0x%x, left alone",
                  pl->nPortIndex, pl->nProfileIndex, pl->eProfile, pl->eLevel);
        }
    }

    if (err != OMX_ErrorNone
            || nIndex != OMX_IndexParamPortDefinition || pParam == NULL) {
        return err;
    }

    OMX_PARAM_PORTDEFINITIONTYPE *def =
            static_cast<OMX_PARAM_PORTDEFINITIONTYPE *>(pParam);

    /* A minimum of one cannot be lowered without saying something absurd, and
     * a port that small was never the problem. */
    if (!isVideoOutputPort(def) || def->nBufferCountMin < 2) {
        return err;
    }

    rememberReportedMin(component, def->nPortIndex, def->nBufferCountMin);
    def->nBufferCountMin--;

    return err;
}

OMX_ERRORTYPE WatchedSetParameter(
        OMX_HANDLETYPE hComponent, OMX_INDEXTYPE nIndex, OMX_PTR pParam) {
    OMX_COMPONENTTYPE *component =
            static_cast<OMX_COMPONENTTYPE *>(hComponent);

    ParameterFunc original = originalSetParameter(component);

    /* Nothing sensible to forward to. Saying so is better than pretending the
     * setting was taken. */
    if (original == NULL) {
        ALOGE("SetParameter on a component this plugin does not know");
        return OMX_ErrorInvalidComponent;
    }

    if (nIndex == OMX_IndexParamPortDefinition && pParam != NULL) {
        const OMX_PARAM_PORTDEFINITIONTYPE *def =
                static_cast<const OMX_PARAM_PORTDEFINITIONTYPE *>(pParam);

        OMX_U32 reported = 0;

        if (isVideoOutputPort(def)
                && reportedMin(component, def->nPortIndex, &reported)
                && def->nBufferCountMin != reported) {
            /* Give the component back the figure it gave us. The caller keeps
             * the one it was told, which is what its arithmetic was built on. */
            OMX_PARAM_PORTDEFINITIONTYPE restored = *def;
            restored.nBufferCountMin = reported;

            OMX_ERRORTYPE err = (*original)(hComponent, nIndex, &restored);

            if (err != OMX_ErrorNone) {
                reportRefusedPortDefinition(err, &restored);
            }
            return err;
        }
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

void watchComponent(OMX_COMPONENTTYPE *component, const char *name) {
    if (component == NULL
            || component->GetParameter == NULL
            || component->SetParameter == NULL) {
        return;
    }

    std::lock_guard<std::mutex> lock(gWatchedLock);

    Watched &watched = gWatched[component];
    watched.name = name == NULL ? "" : name;
    watched.getParameter = component->GetParameter;
    watched.setParameter = component->SetParameter;
    watched.reportedMin.clear();

    component->GetParameter = WatchedGetParameter;
    component->SetParameter = WatchedSetParameter;
}

void forgetComponent(OMX_COMPONENTTYPE *component) {
    std::lock_guard<std::mutex> lock(gWatchedLock);

    std::map<OMX_COMPONENTTYPE *, Watched>::iterator it =
            gWatched.find(component);

    if (it == gWatched.end()) {
        return;
    }

    /* Put the component back as it was before handing it to be freed. */
    component->GetParameter = it->second.getParameter;
    component->SetParameter = it->second.setParameter;

    gWatched.erase(it);
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
        watchComponent(*component, name);
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

