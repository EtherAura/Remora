#pragma once

#include <memory>

struct AVFrame;
class QSize;
class QString;

namespace remora::mirror {

class VaapiInterop {
public:
    VaapiInterop();
    ~VaapiInterop();

    bool initialize(QString *error);
    bool bindFrame(AVFrame *frame, const QSize &size, unsigned luma, unsigned chroma,
                   QString *error);

private:
    struct Platform;
    std::unique_ptr<Platform> platform_;
};

}  // namespace remora::mirror
