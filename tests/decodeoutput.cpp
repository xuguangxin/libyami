/*
 *  decodeoutput.cpp - decode outputs
 *
 *  Copyright (C) 2011-2014 Intel Corporation
 *    Author: Halley Zhao<halley.zhao@intel.com>
 *            Xu Guangxin<guangxin.xu@intel.com>
 *            Liu Yangbin<yangbinx.liu@intel.com>
 *            Lin Hai<hai1.lin@intel.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public License
 *  as published by the Free Software Foundation; either version 2.1
 *  of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free
 *  Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301 USA
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "decodeoutput.h"
#include "common/log.h"
#include "common/utils.h"
#include "vaapi/vaapiutils.h"

#if __ENABLE_MD5__
#include <openssl/md5.h>
#endif

#ifdef __ENABLE_X11__
#include <X11/Xlib.h>
#include <va/va_x11.h>
#endif

#ifdef __ENABLE_TESTS_GLES__
#include "./egl/gles2_help.h"
#include "egl/egl_util.h"
#include "egl/egl_vaapi_image.h"
#endif

#include <va/va.h>
#include <va/va_drmcommon.h>
#include <sys/stat.h>
#include <sstream>
#include <stdio.h>
#include <assert.h>

using YamiMediaCodec::checkVaapiStatus;

bool DecodeOutput::init()
{
    m_nativeDisplay.reset(new NativeDisplay);
    m_nativeDisplay->type = NATIVE_DISPLAY_VA;
    m_nativeDisplay->handle = (intptr_t)*m_vaDisplay;
    if (!m_vaDisplay || !m_nativeDisplay) {
        ERROR("init display error");
        return false;
    }
    return true;
}

SharedPtr<NativeDisplay> DecodeOutput::nativeDisplay()
{
    return m_nativeDisplay;
}

bool DecodeOutput::setVideoSize(uint32_t with, uint32_t height)
{
    m_width = with;
    m_height = height;
    return true;
}

class DecodeOutputNull : public DecodeOutput {
public:
    DecodeOutputNull() {}
    ~DecodeOutputNull() {}
    bool init();
    bool output(SharedPtr<VideoFrame>& frame);
};

bool DecodeOutputNull::init()
{
    m_vaDisplay = createVADisplay();
    if (!m_vaDisplay)
        return false;
    return DecodeOutput::init();
}

bool DecodeOutputNull::output(SharedPtr<VideoFrame>& frame)
{
    VAStatus status = vaSyncSurface(*m_vaDisplay, (VASurfaceID)frame->surface);
    return checkVaapiStatus(status, "vaSyncSurface");
}

class ColorConvert {
public:
    ColorConvert(const SharedPtr<VADisplay>& display, uint32_t fourcc)
        : m_width(0)
        , m_height(0)
        , m_destFourcc(fourcc)
        , m_display(display)

    {
        m_allocator.reset(new PooledFrameAllocator(m_display, 3));
    }
    SharedPtr<VideoFrame> convert(const SharedPtr<VideoFrame>& frame)
    {
        if (frame->fourcc == m_destFourcc)
            return frame;
        SharedPtr<VideoFrame> dstFrame;
        if (frame->fourcc != VA_FOURCC_NV12) {
            fprintf(stderr, "cannot convert fourcc(%u) to fourcc(%u)\n", frame->fourcc, m_destFourcc);
            return dstFrame;
        }
        VAImage srcImage, dstImage;
        char *srcBuf, *destBuf;

        srcBuf = map(frame->surface, srcImage);
        if (!srcBuf)
            return dstFrame;

        if (!initAllocator(srcImage.width, srcImage.height)) {
            unmap(srcImage);
            return dstFrame;
        }
        dstFrame = m_allocator->alloc();
        memcpy(&dstFrame->crop, &frame->crop, sizeof(frame->crop));
        dstFrame->fourcc = m_destFourcc;

        destBuf = map(dstFrame->surface, dstImage);
        if (!destBuf) {
            unmap(srcImage);
            dstFrame.reset();
            return dstFrame;
        }
        copy(frame->crop.width, frame->crop.height, srcBuf, srcImage, destBuf, dstImage);

        unmap(srcImage);
        unmap(dstImage);
        return dstFrame;
    }

private:
    char* map(intptr_t surface, VAImage& image)
    {
        char* p = NULL;
        VAStatus status = vaDeriveImage(*m_display, (VASurfaceID)surface, &image);
        if (!checkVaapiStatus(status, "vaDeriveImage"))
            return NULL;
        status = vaMapBuffer(*m_display, image.buf, (void**)&p);
        if (!checkVaapiStatus(status, "vaMapBuffer")) {
            checkVaapiStatus(vaDestroyImage(*m_display, image.image_id), "vaDestroyImage");
            return NULL;
        }
        return p;
    }
    void unmap(const VAImage& image)
    {
        checkVaapiStatus(vaUnmapBuffer(*m_display, image.buf), "vaUnmapBuffer");
        checkVaapiStatus(vaDestroyImage(*m_display, image.image_id), "vaDestroyImage");
    }

    //support i420,yv12
    void copy(uint32_t width, uint32_t height, char* src, VAImage& srcImage, char* dst, VAImage& dstImage)
    {
        //android libva only support yv12. we set dstImage as yv12
        //and use m_destFourcc for real dest format.
        ASSERT(srcImage.format.fourcc == VA_FOURCC_NV12 && dstImage.format.fourcc == VA_FOURCC_YV12);
        uint32_t planes, byteWidth[3], byteHeight[3];
        if (!getPlaneResolution(srcImage.format.fourcc, width, height, byteWidth, byteHeight, planes)) {
            ERROR("get plane reoslution failed");
            return;
        }
        uint32_t row, col, index;
        char *y_src, *u_src;
        char *y_dst, *u_dst, *v_dst;
        y_src = src + srcImage.offsets[0];
        u_src = src + srcImage.offsets[1];

        //copy y
        y_dst = dst + dstImage.offsets[0];
        for (row = 0; row < byteHeight[0]; ++row) {
            memcpy(y_dst, y_src, byteWidth[0]);
            y_dst += dstImage.pitches[0];
            y_src += srcImage.pitches[0];
        }

        //copy uv
        int u, v;
        u = (m_destFourcc == VA_FOURCC_YV12) ? 2 : 1;
        v = (m_destFourcc == VA_FOURCC_YV12) ? 1 : 2;
        u_dst = dst + dstImage.offsets[u];
        v_dst = dst + dstImage.offsets[v];
        for (row = 0; row < byteHeight[1]; ++row) {
            col = 0, index = 0;
            while (index < byteWidth[1]) {
                u_dst[col] = u_src[index];
                v_dst[col] = u_src[index + 1];
                index += 2;
                col++;
            }
            u_src += srcImage.pitches[1];
            u_dst += dstImage.pitches[u];
            v_dst += dstImage.pitches[v];
        }
    }

    bool initAllocator(uint32_t width, uint32_t height)
    {
        if (width > m_width || height > m_height) {
            m_width = width > m_width ? width : m_width;
            m_height = height > m_height ? height : m_height;
            //android driver not supported VA_FOURCC_I420, so use VA_FOURCC_YV12
            //to replace VA_FOURCC_I420. But m_destFourcc keep the true fourcc
            if (!m_allocator->setFormat(VA_FOURCC_YV12, m_width, m_height)) {
                m_allocator.reset();
                fprintf(stderr, "m_allocator setFormat failed\n");
                return false;
            }
        }
        return true;
    }

    uint32_t m_width;
    uint32_t m_height;
    uint32_t m_destFourcc;
    SharedPtr<VADisplay> m_display;
    SharedPtr<FrameAllocator> m_allocator;
};

class DecodeOutputFile : public DecodeOutput {
public:
    DecodeOutputFile(const char* outputFile, const char* inputFile, uint32_t fourcc)
        : m_destFourcc(fourcc)
        , m_inputFile(inputFile)
        , m_outputFile(outputFile)
    {
    }
    DecodeOutputFile() {}
    virtual bool init();
    virtual bool output(SharedPtr<VideoFrame>& frame);

protected:
    virtual bool writeFrame(SharedPtr<VideoFrame>& frame) = 0;
    uint32_t m_destFourcc;
    const char* m_inputFile;
    const char* m_outputFile;

private:
    SharedPtr<ColorConvert> m_convert;
};

bool DecodeOutputFile::init()
{
    m_vaDisplay = createVADisplay();
    if (!m_vaDisplay)
        return false;
    if (m_destFourcc != VA_FOURCC_NV12) {
        if (m_destFourcc != VA_FOURCC_YV12 && m_destFourcc != VA_FOURCC_I420) {
            fprintf(stderr, "Output Fourcc isn't supported\n");
            return false;
        }
        m_convert.reset(new ColorConvert(m_vaDisplay, m_destFourcc));
    }
    return DecodeOutput::init();
}

bool DecodeOutputFile::output(SharedPtr<VideoFrame>& frame)
{
    if (!setVideoSize(frame->crop.width, frame->crop.height))
        return false;

    if (m_convert)
        frame = m_convert->convert(frame);
    return writeFrame(frame);
}

class DecodeOutputDump : public DecodeOutputFile {
public:
    DecodeOutputDump(const char* outputFile, const char* inputFile, uint32_t fourcc)
        : DecodeOutputFile(outputFile, inputFile, fourcc)
    {
    }

protected:
    bool setVideoSize(uint32_t width, uint32_t height);
    virtual bool writeFrame(SharedPtr<VideoFrame>& frame);

private:
    std::string getOutputFileName(uint32_t, uint32_t);
    SharedPtr<VppOutput> m_output;
};

std::string DecodeOutputDump::getOutputFileName(uint32_t width, uint32_t height)
{
    std::ostringstream name;
    struct stat buf;
    int r = stat(m_outputFile, &buf);
    if (r == 0 && buf.st_mode & S_IFDIR) {
        const char* baseFileName = m_inputFile;
        const char* s = strrchr(m_inputFile, '/');
        if (s)
            baseFileName = s + 1;
        name << m_outputFile << "/" << baseFileName;
        char* ch = reinterpret_cast<char*>(&m_destFourcc);
        name << "_" << width << 'x' << height << "." << ch[0] << ch[1] << ch[2] << ch[3];
    } else {
        name << m_outputFile;
    }
    return name.str();
}

bool DecodeOutputDump::setVideoSize(uint32_t width, uint32_t height)
{
    if (!m_output) {
        std::string name = getOutputFileName(width, height);
        m_output = VppOutput::create(name.c_str(), m_destFourcc, m_width, m_height);
        SharedPtr<VppOutputFile> outputFile = std::tr1::dynamic_pointer_cast<VppOutputFile>(m_output);
        if (!outputFile) {
            ERROR("maybe you set a wrong extension");
            return false;
        }
        SharedPtr<FrameWriter> writer(new VaapiFrameWriter(m_vaDisplay));
        if (!outputFile->config(writer)) {
            ERROR("config writer failed");
            return false;
        }
    }
    return DecodeOutputFile::setVideoSize(width, height);
}

bool DecodeOutputDump::writeFrame(SharedPtr<VideoFrame>& frame)
{
    return m_output->output(frame);
}

#if __ENABLE_MD5__
class DecodeOutputMD5 : public DecodeOutputFile {
public:
    DecodeOutputMD5(const char* outputFile, const char* inputFile, uint32_t fourcc)
        : DecodeOutputFile(outputFile, inputFile, fourcc)
    {
    }
    virtual ~DecodeOutputMD5();

protected:
    bool setVideoSize(uint32_t width, uint32_t height);
    virtual bool writeFrame(SharedPtr<VideoFrame>& frame);

private:
    std::string getOutputFileName(uint32_t width, uint32_t height);
    std::string writeToFile(MD5_CTX&);
    static bool calcMD5(char* ptr, int size, FILE* fp);

    FILE* m_file;
    static MD5_CTX m_fileMD5;
    static MD5_CTX m_frameMD5;
    SharedPtr<VaapiFrameIO> m_frameIO;
};

MD5_CTX DecodeOutputMD5::m_frameMD5 = { 0 };
MD5_CTX DecodeOutputMD5::m_fileMD5 = { 0 };

std::string DecodeOutputMD5::getOutputFileName(uint32_t width, uint32_t height)
{
    std::ostringstream name;

    name << m_outputFile;
    struct stat buf;
    int r = stat(m_outputFile, &buf);
    if (r == 0 && buf.st_mode & S_IFDIR) {
        const char* fileName = m_inputFile;
        const char* s = strrchr(m_inputFile, '/');
        if (s)
            fileName = s + 1;
        name << "/" << fileName << ".md5";
    }
    return name.str();
}
bool DecodeOutputMD5::setVideoSize(uint32_t width, uint32_t height)
{
    if (!m_file) {
        std::string name = getOutputFileName(width, height);
        m_file = fopen(name.c_str(), "wb");
        if (!m_file) {
            //ERROR("fail to open input file: %s", name.c_str());
            return false;
        }
        MD5_Init(&m_fileMD5);
        m_frameIO.reset(new VaapiFrameIO(m_vaDisplay, calcMD5));
        return true;
    }
    return DecodeOutputFile::setVideoSize(width, height);
}

std::string DecodeOutputMD5::writeToFile(MD5_CTX& t_ctx)
{
    char temp[4];
    uint8_t result[16] = { 0 };
    std::string strMD5;
    MD5_Final(result, &t_ctx);
    for(uint32_t i = 0; i < 16; i++) {
        memset(temp, 0, sizeof(temp));
        snprintf(temp, sizeof(temp), "%02x", (uint32_t)result[i]);
        strMD5 += temp;
    }
    if (m_file)
        fprintf(m_file, "%s\n", strMD5.c_str());
    return strMD5;
}

bool DecodeOutputMD5::writeFrame(SharedPtr<VideoFrame>& frame)
{
    MD5_Init(&m_frameMD5);
    m_frameIO->doIO(m_file, frame);
    writeToFile(m_frameMD5);
    return true;
}

bool DecodeOutputMD5::calcMD5(char* ptr, int size, FILE* fp)
{
    MD5_Update(&m_frameMD5, ptr, size);
    MD5_Update(&m_fileMD5, ptr, size);
    return true;
}

DecodeOutputMD5::~DecodeOutputMD5()
{
    if (m_file) {
        fprintf(m_file, "The whole frames MD5 ");
        std::string fileMd5 = writeToFile(m_fileMD5);
        fprintf(stderr, "The whole frames MD5:\n%s\n", fileMd5.c_str());
        fclose(m_file);
    }
}
#endif //__ENABLE_MD5__

#ifdef __ENABLE_X11__
class DecodeOutputX11 : public DecodeOutput
{
public:
    DecodeOutputX11();
    virtual ~DecodeOutputX11();
    bool init();
protected:
    virtual bool setVideoSize(uint32_t width, uint32_t height);
    SharedPtr<VADisplay> createX11Display();

    Display* m_display;
    Window   m_window;
};

class DecodeOutputXWindow : public DecodeOutputX11 {
public:
    DecodeOutputXWindow() {}
    virtual ~DecodeOutputXWindow() {}
    bool output(SharedPtr<VideoFrame>& frame);
};

SharedPtr<VADisplay> DecodeOutputX11::createX11Display()
{
    SharedPtr<VADisplay> display;

    m_display = XOpenDisplay(NULL);
    if (!m_display) {
        ERROR("Failed to XOpenDisplay for DecodeOutputX11");
        return display;
    }

    VADisplay m_vaDisplay = vaGetDisplay(m_display);
    int major, minor;
    VAStatus status = vaInitialize(m_vaDisplay, &major, &minor);
    if (!checkVaapiStatus(status, "vaInitialize"))
        return display;
    display.reset(new VADisplay(m_vaDisplay));
    return display;
}

bool DecodeOutputX11::init()
{
    m_vaDisplay = createX11Display();
    if (!m_vaDisplay)
        return false;
    return DecodeOutput::init();
}

bool DecodeOutputX11::setVideoSize(uint32_t width, uint32_t height)
{
    if (m_window) {
        //todo, resize window;
    } else {
        DefaultScreen(m_display);

        XSetWindowAttributes x11WindowAttrib;
        x11WindowAttrib.event_mask = KeyPressMask;
        m_window = XCreateWindow(m_display, DefaultRootWindow(m_display),
            0, 0, width, height, 0, CopyFromParent, InputOutput,
            CopyFromParent, CWEventMask, &x11WindowAttrib);
        XMapWindow(m_display, m_window);
    }
    XSync(m_display, false);
    {
        DEBUG("m_window=%lu", m_window);
        XWindowAttributes wattr;
        XGetWindowAttributes(m_display, m_window, &wattr);
    }
    return DecodeOutput::setVideoSize(width, height);
}

DecodeOutputX11::DecodeOutputX11()
    : m_display(NULL)
    , m_window(0)
{
}

DecodeOutputX11::~DecodeOutputX11()
{
    m_vaDisplay.reset();
    if (m_window)
        XDestroyWindow(m_display, m_window);
    if (m_display)
        XCloseDisplay(m_display);
}

bool DecodeOutputXWindow::output(SharedPtr<VideoFrame>& frame)
{
    if (!setVideoSize(frame->crop.width, frame->crop.height))
        return false;

    VAStatus status = vaPutSurface(*m_vaDisplay, (VASurfaceID)frame->surface,
        m_window, 0, 0, frame->crop.width, frame->crop.height,
        frame->crop.x, frame->crop.y, frame->crop.width, frame->crop.height,
        NULL, 0, 0);
    return checkVaapiStatus(status, "vaPutSurface");
}

#ifdef __ENABLE_TESTS_GLES__
class DecodeOutputEgl : public DecodeOutputX11 {
public:
    DecodeOutputEgl();
    virtual ~DecodeOutputEgl();
protected:
    virtual bool setVideoSize(uint32_t width, uint32_t height) = 0;
    bool setVideoSize(uint32_t width, uint32_t height, bool externalTexture);
    EGLContextType *m_eglContext;
    GLuint m_textureId;
private:
    DISALLOW_COPY_AND_ASSIGN(DecodeOutputEgl);
};

class DecodeOutputPixelMap : public DecodeOutputEgl
{
public:
    virtual bool output(SharedPtr<VideoFrame>& frame);

    DecodeOutputPixelMap();
    virtual ~DecodeOutputPixelMap();
private:
    virtual bool setVideoSize(uint32_t width, uint32_t height);
    XID m_pixmap;
    DISALLOW_COPY_AND_ASSIGN(DecodeOutputPixelMap);
};

class DecodeOutputDmabuf: public DecodeOutputEgl
{
public:
    DecodeOutputDmabuf(VideoDataMemoryType memoryType);
    virtual ~DecodeOutputDmabuf(){};
    virtual bool output(SharedPtr<VideoFrame>& frame);

protected:
    EGLImageKHR createEGLImage(SharedPtr<VideoFrame>&);
    bool draw2D(EGLImageKHR&);
    virtual bool setVideoSize(uint32_t width, uint32_t height);

private:
    VideoDataMemoryType m_memoryType;
    SharedPtr<FrameAllocator> m_allocator;
    SharedPtr<IVideoPostProcess> m_vpp;
};

bool DecodeOutputEgl::setVideoSize(uint32_t width, uint32_t height, bool externalTexture)
{
    if (!DecodeOutputX11::setVideoSize(width, height))
        return false;
    if (!m_eglContext)
        m_eglContext = eglInit(m_display, m_window, VA_FOURCC_RGBA, externalTexture);
    return m_eglContext;
}

DecodeOutputEgl::DecodeOutputEgl()
    : m_eglContext(NULL)
    , m_textureId(0)
{
}

DecodeOutputEgl::~DecodeOutputEgl()
{
    if(m_textureId)
        glDeleteTextures(1, &m_textureId);
    if (m_eglContext)
        eglRelease(m_eglContext);
}

bool DecodeOutputPixelMap::setVideoSize(uint32_t width, uint32_t height)
{
    if (!DecodeOutputEgl::setVideoSize(width, height, false))
        return false;
    if (!m_pixmap) {
        int screen = DefaultScreen(m_display);
        m_pixmap = XCreatePixmap(m_display, DefaultRootWindow(m_display), m_width, m_height, XDefaultDepth(m_display, screen));
        if (!m_pixmap)
            return false;
        XSync(m_display, false);
        m_textureId = createTextureFromPixmap(m_eglContext, m_pixmap);
    }
    return m_textureId;
}

bool DecodeOutputPixelMap::output(SharedPtr<VideoFrame>& frame)
{
    if (!setVideoSize(frame->crop.width, frame->crop.height))
        return false;

    VAStatus status = vaPutSurface(*m_vaDisplay, (VASurfaceID)frame->surface,
        m_pixmap, 0, 0, m_width, m_height,
        frame->crop.x, frame->crop.y, frame->crop.width, frame->crop.height,
        NULL, 0, 0);
    if (!checkVaapiStatus(status, "vaPutSurface")) {
        return false;
    }
    drawTextures(m_eglContext, GL_TEXTURE_2D, &m_textureId, 1);
    return true;
}

DecodeOutputPixelMap::DecodeOutputPixelMap()
    : m_pixmap(0)
{
}

DecodeOutputPixelMap::~DecodeOutputPixelMap()
{
    if(m_pixmap)
        XFreePixmap(m_display, m_pixmap);
}

bool DecodeOutputDmabuf::setVideoSize(uint32_t width, uint32_t height)
{
    if (!DecodeOutputEgl::setVideoSize(width, height, m_memoryType == VIDEO_DATA_MEMORY_TYPE_DMA_BUF))
        return false;
    if (!m_textureId) {
        glGenTextures(1, &m_textureId);
        m_vpp.reset(createVideoPostProcess(YAMI_VPP_SCALER), releaseVideoPostProcess);
        m_vpp->setNativeDisplay(*m_nativeDisplay);
        m_allocator.reset(new PooledFrameAllocator(m_vaDisplay, 3));
        if (!m_allocator->setFormat(VA_FOURCC_BGRX, m_width, m_height)) {
            m_allocator.reset();
            fprintf(stderr, "m_allocator setFormat failed\n");
            return false;
        }
    }
    return m_textureId;
}

EGLImageKHR DecodeOutputDmabuf::createEGLImage(SharedPtr<VideoFrame>& frame)
{
    EGLImageKHR eglImage = EGL_NO_IMAGE_KHR;
    VASurfaceID surface = (VASurfaceID)frame->surface;
    VAImage image;
    VAStatus status = vaDeriveImage(*m_vaDisplay, surface, &image);
    if (!checkVaapiStatus(status, "vaDeriveImage"))
        return eglImage;
    VABufferInfo m_bufferInfo;
    if (m_memoryType == VIDEO_DATA_MEMORY_TYPE_DRM_NAME)
        m_bufferInfo.mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_KERNEL_DRM;
    else if (m_memoryType == VIDEO_DATA_MEMORY_TYPE_DMA_BUF)
        m_bufferInfo.mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
    status = vaAcquireBufferHandle(*m_vaDisplay, image.buf, &m_bufferInfo);
    if (!checkVaapiStatus(status, "vaDeriveImage")) {
        vaDestroyImage(*m_vaDisplay, image.image_id);
        return eglImage;
    }
    eglImage = createEglImageFromHandle(m_eglContext->eglContext.display, m_eglContext->eglContext.context,
        m_memoryType, m_bufferInfo.handle, image.width, image.height, image.pitches[0]);
    checkVaapiStatus(vaReleaseBufferHandle(*m_vaDisplay, image.buf), "vaReleaseBufferHandle");
    checkVaapiStatus(vaDestroyImage(*m_vaDisplay, image.image_id), "vaDestroyImage");
    return eglImage;
}

bool DecodeOutputDmabuf::draw2D(EGLImageKHR& eglImage)
{
    GLenum target = GL_TEXTURE_2D;
    if (m_memoryType == VIDEO_DATA_MEMORY_TYPE_DMA_BUF)
        target = GL_TEXTURE_EXTERNAL_OES;
    glBindTexture(target, m_textureId);
    imageTargetTexture2D(target, eglImage);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    drawTextures(m_eglContext, target, &m_textureId, 1);
    return true;
}

bool DecodeOutputDmabuf::output(SharedPtr<VideoFrame>& frame)
{
    setVideoSize(frame->crop.width, frame->crop.height);

    SharedPtr<VideoFrame> dest = m_allocator->alloc();
    YamiStatus result = m_vpp->process(frame, dest);
    if (result != YAMI_SUCCESS) {
        ERROR("vpp process failed, status = %d", result);
        return false;
    }
    EGLImageKHR eglImage = createEGLImage(dest);
    if (eglImage == EGL_NO_IMAGE_KHR) {
        ERROR("Failed to map %p to egl image", (void*)dest->surface);
        return false;
    }
    draw2D(eglImage);
    return destroyImage(m_eglContext->eglContext.display, eglImage);
}

DecodeOutputDmabuf::DecodeOutputDmabuf(VideoDataMemoryType memoryType)
    : m_memoryType(memoryType)
{
}

#endif //__ENABLE_TESTS_GLES__
#endif //__ENABLE_X11__

DecodeOutput* DecodeOutput::create(int renderMode, uint32_t fourcc, const char* inputFile, const char* outputFile)
{
    DecodeOutput* output;
    switch (renderMode) {
#ifdef __ENABLE_MD5__
    case -2:
        output = new DecodeOutputMD5(outputFile, inputFile, fourcc);
        break;
#endif //__ENABLE_MD5__
    case -1:
        output = new DecodeOutputNull();
        break;
    case 0:
        output = new DecodeOutputDump(outputFile, inputFile, fourcc);
        break;
#ifdef __ENABLE_X11__
    case 1:
        output = new DecodeOutputXWindow();
        break;
#ifdef __ENABLE_TESTS_GLES__
    case 2:
        output = new DecodeOutputPixelMap();
        break;
    case 3:
        output = new DecodeOutputDmabuf(VIDEO_DATA_MEMORY_TYPE_DRM_NAME);
        break;
    case 4:
        output = new DecodeOutputDmabuf(VIDEO_DATA_MEMORY_TYPE_DMA_BUF);
        break;
#endif //__ENABLE_TESTS_GLES__
#endif //__ENABLE_X11__
    default:
        fprintf(stderr, "renderMode:%d, do not support this render mode\n", renderMode);
        return NULL;
    }
    if (!output->init())
        fprintf(stderr, "DecodeOutput init failed\n");
    return output;
}
