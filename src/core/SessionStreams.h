#pragma once
#include <QByteArray>
#include <QHash>
#include <QJsonObject>

// Which process plays a stream on the desk, for the host agent's session audio (docs/PHONE_APP.md,
// Sound). Pure: pactl's JSON in.
//
// A PulseAudio client names its process on each stream it opens (application.process.id). A
// native PipeWire client — VLC 4's audio output, pw-play — names it only on its client, so its
// stream is matched through the client it names: pipewire.sec.pid, which the server takes from
// the socket's credentials, else the client's own application.process.id.

namespace remora::sessionaudio {

// `pactl -f json list clients` → client index → process id. A client that names no process is
// left out.
QHash<qint64, int> clientProcesses(const QByteArray &clientsJson);

// One entry of `pactl -f json list sink-inputs`: the process its properties name, else its
// client's in `clients`; 0 when neither says.
int streamProcess(const QJsonObject &sinkInput, const QHash<qint64, int> &clients);

}  // namespace remora::sessionaudio
