// Copyright 2010-2021, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "renderer/win32/win32_renderer_util.h"

// clang-format off
#define _ATL_NO_AUTOMATIC_NAMESPACE
#define _WTL_NO_AUTOMATIC_NAMESPACE
#include <atlbase.h>
#include <atltypes.h>
#include <atlapp.h>
#include <atlgdi.h>
#include <atlmisc.h>
#include <winuser.h>
// clang-format on

#include <memory>
#include <string>

#include "base/logging.h"
#include "base/system_util.h"
#include "base/win32/win_util.h"
#include "protocol/renderer_command.pb.h"
#include "absl/base/macros.h"

namespace mozc {
namespace renderer {
namespace win32 {
using WTL::CFont;
using WTL::CFontHandle;
using WTL::CLogFont;

namespace {
// This template class is used to represent rendering information which may
// or may not be available depends on the application and operations.
// TODO(yuryu): Replace with std::optional<T>.
template <typename T>
class Optional {
 public:
  Optional() : value_(T()), has_value_(false) {}

  explicit Optional(const T &src) : value_(src), has_value_(true) {}

  const T &value() const {
    DCHECK(has_value_);
    return value_;
  }

  T *mutable_value() {
    has_value_ = true;
    return &value_;
  }

  bool has_value() const { return has_value_; }

  void Clear() { has_value_ = false; }

 private:
  T value_;
  bool has_value_;
};

// A set of rendering information relevant to the target application.  Note
// that all of the positional fields are (logical) screen coordinates.
struct CandidateWindowLayoutParams {
  Optional<HWND> window_handle;
  Optional<IMECHARPOSITION> char_pos;
  Optional<CRect> client_rect;
  Optional<bool> vertical_writing;
};

bool IsValidRect(const commands::RendererCommand::Rectangle &rect) {
  return rect.has_left() && rect.has_top() && rect.has_right() &&
         rect.has_bottom();
}

CRect ToRect(const commands::RendererCommand::Rectangle &rect) {
  DCHECK(IsValidRect(rect));
  return CRect(rect.left(), rect.top(), rect.right(), rect.bottom());
}

bool IsValidPoint(const commands::RendererCommand::Point &point) {
  return point.has_x() && point.has_y();
}

CPoint ToPoint(const commands::RendererCommand::Point &point) {
  DCHECK(IsValidPoint(point));
  return CPoint(point.x(), point.y());
}

// "base_pos" is an ideal position where the candidate window is placed.
// Basically the ideal position depends on historical reason for each
// country and language.
// As for Japanese IME, the bottom-left (for horizontal writing) and
// top-left (for vertical writing) corner of the target segment have been
// used for many years.
CPoint GetBasePositionFromExcludeRect(const CRect &exclude_rect,
                                      bool is_vertical) {
  if (is_vertical) {
    // Vertical
    return exclude_rect.TopLeft();
  }

  // Horizontal
  return CPoint(exclude_rect.left, exclude_rect.bottom);
}

CPoint GetBasePositionFromIMECHARPOSITION(const IMECHARPOSITION &char_pos,
                                          bool is_vertical) {
  if (is_vertical) {
    return CPoint(char_pos.pt.x - char_pos.cLineHeight, char_pos.pt.y);
  }
  // Horizontal
  return CPoint(char_pos.pt.x, char_pos.pt.y + char_pos.cLineHeight);
}

bool ExtractParams(LayoutManager *layout,
                   const commands::RendererCommand::ApplicationInfo &app_info,
                   CandidateWindowLayoutParams *params) {
  DCHECK_NE(nullptr, layout);
  DCHECK_NE(nullptr, params);

  params->window_handle.Clear();
  params->char_pos.Clear();
  params->client_rect.Clear();
  params->vertical_writing.Clear();

  if (!app_info.has_target_window_handle()) {
    return false;
  }
  const HWND target_window =
      WinUtil::DecodeWindowHandle(app_info.target_window_handle());

  *params->window_handle.mutable_value() = target_window;

  if (app_info.has_composition_target()) {
    const commands::RendererCommand::CharacterPosition &char_pos =
        app_info.composition_target();
    // Check the availability of optional fields.
    if (char_pos.has_position() && char_pos.has_top_left() &&
        IsValidPoint(char_pos.top_left()) && char_pos.has_line_height() &&
        char_pos.line_height() > 0 && char_pos.has_document_area() &&
        IsValidRect(char_pos.document_area())) {
      // Positional fields are (logical) screen coordinate.
      IMECHARPOSITION *dest = params->char_pos.mutable_value();
      dest->dwCharPos = char_pos.position();
      dest->pt = ToPoint(char_pos.top_left());
      dest->cLineHeight = char_pos.line_height();
      dest->rcDocument = ToRect(char_pos.document_area());
    }
  }

  {
    CRect client_rect_in_local_coord;
    CRect client_rect_in_logical_screen_coord;
    if (layout->GetClientRect(target_window, &client_rect_in_local_coord) &&
        layout->ClientRectToScreen(target_window, client_rect_in_local_coord,
                                   &client_rect_in_logical_screen_coord)) {
      *params->client_rect.mutable_value() =
          client_rect_in_logical_screen_coord;
    }
  }

  {
    const LayoutManager::WritingDirection direction =
        layout->GetWritingDirection(app_info);
    if (direction == LayoutManager::VERTICAL_WRITING) {
      *params->vertical_writing.mutable_value() = true;
    } else if (direction == LayoutManager::HORIZONTAL_WRITING) {
      *params->vertical_writing.mutable_value() = false;
    }
  }
  return true;
}

class NativeSystemPreferenceAPI : public SystemPreferenceInterface {
 public:
  virtual ~NativeSystemPreferenceAPI() {}

  virtual bool GetDefaultGuiFont(LOGFONTW *log_font) {
    if (log_font == nullptr) {
      return false;
    }

    CLogFont message_box_font;
    // Use message box font as a default font to be consistent with
    // the candidate window.
    // TODO(yukawa): make a theme layer which is responsible for
    //   the look and feel of both composition window and candidate window.
    // TODO(yukawa): verify the font can render U+005C as a yen sign.
    //               (http://b/1992773)
    message_box_font.SetMessageBoxFont();
    // Use factor "3" to be consistent with the candidate window.
    message_box_font.MakeLarger(3);
    message_box_font.lfWeight = FW_NORMAL;

    *log_font = message_box_font;
    return true;
  }
};

class NativeWorkingAreaAPI : public WorkingAreaInterface {
 public:
  NativeWorkingAreaAPI() {}

 private:
  virtual bool GetWorkingAreaFromPoint(const POINT &point, RECT *working_area) {
    if (working_area == nullptr) {
      return false;
    }
    ::SetRect(working_area, 0, 0, 0, 0);

    // Obtain the monitor's working area
    const HMONITOR monitor =
        ::MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
    if (monitor == nullptr) {
      return false;
    }

    MONITORINFO monitor_info = {};
    monitor_info.cbSize = CCSIZEOF_STRUCT(MONITORINFO, dwFlags);
    if (!::GetMonitorInfo(monitor, &monitor_info)) {
      const DWORD error = GetLastError();
      LOG(ERROR) << "GetMonitorInfo failed. Error: " << error;
      return false;
    }

    *working_area = monitor_info.rcWork;
    return true;
  }
};

class NativeWindowPositionAPI : public WindowPositionInterface {
 public:
  NativeWindowPositionAPI()
      : logical_to_physical_point_for_per_monitor_dpi_(
            GetLogicalToPhysicalPointForPerMonitorDPI()) {}

  NativeWindowPositionAPI(const NativeWindowPositionAPI &) = delete;
  NativeWindowPositionAPI &operator=(const NativeWindowPositionAPI &) = delete;

  virtual ~NativeWindowPositionAPI() {}

  virtual bool LogicalToPhysicalPoint(HWND window_handle,
                                      const POINT &logical_coordinate,
                                      POINT *physical_coordinate) {
    if (physical_coordinate == nullptr) {
      return false;
    }
    DCHECK_NE(nullptr, physical_coordinate);
    if (!::IsWindow(window_handle)) {
      return false;
    }

    // The attached window is likely to be a child window but only root
    // windows are fully supported by LogicalToPhysicalPoint API.  Using
    // root window handle instead of target window handle is likely to make
    // this API happy.
    const HWND root_window_handle = GetRootWindow(window_handle);

    // The document of LogicalToPhysicalPoint API is somewhat ambiguous.
    // http://msdn.microsoft.com/en-us/library/ms633533.aspx
    // Both input coordinates and output coordinates of this API are so-called
    // screen coordinates (offset from the upper-left corner of the screen).
    // Note that the input coordinates are logical coordinates, which means you
    // should pass screen coordinates obtained in a DPI-unaware process to
    // this API.  For example, coordinates returned by ClientToScreen API in a
    // DPI-unaware process are logical coordinates.  You can copy these
    // coordinates to a DPI-aware process and convert them to physical screen
    // coordinates by LogicalToPhysicalPoint API.
    *physical_coordinate = logical_coordinate;

    // Despite its name, LogicalToPhysicalPoint API no longer converts
    // coordinates on Windows 8.1 and later. We must use
    // LogicalToPhysicalPointForPerMonitorDPI API instead when it is available.
    // See http://go.microsoft.com/fwlink/?LinkID=307061
    if (SystemUtil::IsWindows8_1OrLater()) {
      if (logical_to_physical_point_for_per_monitor_dpi_ == nullptr) {
        return false;
      }
      return logical_to_physical_point_for_per_monitor_dpi_(
                 root_window_handle, physical_coordinate) != FALSE;
    }
    // On Windows 8 and prior, it's OK to rely on LogicalToPhysicalPoint API.
    return ::LogicalToPhysicalPoint(root_window_handle, physical_coordinate) !=
           FALSE;
  }

  // This method is not const to implement Win32WindowInterface.
  virtual bool GetWindowRect(HWND window_handle, RECT *rect) {
    return (::GetWindowRect(window_handle, rect) != FALSE);
  }

  // This method is not const to implement Win32WindowInterface.
  virtual bool GetClientRect(HWND window_handle, RECT *rect) {
    return (::GetClientRect(window_handle, rect) != FALSE);
  }

  // This method is not const to implement Win32WindowInterface.
  virtual bool ClientToScreen(HWND window_handle, POINT *point) {
    return (::ClientToScreen(window_handle, point) != FALSE);
  }

  // This method is not const to implement Win32WindowInterface.
  virtual bool IsWindow(HWND window_handle) {
    return (::IsWindow(window_handle) != FALSE);
  }

  // This method is not const to implement Win32WindowInterface.
  virtual HWND GetRootWindow(HWND window_handle) {
    // See the following document for Win32 window system.
    // http://msdn.microsoft.com/en-us/library/ms997562.aspx
    return ::GetAncestor(window_handle, GA_ROOT);
  }

  // This method is not const to implement Win32WindowInterface.
  virtual bool GetWindowClassName(HWND window_handle,
                                  std::wstring *class_name) {
    if (class_name == nullptr) {
      return false;
    }
    wchar_t class_name_buffer[1024] = {};
    const size_t num_copied_without_null = ::GetClassNameW(
        window_handle, class_name_buffer, std::size(class_name_buffer));
    if (num_copied_without_null >= (std::size(class_name_buffer) - 1)) {
      DLOG(ERROR) << "buffer length is insufficient.";
      return false;
    }
    class_name_buffer[num_copied_without_null] = L'\0';
    class_name->assign(class_name_buffer);
    return (::IsWindow(window_handle) != FALSE);
  }

 private:
  typedef BOOL(WINAPI *LogicalToPhysicalPointForPerMonitorDPIFunc)(
      HWND window_handle, POINT *point);
  static LogicalToPhysicalPointForPerMonitorDPIFunc
  GetLogicalToPhysicalPointForPerMonitorDPI() {
    // LogicalToPhysicalPointForPerMonitorDPI API is available on Windows 8.1
    // and later.
    if (!SystemUtil::IsWindows8_1OrLater()) {
      return nullptr;
    }

    const HMODULE module = WinUtil::GetSystemModuleHandle(L"user32.dll");
    if (module == nullptr) {
      return nullptr;
    }
    return reinterpret_cast<LogicalToPhysicalPointForPerMonitorDPIFunc>(
        ::GetProcAddress(module, "LogicalToPhysicalPointForPerMonitorDPI"));
  }

  const LogicalToPhysicalPointForPerMonitorDPIFunc
      logical_to_physical_point_for_per_monitor_dpi_;
};

struct WindowInfo {
  std::wstring class_name;
  CRect window_rect;
  CPoint client_area_offset;
  CSize client_area_size;
  double scale_factor;
  WindowInfo() : scale_factor(1.0) {}
};

class SystemPreferenceEmulatorImpl : public SystemPreferenceInterface {
 public:
  explicit SystemPreferenceEmulatorImpl(const LOGFONTW &gui_font)
      : default_gui_font_(gui_font) {}

  virtual ~SystemPreferenceEmulatorImpl() {}

  virtual bool GetDefaultGuiFont(LOGFONTW *log_font) {
    if (log_font == nullptr) {
      return false;
    }
    *log_font = default_gui_font_;
    return true;
  }

 private:
  CLogFont default_gui_font_;
};

class WorkingAreaEmulatorImpl : public WorkingAreaInterface {
 public:
  explicit WorkingAreaEmulatorImpl(const CRect &area) : area_(area) {}

 private:
  virtual bool GetWorkingAreaFromPoint(const POINT &point, RECT *working_area) {
    if (working_area == nullptr) {
      return false;
    }
    *working_area = area_;
    return true;
  }
  const CRect area_;
};

class WindowPositionEmulatorImpl : public WindowPositionEmulator {
 public:
  WindowPositionEmulatorImpl() {}
  WindowPositionEmulatorImpl(const WindowPositionEmulatorImpl &) = delete;
  WindowPositionEmulatorImpl &operator=(const WindowPositionEmulatorImpl &) =
      delete;
  virtual ~WindowPositionEmulatorImpl() {}

  // This method is not const to implement Win32WindowInterface.
  virtual bool GetWindowRect(HWND window_handle, RECT *rect) {
    if (rect == nullptr) {
      return false;
    }
    const WindowInfo *info = GetWindowInformation(window_handle);
    if (info == nullptr) {
      return false;
    }
    *rect = info->window_rect;
    return true;
  }

  // This method is not const to implement Win32WindowInterface.
  virtual bool GetClientRect(HWND window_handle, RECT *rect) {
    if (rect == nullptr) {
      return false;
    }
    const WindowInfo *info = GetWindowInformation(window_handle);
    if (info == nullptr) {
      return false;
    }
    *rect = CRect(CPoint(0, 0), info->client_area_size);
    return true;
  }

  // This method is not const to implement Win32WindowInterface.
  virtual bool ClientToScreen(HWND window_handle, POINT *point) {
    if (point == nullptr) {
      return false;
    }
    const WindowInfo *info = GetWindowInformation(window_handle);
    if (info == nullptr) {
      return false;
    }
    *point = (info->window_rect.TopLeft() + info->client_area_offset + *point);
    return true;
  }

  // This method is not const to implement Win32WindowInterface.
  virtual bool IsWindow(HWND window_handle) {
    const WindowInfo *info = GetWindowInformation(window_handle);
    if (info == nullptr) {
      return false;
    }
    return true;
  }

  // This method wraps API call of GetAncestor/GA_ROOT.
  virtual HWND GetRootWindow(HWND window_handle) {
    const std::map<HWND, HWND>::const_iterator it =
        root_map_.find(window_handle);
    if (it == root_map_.end()) {
      return window_handle;
    }
    return it->second;
  }

  // This method is not const to implement Win32WindowInterface.
  virtual bool GetWindowClassName(HWND window_handle,
                                  std::wstring *class_name) {
    if (class_name == nullptr) {
      return false;
    }
    const WindowInfo *info = GetWindowInformation(window_handle);
    if (info == nullptr) {
      return false;
    }
    *class_name = info->class_name;
    return true;
  }

  // This method is not const to implement Win32WindowInterface.
  virtual bool LogicalToPhysicalPoint(HWND window_handle,
                                      const POINT &logical_coordinate,
                                      POINT *physical_coordinate) {
    if (physical_coordinate == nullptr) {
      return false;
    }

    DCHECK_NE(nullptr, physical_coordinate);
    const WindowInfo *root_info =
        GetWindowInformation(GetRootWindow(window_handle));
    if (root_info == nullptr) {
      return false;
    }

    // BottomRight is treated inside of the rect in this scenario.
    const CRect &bottom_right_inflated_rect = CRect(
        root_info->window_rect.left, root_info->window_rect.top,
        root_info->window_rect.right + 1, root_info->window_rect.bottom + 1);
    if (bottom_right_inflated_rect.PtInRect(logical_coordinate) == FALSE) {
      return false;
    }
    physical_coordinate->x = logical_coordinate.x * root_info->scale_factor;
    physical_coordinate->y = logical_coordinate.y * root_info->scale_factor;
    return true;
  }

  virtual HWND RegisterWindow(const std::wstring &class_name,
                              const RECT &window_rect,
                              const POINT &client_area_offset,
                              const SIZE &client_area_size,
                              double scale_factor) {
    const HWND hwnd = GetNextWindowHandle();
    window_map_[hwnd].class_name = class_name;
    window_map_[hwnd].window_rect = window_rect;
    window_map_[hwnd].client_area_offset = client_area_offset;
    window_map_[hwnd].client_area_size = client_area_size;
    window_map_[hwnd].scale_factor = scale_factor;
    return hwnd;
  }

  virtual void SetRoot(HWND child_window, HWND root_window) {
    root_map_[child_window] = root_window;
  }

 private:
  HWND GetNextWindowHandle() const {
    if (window_map_.size() > 0) {
      const HWND last_hwnd = window_map_.rbegin()->first;
      return WinUtil::DecodeWindowHandle(
          WinUtil::EncodeWindowHandle(last_hwnd) + 7);
    }
    return WinUtil::DecodeWindowHandle(0x12345678);
  }

  // This method is not const to implement Win32WindowInterface.
  const WindowInfo *GetWindowInformation(HWND hwnd) {
    if (window_map_.find(hwnd) == window_map_.end()) {
      return nullptr;
    }
    return &(window_map_.find(hwnd)->second);
  }

  std::map<HWND, WindowInfo> window_map_;
  std::map<HWND, HWND> root_map_;
};

bool IsVerticalWriting(const CandidateWindowLayoutParams &params) {
  return params.vertical_writing.has_value() && params.vertical_writing.value();
}

// This function tries to use IMECHARPOSITION structure, which gives us
// sufficient information around the focused segment to use EXCLUDE-style
// positioning.  However, relatively small number of applications support
// this structure.
//   Expected applications and controls are:
//     - Microsoft Word
//     - Built-in RichEdit control
//     -- Chrome's Omni-box
//     - Built-in Edit control
//     -- Internet Explorer's address bar
//     - Firefox
//   See also relevant unit tests.
// Returns true if the |candidate_layout| is determined in successful.
bool LayoutCandidateWindowByCompositionTarget(
    const CandidateWindowLayoutParams &params, LayoutManager *layout_manager,
    CandidateWindowLayout *candidate_layout) {
  DCHECK(candidate_layout);
  candidate_layout->Clear();

  if (!params.window_handle.has_value()) {
    return false;
  }
  if (!params.char_pos.has_value()) {
    return false;
  }

  const HWND target_window = params.window_handle.value();
  const IMECHARPOSITION &char_pos = params.char_pos.value();

  // From the behavior of MS Office, we assume that an application fills
  // members in IMECHARPOSITION as follows, even though other interpretations
  // might be possible from the document especially for the vertical writing.
  //   http://msdn.microsoft.com/en-us/library/dd318162.aspx
  //
  // [Horizontal Writing]
  //
  //    (pt)
  //     v_____
  //     |     |
  //     |     | (cLineHeight)
  //     |     |
  //   --+-----+---------->  (Base Line)
  //
  // [Vertical Writing]
  //
  //    |
  //    +-----< (pt)
  //    |     |
  //    |-----+
  //    | (cLineHeight)
  //    |
  //    |
  //    v
  //   (Base Line)

  const bool is_vertical = IsVerticalWriting(params);
  CRect exclude_region_in_logical_coord;
  if (is_vertical) {
    // Vertical
    exclude_region_in_logical_coord.left = char_pos.pt.x - char_pos.cLineHeight;
    exclude_region_in_logical_coord.top = char_pos.pt.y;
    exclude_region_in_logical_coord.right = char_pos.pt.x;
    exclude_region_in_logical_coord.bottom = char_pos.pt.y + 1;
  } else {
    // Horizontal
    exclude_region_in_logical_coord.left = char_pos.pt.x;
    exclude_region_in_logical_coord.top = char_pos.pt.y;
    exclude_region_in_logical_coord.right = char_pos.pt.x + 1;
    exclude_region_in_logical_coord.bottom =
        char_pos.pt.y + char_pos.cLineHeight;
  }

  const CPoint base_pos_in_logical_coord =
      GetBasePositionFromIMECHARPOSITION(char_pos, is_vertical);

  CPoint base_pos_in_physical_coord;
  layout_manager->GetPointInPhysicalCoords(
      target_window, base_pos_in_logical_coord, &base_pos_in_physical_coord);

  CRect exclude_region_in_physical_coord;
  layout_manager->GetRectInPhysicalCoords(target_window,
                                          exclude_region_in_logical_coord,
                                          &exclude_region_in_physical_coord);

  candidate_layout->InitializeWithPositionAndExcludeRegion(
      base_pos_in_physical_coord, exclude_region_in_physical_coord);
  return true;
}

bool LayoutIndicatorWindowByCompositionTarget(
    const CandidateWindowLayoutParams &params,
    const LayoutManager &layout_manager, CRect *target_rect) {
  DCHECK(target_rect);
  *target_rect = CRect();

  if (!params.window_handle.has_value()) {
    return false;
  }
  if (!params.char_pos.has_value()) {
    return false;
  }

  const HWND target_window = params.window_handle.value();
  const IMECHARPOSITION &char_pos = params.char_pos.value();

  // From the behavior of MS Office, we assume that an application fills
  // members in IMECHARPOSITION as follows, even though other interpretations
  // might be possible from the document especially for the vertical writing.
  //   http://msdn.microsoft.com/en-us/library/dd318162.aspx

  const bool is_vertical = IsVerticalWriting(params);
  CRect rect_in_logical_coord;
  if (is_vertical) {
    // [Vertical Writing]
    //
    //    |
    //    +-----< (pt)
    //    |     |
    //    |-----+
    //    | (cLineHeight)
    //    |
    //    |
    //    v
    //   (Base Line)
    rect_in_logical_coord =
        CRect(char_pos.pt.x - char_pos.cLineHeight, char_pos.pt.y,
              char_pos.pt.x, char_pos.pt.y + 1);
  } else {
    // [Horizontal Writing]
    //
    //    (pt)
    //     v_____
    //     |     |
    //     |     | (cLineHeight)
    //     |     |
    //   --+-----+---------->  (Base Line)
    rect_in_logical_coord =
        CRect(char_pos.pt.x, char_pos.pt.y, char_pos.pt.x + 1,
              char_pos.pt.y + char_pos.cLineHeight);
  }

  layout_manager.GetRectInPhysicalCoords(target_window, rect_in_logical_coord,
                                         target_rect);
  return true;
}

bool GetTargetRectForIndicator(const CandidateWindowLayoutParams &params,
                               const LayoutManager &layout_manager,
                               CRect *focus_rect) {
  if (focus_rect == nullptr) {
    return false;
  }

  if (LayoutIndicatorWindowByCompositionTarget(params, layout_manager,
                                               focus_rect)) {
    return true;
  }

  // Clear the data just in case.
  *focus_rect = CRect();
  return false;
}

}  // namespace

SystemPreferenceInterface *SystemPreferenceFactory::CreateMock(
    const LOGFONTW &gui_font) {
  return new SystemPreferenceEmulatorImpl(gui_font);
}

WorkingAreaInterface *WorkingAreaFactory::Create() {
  return new NativeWorkingAreaAPI();
}

WindowPositionEmulator *WindowPositionEmulator::Create() {
  return new WindowPositionEmulatorImpl();
}

CandidateWindowLayout::CandidateWindowLayout()
    : position_(CPoint()),
      exclude_region_(CRect()),
      has_exclude_region_(false),
      initialized_(false) {}

CandidateWindowLayout::~CandidateWindowLayout() {}

void CandidateWindowLayout::Clear() {
  position_ = CPoint();
  exclude_region_ = CRect();
  has_exclude_region_ = false;
  initialized_ = false;
}

void CandidateWindowLayout::InitializeWithPosition(const POINT &position) {
  position_ = position;
  exclude_region_ = CRect();
  has_exclude_region_ = false;
  initialized_ = true;
}

void CandidateWindowLayout::InitializeWithPositionAndExcludeRegion(
    const POINT &position, const RECT &exclude_region) {
  position_ = position;
  exclude_region_ = exclude_region;
  has_exclude_region_ = true;
  initialized_ = true;
}

const POINT &CandidateWindowLayout::position() const { return position_; }

const RECT &CandidateWindowLayout::exclude_region() const {
  DCHECK(has_exclude_region_);
  return exclude_region_;
}

bool CandidateWindowLayout::has_exclude_region() const {
  return has_exclude_region_;
}

bool CandidateWindowLayout::initialized() const { return initialized_; }

IndicatorWindowLayout::IndicatorWindowLayout() : is_vertical(false) {
  ::SetRect(&window_rect, 0, 0, 0, 0);
}

void IndicatorWindowLayout::Clear() {
  is_vertical = false;
  ::SetRect(&window_rect, 0, 0, 0, 0);
}

void LayoutManager::GetPointInPhysicalCoords(HWND window_handle,
                                             const POINT &point,
                                             POINT *result) const {
  if (result == nullptr) {
    return;
  }

  DCHECK_NE(nullptr, window_position_.get());
  if (window_position_->LogicalToPhysicalPoint(window_handle, point, result)) {
    return;
  }

  // LogicalToPhysicalPoint API failed for some reason.
  // Emulate the result based on the scaling factor.
  const HWND root_window_handle =
      window_position_->GetRootWindow(window_handle);
  const double scale_factor = GetScalingFactor(root_window_handle);
  result->x = point.x * scale_factor;
  result->y = point.y * scale_factor;
  return;
}

void LayoutManager::GetRectInPhysicalCoords(HWND window_handle,
                                            const RECT &rect,
                                            RECT *result) const {
  if (result == nullptr) {
    return;
  }

  DCHECK_NE(nullptr, window_position_.get());

  CPoint top_left;
  GetPointInPhysicalCoords(window_handle, CPoint(rect.left, rect.top),
                           &top_left);
  CPoint bottom_right;
  GetPointInPhysicalCoords(window_handle, CPoint(rect.right, rect.bottom),
                           &bottom_right);
  *result = CRect(top_left, bottom_right);
  return;
}

LayoutManager::LayoutManager()
    : system_preference_(new NativeSystemPreferenceAPI),
      window_position_(new NativeWindowPositionAPI) {}

LayoutManager::LayoutManager(SystemPreferenceInterface *mock_system_preference,
                             WindowPositionInterface *mock_window_position)
    : system_preference_(mock_system_preference),
      window_position_(mock_window_position) {}

LayoutManager::~LayoutManager() {}

bool LayoutManager::ClientPointToScreen(HWND src_window_handle,
                                        const POINT &src_point,
                                        POINT *dest_point) const {
  if (dest_point == nullptr) {
    return false;
  }

  if (!window_position_->IsWindow(src_window_handle)) {
    DLOG(ERROR) << "Invalid window handle.";
    return false;
  }

  CPoint converted = src_point;
  if (window_position_->ClientToScreen(src_window_handle, &converted) ==
      FALSE) {
    DLOG(ERROR) << "ClientToScreen failed.";
    return false;
  }

  *dest_point = converted;
  return true;
}

bool LayoutManager::ClientRectToScreen(HWND src_window_handle,
                                       const RECT &src_rect,
                                       RECT *dest_rect) const {
  if (dest_rect == nullptr) {
    return false;
  }

  if (!window_position_->IsWindow(src_window_handle)) {
    DLOG(ERROR) << "Invalid window handle.";
    return false;
  }

  CPoint top_left(src_rect.left, src_rect.top);
  if (window_position_->ClientToScreen(src_window_handle, &top_left) == FALSE) {
    DLOG(ERROR) << "ClientToScreen failed.";
    return false;
  }

  CPoint bottom_right(src_rect.right, src_rect.bottom);
  if (window_position_->ClientToScreen(src_window_handle, &bottom_right) ==
      FALSE) {
    DLOG(ERROR) << "ClientToScreen failed.";
    return false;
  }

  dest_rect->left = top_left.x;
  dest_rect->top = top_left.y;
  dest_rect->right = bottom_right.x;
  dest_rect->bottom = bottom_right.y;
  return true;
}

bool LayoutManager::GetClientRect(HWND window_handle, RECT *client_rect) const {
  return window_position_->GetClientRect(window_handle, client_rect);
}

double LayoutManager::GetScalingFactor(HWND window_handle) const {
  constexpr double kDefaultValue = 1.0;
  CRect window_rect_in_logical_coord;
  if (!window_position_->GetWindowRect(window_handle,
                                       &window_rect_in_logical_coord)) {
    return kDefaultValue;
  }

  CPoint top_left_in_physical_coord;
  if (!window_position_->LogicalToPhysicalPoint(
          window_handle, window_rect_in_logical_coord.TopLeft(),
          &top_left_in_physical_coord)) {
    return kDefaultValue;
  }
  CPoint bottom_right_in_physical_coord;
  if (!window_position_->LogicalToPhysicalPoint(
          window_handle, window_rect_in_logical_coord.BottomRight(),
          &bottom_right_in_physical_coord)) {
    return kDefaultValue;
  }
  const CRect window_rect_in_physical_coord(top_left_in_physical_coord,
                                            bottom_right_in_physical_coord);

  if (window_rect_in_physical_coord == window_rect_in_logical_coord) {
    // No scaling.
    return 1.0;
  }

  // use larger edge to calculate the scaling factor more accurately.
  if (window_rect_in_logical_coord.Width() >
      window_rect_in_logical_coord.Height()) {
    // Use width.
    if (window_rect_in_physical_coord.Width() <= 0 ||
        window_rect_in_logical_coord.Width() <= 0) {
      return kDefaultValue;
    }
    DCHECK_NE(0, window_rect_in_logical_coord.Width()) << "divided-by-zero";
    return (static_cast<double>(window_rect_in_physical_coord.Width()) /
            window_rect_in_logical_coord.Width());
  } else {
    // Use Height.
    if (window_rect_in_physical_coord.Height() <= 0 ||
        window_rect_in_logical_coord.Height() <= 0) {
      return kDefaultValue;
    }
    DCHECK_NE(0, window_rect_in_logical_coord.Height()) << "divided-by-zero";
    return (static_cast<double>(window_rect_in_physical_coord.Height()) /
            window_rect_in_logical_coord.Height());
  }
}

bool LayoutManager::GetDefaultGuiFont(LOGFONTW *logfont) const {
  return system_preference_->GetDefaultGuiFont(logfont);
}

LayoutManager::WritingDirection LayoutManager::GetWritingDirection(
    const commands::RendererCommand_ApplicationInfo &app_info) {
  // TODO(https://github.com/google/mozc/issues/362): Implement this for TSF.
  // When fixing this, we also need to update Chromium.
  //   https://chromium-review.googlesource.com/c/chromium/src/+/4023235
  return WRITING_DIRECTION_UNSPECIFIED;
}

bool LayoutManager::LayoutCandidateWindow(
    const commands::RendererCommand::ApplicationInfo &app_info,
    CandidateWindowLayout *candidate_layout) {

  CandidateWindowLayoutParams params;
  if (!ExtractParams(this, app_info, &params)) {
    return false;
  }

  if (LayoutCandidateWindowByCompositionTarget(
          params, this, candidate_layout)) {
    DCHECK(candidate_layout->initialized());
    return true;
  }

  return false;
}


bool LayoutManager::LayoutIndicatorWindow(
    const commands::RendererCommand_ApplicationInfo &app_info,
    IndicatorWindowLayout *indicator_layout) {
  if (indicator_layout == nullptr) {
    return false;
  }
  indicator_layout->Clear();

  CandidateWindowLayoutParams params;
  if (!ExtractParams(this, app_info, &params)) {
    return false;
  }

  CRect target_rect;
  if (!GetTargetRectForIndicator(params, *this, &target_rect)) {
    return false;
  }

  indicator_layout->is_vertical = IsVerticalWriting(params);
  indicator_layout->window_rect = target_rect;
  return true;
}

}  // namespace win32
}  // namespace renderer
}  // namespace mozc
