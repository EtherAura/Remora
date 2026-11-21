#include "VaapiInterop.h"

#include <QByteArray>
#include <QLibrary>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QSize>
#include <QString>

#if defined(Q_OS_LINUX) && __has_include(<EGL/eglext.h>) && __has_include(<libdrm/drm_fourcc.h>)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <libdrm/drm_fourcc.h>
#include <limits>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixfmt.h>
}

namespace remora::mirror {

struct VaapiInterop::Platform {
    QLibrary egl{QStringLiteral("EGL"), 1};
    EGLDisplay display = EGL_NO_DISPLAY;
    PFNEGLCREATEIMAGEKHRPROC createImage = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroyImage = nullptr;
    void (*bindImage)(GLenum, void *) = nullptr;
    bool modifierImport = false;
};

VaapiInterop::VaapiInterop() = default;
VaapiInterop::~VaapiInterop() = default;

bool VaapiInterop::initialize(QString *error) {
    auto platform = std::make_unique<Platform>();
    auto *context = QOpenGLContext::currentContext();
    const auto getDisplay = reinterpret_cast<decltype(&eglGetCurrentDisplay)>(
        platform->egl.resolve("eglGetCurrentDisplay"));
    const auto query = reinterpret_cast<decltype(&eglQueryString)>(
        platform->egl.resolve("eglQueryString"));
    const auto getProc = reinterpret_cast<decltype(&eglGetProcAddress)>(
        platform->egl.resolve("eglGetProcAddress"));
    if (!context || !getDisplay || !query || !getProc) {
        *error = QStringLiteral("EGL entry points unavailable");
        return false;
    }
    platform->display = getDisplay();
    if (platform->display == EGL_NO_DISPLAY) {
        *error = QStringLiteral("the window is not using EGL");
        return false;
    }
    const auto extensions = QByteArray(query(platform->display, EGL_EXTENSIONS)).split(' ');
    if (!extensions.contains("EGL_EXT_image_dma_buf_import")
        || !context->hasExtension(QByteArrayLiteral("GL_OES_EGL_image"))) {
        *error = QStringLiteral("EGL DMA-BUF texture import is unavailable");
        return false;
    }
    platform->modifierImport = extensions.contains("EGL_EXT_image_dma_buf_import_modifiers");
    platform->createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(getProc("eglCreateImageKHR"));
    platform->destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(getProc("eglDestroyImageKHR"));
    platform->bindImage = reinterpret_cast<decltype(platform->bindImage)>(
        context->getProcAddress("glEGLImageTargetTexture2DOES"));
    if (!platform->createImage || !platform->destroyImage || !platform->bindImage) {
        *error = QStringLiteral("EGL image entry points unavailable");
        return false;
    }
    platform_ = std::move(platform);
    return true;
}

bool VaapiInterop::bindFrame(AVFrame *frame, const QSize &size, unsigned luma, unsigned chroma,
                             QString *error) {
    const auto release = [](AVFrame *value) { av_frame_free(&value); };
    std::unique_ptr<AVFrame, decltype(release)> mapped(av_frame_alloc(), release);
    if (!mapped) {
        *error = QStringLiteral("could not allocate a DRM frame");
        return false;
    }
    mapped->format = AV_PIX_FMT_DRM_PRIME;
    const int rc = av_hwframe_map(mapped.get(), frame, AV_HWFRAME_MAP_READ);
    if (rc < 0 || !mapped->data[0]) {
        *error = QStringLiteral("VA-API surface export failed (%1)").arg(rc);
        return false;
    }
    const auto &descriptor = *reinterpret_cast<const AVDRMFrameDescriptor *>(mapped->data[0]);
    if (descriptor.nb_layers != 2 || descriptor.layers[0].format != DRM_FORMAT_R8 ||
        descriptor.layers[1].format != DRM_FORMAT_GR88) {
        *error = QStringLiteral("VA-API export does not contain separate NV12 planes");
        return false;
    }

    auto *gl = QOpenGLContext::currentContext()->functions();
    const unsigned textures[] = {luma, chroma};
    const QSize planeSizes[] = {size, QSize(size.width() / 2 + size.width() % 2,
                                           size.height() / 2 + size.height() % 2)};
    for (int i = 0; i < 2; ++i) {
        const auto &layer = descriptor.layers[i];
        const auto &plane = layer.planes[0];
        if (layer.nb_planes != 1 || plane.object_index < 0
            || plane.object_index >= descriptor.nb_objects || plane.object_index >= AV_DRM_MAX_PLANES
            || plane.offset < 0 || plane.offset > std::numeric_limits<EGLint>::max()
            || plane.pitch <= 0 || plane.pitch > std::numeric_limits<EGLint>::max()) {
            *error = QStringLiteral("unsupported DMA-BUF plane layout");
            return false;
        }
        const auto &object = descriptor.objects[plane.object_index];
        const auto modifier = object.format_modifier;
        if (modifier != DRM_FORMAT_MOD_INVALID && modifier != DRM_FORMAT_MOD_LINEAR
            && !platform_->modifierImport) {
            *error = QStringLiteral("EGL lacks support for the exported DMA-BUF modifier");
            return false;
        }

        QList<EGLint> attributes{
            EGL_WIDTH, planeSizes[i].width(),
            EGL_HEIGHT, planeSizes[i].height(),
            EGL_LINUX_DRM_FOURCC_EXT, EGLint(layer.format),
            EGL_DMA_BUF_PLANE0_FD_EXT, object.fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGLint(plane.offset),
            EGL_DMA_BUF_PLANE0_PITCH_EXT, EGLint(plane.pitch),
        };
        if (modifier != DRM_FORMAT_MOD_INVALID && platform_->modifierImport) {
            attributes << EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT << EGLint(modifier & 0xffffffff)
                       << EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT << EGLint(modifier >> 32);
        }
        attributes << EGL_NONE;
        auto image = platform_->createImage(platform_->display, EGL_NO_CONTEXT,
                                             EGL_LINUX_DMA_BUF_EXT, nullptr,
                                             attributes.constData());
        if (image == EGL_NO_IMAGE_KHR) {
            *error = QStringLiteral("EGL rejected DMA-BUF plane %1 (modifier 0x%2)")
                         .arg(i).arg(modifier, 0, 16);
            return false;
        }
        gl->glActiveTexture(GL_TEXTURE0 + i);
        gl->glBindTexture(GL_TEXTURE_2D, textures[i]);
        platform_->bindImage(GL_TEXTURE_2D, image);
        platform_->destroyImage(platform_->display, image);
        const auto glError = gl->glGetError();
        if (glError != GL_NO_ERROR) {
            *error = QStringLiteral("OpenGL rejected DMA-BUF plane %1 (0x%2)")
                         .arg(i).arg(glError, 0, 16);
            return false;
        }
    }
    return true;
}

}  // namespace remora::mirror
#else
namespace remora::mirror {
struct VaapiInterop::Platform {};
VaapiInterop::VaapiInterop() = default;
VaapiInterop::~VaapiInterop() = default;
bool VaapiInterop::initialize(QString *error) {
    *error = QStringLiteral("VA-API texture import was not built");
    return false;
}
bool VaapiInterop::bindFrame(AVFrame *, const QSize &, unsigned, unsigned, QString *) {
    return false;
}
}  // namespace remora::mirror
#endif
