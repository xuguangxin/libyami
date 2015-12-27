
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef ANDROID
#define __DISABLE_DRM__ 1
#endif

#include "interface/NativeDisplayHelper.h"
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <list>
#include <va/va.h>
#include <va/va_tpi.h>
#ifdef HAVE_VA_X11
#include <va/va_x11.h>
#endif
#ifndef __DISABLE_DRM__
#include <va/va_drm.h>
#endif
#include "common/log.h"
#include "common/lock.h"
#include "vaapi/vaapiutils.h"

///helper to share NativeDisplay.handle cross components
namespace YamiMediaCodec{

class YamiDisplayBase
{
    friend class YamiDisplayCache;
public:
    virtual ~YamiDisplayBase();
    //FIXME: add more create functions.
    static SharedPtr<YamiDisplayBase> create(const YamiDisplay&);
    void getNativeDisplay(NativeDisplay*);
protected:
    YamiDisplayBase();

    /// for display cache management.
    virtual bool isCompatible(const YamiDisplay& other) = 0;
    virtual bool initialize(const YamiDisplay&) = 0;

    //for init and deinit
    bool setNativeDisplay(const VADisplay&);
    void unsetNativeDisplay();
    void setYamiDisplay(const YamiDisplay&);

    //for isCompatible
    YamiDisplay   m_yamiDisplay;
private:
    NativeDisplay m_nativeDisplay;
    DISALLOW_COPY_AND_ASSIGN(YamiDisplayBase);
};

YamiDisplayBase::YamiDisplayBase()
{
    memset(&m_nativeDisplay, 0, sizeof(m_nativeDisplay));
    memset(&m_yamiDisplay, 0, sizeof(m_yamiDisplay));
}

YamiDisplayBase::~YamiDisplayBase()
{
    ASSERT(!m_nativeDisplay.handle && "you must call unsetNativeDisplay in derived class");
    if (m_yamiDisplay.free)
        m_yamiDisplay.free(m_yamiDisplay.user);
}

void YamiDisplayBase::getNativeDisplay(NativeDisplay* native)
{
    *native = m_nativeDisplay;
}

bool YamiDisplayBase::setNativeDisplay(const VADisplay& vaDisplay)
{
    int majorVersion, minorVersion;
    VAStatus vaStatus;
    vaStatus= vaInitialize(vaDisplay, &majorVersion, &minorVersion);
    if (!checkVaapiStatus(vaStatus, "vaInitialize"))
        return false;
    m_nativeDisplay.type = NATIVE_DISPLAY_VA;
    m_nativeDisplay.handle = (intptr_t)vaDisplay;
    return true;
}

void YamiDisplayBase::unsetNativeDisplay()
{
    VADisplay vaDisplay = (VADisplay)m_nativeDisplay.handle;
    if (vaDisplay) {
        checkVaapiStatus(vaTerminate(vaDisplay), "vaTerminate");
    }
    m_nativeDisplay.handle = 0;
}

void YamiDisplayBase::setYamiDisplay(const YamiDisplay& yamiDisplay)
{
    m_yamiDisplay = yamiDisplay;
}

#if __ENABLE_X11__
class YamiDisplayX11 : public YamiDisplayBase{
  public:
    YamiDisplayX11() :YamiDisplayBase(), m_xDisplay(NULL) { }
    virtual ~YamiDisplayX11() {
        unsetNativeDisplay();
        if (m_xDisplay)
            XCloseDisplay(m_xDisplay);
    };

    virtual bool initialize(const YamiDisplay& display) {
        ASSERT(display.type == YAMI_DISPLAY_X11 || display.type == YAMI_DISPLAY_AUTO);
        setYamiDisplay(display);
        Display* xDisplay;
        if (!display.handle || display.type == YAMI_DISPLAY_AUTO) {
            ASSERT(display.free == NULL);
            m_xDisplay = XOpenDisplay(NULL);
            if (!m_xDisplay) {
                ERROR("XOpenDisplay failed");
                return false;
            }
            xDisplay = m_xDisplay;
        } else {
            xDisplay = (Display*)display.handle;
        }
        return setNativeDisplay(vaGetDisplay(xDisplay));
    };

    virtual bool isCompatible(const YamiDisplay& display) {
        if (display.type == YAMI_DISPLAY_AUTO)
            return true;
        if (display.type == YAMI_DISPLAY_DRM && display.handle == -1)
            return true;
        if (display.type == YAMI_DISPLAY_X11
            && (!display.handle || display.handle == m_yamiDisplay.handle))
            return true;
        return false;
    }
private:
    Display* m_xDisplay;
};
#endif

#ifndef __DISABLE_DRM__
class YamiDisplayDrm : public YamiDisplayBase{
  public:
    YamiDisplayDrm() :YamiDisplayBase(){ };
    virtual ~YamiDisplayDrm() {
        unsetNativeDisplay();
        ::close(m_fd);
    };
    virtual bool initialize(const YamiDisplay& display) {
        ASSERT(display.type == YAMI_DISPLAY_DRM || display.type == YAMI_DISPLAY_AUTO);

        setYamiDisplay(display);

        int fd;
        if ((display.type == YAMI_DISPLAY_DRM && display.handle == -1)
            || display.type == YAMI_DISPLAY_AUTO) {
            ASSERT(display.free == NULL);
            m_fd = open("/dev/dri/renderD128", O_RDWR);
            if (m_fd < 0)
                m_fd = open("/dev/dri/card0", O_RDWR);
            if (m_fd < 0)
                return false;
            fd = m_fd;
        } else {
            fd = (int)display.handle;
        }
        return setNativeDisplay(vaGetDisplayDRM(fd));
    }

    bool isCompatible(const YamiDisplay& display) {
        if (display.type == YAMI_DISPLAY_AUTO)
            return true;
        if (display.type != YAMI_DISPLAY_DRM)
            return false;
        if (display.handle == 0 || display.handle == -1
            || display.handle == m_yamiDisplay.handle)
            return true;
        return false;
    }
private:
    int m_fd;
};
#endif

//display cache
class YamiDisplayCache
{
public:
    static SharedPtr<YamiDisplayCache> getInstance();
    SharedPtr<YamiDisplayBase> createDisplay(const YamiDisplay&);

    ~YamiDisplayCache() {}
private:
    YamiDisplayCache() {}

    std::list<WeakPtr<YamiDisplayBase> > m_cache;
    YamiMediaCodec::Lock m_lock;
};

SharedPtr<YamiDisplayCache> YamiDisplayCache::getInstance()
{
    static SharedPtr<YamiDisplayCache> cache;
    static YamiMediaCodec::Lock lock;

    YamiMediaCodec::AutoLock locker(lock);
    if (!cache) {
        SharedPtr<YamiDisplayCache> temp(new YamiDisplayCache);
        cache = temp;
    }
    return cache;
}

bool expired(const WeakPtr<YamiDisplayBase>& weak)
{
    return !weak.lock();
}

SharedPtr<YamiDisplayBase> YamiDisplayCache::createDisplay(const YamiDisplay& yamiDisplay)
{
    YamiMediaCodec::AutoLock locker(m_lock);

    m_cache.remove_if(expired);

    //lockup first
    std::list<WeakPtr<YamiDisplayBase> >::iterator it;
    for (it = m_cache.begin(); it != m_cache.end(); ++it) {
        SharedPtr<YamiDisplayBase> display  = (*it).lock();
        if (display->isCompatible(yamiDisplay)) {
            return display;
        }
    }

    //crate new one
    DEBUG("yamiDisplay: (type : %d), (handle : %ld)", yamiDisplay.type, yamiDisplay.handle);

    SharedPtr<YamiDisplayBase> display;
    switch (yamiDisplay.type) {
    case NATIVE_DISPLAY_AUTO:
        //fall through
#ifndef __DISABLE_DRM__
    case NATIVE_DISPLAY_DRM:
        display.reset(new YamiDisplayDrm());
        break;
#endif
#if __ENABLE_X11__
    case NATIVE_DISPLAY_X11:
        display.reset(new YamiDisplayX11());
        break;
#endif
    default:
        break;
    }

    if (!display) {
        ERROR("no display avaliable, maybe use wrong configure in project");
        return display;
    }

    if (!display->initialize(yamiDisplay)) {
        display.reset();
        return display;
    }
    WeakPtr<YamiDisplayBase> weak(display);
    m_cache.push_back(weak);
    return display;
}

SharedPtr<YamiDisplayBase> YamiDisplayBase::create(const YamiDisplay& display)
{
    return YamiDisplayCache::getInstance()->createDisplay(display);
}

class NativeDisplayImp : public NativeDisplay
{
public:
    NativeDisplayImp(const SharedPtr<YamiDisplayBase>& base)
        :m_base(base)
    {
        m_base->getNativeDisplay(this);
    }
private:
    SharedPtr<YamiDisplayBase> m_base;
};

}

using namespace YamiMediaCodec;

extern "C" {

NativeDisplay* createNativeDisplay(YamiDisplay* display)
{

    YamiDisplay d;
    if (!display) {
        memset(&d, 0, sizeof(d));
        d.type = YAMI_DISPLAY_AUTO;
    } else {
        d = *display;
    }
    SharedPtr<YamiDisplayBase> base = YamiDisplayBase::create(d);
    if (!base)
        return NULL;
    return new NativeDisplayImp(base);
}

/* we hold a reference count for YamiDisplay.user, releaseNativeDisplay
 * will unref it, when the ref count equals 0, we will call YamiDisplay::free
 */
void releaseNativeDisplay(NativeDisplay* p)
{
    NativeDisplayImp* display = static_cast<NativeDisplayImp*>(p);
    if (display)
        delete display;
}

} /* extern "C"*/
