#pragma once

// The one place the installed desktop-entry id is written down.
//
// Wayland resolves a window's icon by matching its app_id against a .desktop file; X11 does the
// same through WM_CLASS/StartupWMClass. Every window Remora opens therefore has to name this id,
// and it has to be the SAME id CMakeLists installs packaging/<id>.desktop and <id>.svg under. It
// lived as a literal in App.cpp alone, so the mirror client — which nobody thought of as needing
// an identity — got none and rendered with the compositor's generic glyph (bd remora-28ix.2.8).
//
// Plain char[] rather than QString: this is included by both the Widgets side and the mirror, and
// a namespace-scope QString would be a static initialiser in every translation unit that sees it.

namespace remora {

inline constexpr char kDesktopId[] = "io.github.EtherAura.Remora";
// Where the installer puts the scalable icon, for the case where no icon theme is loaded (a bare
// session, or a mirror launched before the theme is up) and QIcon::fromTheme comes back empty.
inline constexpr char kIconPathTemplate[] = "/usr/share/icons/hicolor/scalable/apps/%1.svg";

}  // namespace remora
