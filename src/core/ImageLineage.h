#pragma once
#include <QString>
#include <QStringList>
#include <optional>

namespace remora {

struct ResolvedConfig;

// A local image the registry does not know, and the registered image it was built over
// (bd remora-3czv). Every question about what an image CAN DO is asked of a registered tag, and a
// profile pinned to a layer built by hand over one (image_tag=remora-local:…) used to get
// "nothing" for an answer: the GUI greyed h265 out of an image that ships the encoder and reset
// the page to h264. The layers Remora builds label the registered image at the bottom of their
// stack (org.remora.addons.root, org.remora.play.base — ImageBuild.cpp, PlayInstall), and a layer
// built over one of those inherits the labels; any other layer still starts with its base's layers.
//
// Pure data, like ManagedHost: the engine asks the docker host (probeImageRoot, learnImageRoot)
// and records the answer here; the Resolver reads it. Keyed by docker host as well as tag, because
// a remora-local: name means something only to the daemon that built it.
QString imageHostKey(const ResolvedConfig &r);
void recordImageRoot(const QString &dockerHost, const QString &tag, const QString &rootTag);
// nullopt = never asked; empty = asked, and the image names no registered root.
std::optional<QString> recordedImageRoot(const QString &dockerHost, const QString &tag);
void forgetImageRoots();

// Which of an image's tags the registry knows: a registered name exactly, else the longest
// registered name one of them extends by a '-' suffix — the dated tag an older public release
// keeps once the plain name moves on, or a CPU-tuned variant of a registered build. Empty when none.
QString registeredTagAmong(const QStringList &tags);

// What probeImageRoot's script prints, read for the image `pin` (Probe.cpp). Two sections, each
// headed by its name on a line of its own:
//   ROOT    the tags of the image the labels name, one per line (none when there is no label)
//   LAYERS  "<layers> <tags>" per image, both comma-joined: the pin, then every registered tag
//           present on the host
// The labels answer first. Without them — a layer `remora build` made, a `docker commit` — the
// layers do: a layer built over a registered image starts with exactly that image's layers, and
// the registered image with the most of them is the one it was built on. Empty when neither says.
QString parseImageRootProbe(const QString &out, const QString &pin);

}  // namespace remora
