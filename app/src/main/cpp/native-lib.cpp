#include "Crypto/unpkg.h"
#include "Crypto/unself.h"
#include "Emu/Audio/Cubeb/CubebBackend.h"
#include "Emu/Audio/Null/NullAudioBackend.h"
#include "Emu/Cell/Modules/cellMsgDialog.h"
#include "Emu/Cell/PPUAnalyser.h"
#include "Emu/Cell/SPURecompiler.h"
#include "Emu/Cell/lv2/sys_sync.h"
#include "Emu/IdManager.h"
#include "Emu/Io/KeyboardHandler.h"
#include "Emu/Io/Null/NullKeyboardHandler.h"
#include "Emu/Io/Null/NullMouseHandler.h"
#include "Emu/Io/Null/null_camera_handler.h"
#include "Emu/Io/Null/null_music_handler.h"
#include "Emu/Io/pad_config_types.h"
#include "Emu/RSX/Null/NullGSRender.h"
#include "Emu/RSX/RSXThread.h"
#include "Emu/RSX/VK/VKGSRender.h"
#include "Emu/localized_string_id.h"
#include "Emu/system_config.h"
#include "Emu/system_config_types.h"
#include "Emu/system_progress.hpp"
#include "Emu/system_utils.hpp"
#include "Emu/vfs_config.h"
#include "Input/ds3_pad_handler.h"
#include "Input/ds4_pad_handler.h"
#include "Input/dualsense_pad_handler.h"
#include "Input/hid_pad_handler.h"
#include "Input/pad_thread.h"
#include "Input/virtual_pad_handler.h"
#include "Loader/PSF.h"
#include "Loader/PUP.h"
#include "Loader/TAR.h"
#include "Utilities/File.h"
#include "Utilities/JIT.h"
#include "Utilities/StrFmt.h"
#include "Utilities/StrUtil.h"
#include "Utilities/Thread.h"
#include "block_dev.hpp"
#include "hidapi_libusb.h"
#include "iso.hpp"
#include "libusb.h"
#include "rpcs3_version.h"
#include "util/asm.hpp"
#include "util/console.h"
#include "util/fixed_typemap.hpp"
#include "util/logs.hpp"
#include "util/serialization.hpp"
#include "util/sysinfo.hpp"
#include <Emu/Cell/Modules/cellSaveData.h>
#include <Emu/Cell/Modules/sceNpTrophy.h>
#include <Emu/Io/pad_config.h>
#include <Emu/RSX/GSFrameBase.h>
#include <Emu/System.h>

#include <algorithm>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <iterator>
#include <jni.h>
#include <optional>
#include <span>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <vector>

struct AtExit {
  std::function<void()> cb;
  ~AtExit() { cb(); }
};

static bool g_initialized;
static std::atomic<ANativeWindow *> g_native_window;

extern std::string g_android_executable_dir;
extern std::string g_android_config_dir;
extern std::string g_android_cache_dir;

static std::mutex g_virtual_pad_mutex;
static std::shared_ptr<Pad> g_virtual_pad;

std::string g_input_config_override;
cfg_input_configurations g_cfg_input_configs;

LOG_CHANNEL(rpcs3_android, "ANDROID");

struct LogListener : logs::listener {
  LogListener() { logs::listener::add(this); }

  void log(u64 stamp, const logs::message &msg, const std::string &prefix,
           const std::string &text) override {
    int prio = 0;
    switch (static_cast<logs::level>(msg)) {
    case logs::level::always:
    case logs::level::fatal:
      prio = ANDROID_LOG_FATAL;
      break;
    case logs::level::error:
      prio = ANDROID_LOG_ERROR;
      break;
    case logs::level::todo:
      prio = ANDROID_LOG_WARN;
      break;
    case logs::level::success:
      prio = ANDROID_LOG_INFO;
      break;
    case logs::level::warning:
      prio = ANDROID_LOG_WARN;
      break;
    case logs::level::notice:
      prio = ANDROID_LOG_DEBUG;
      break;
    case logs::level::trace:
      prio = ANDROID_LOG_VERBOSE;
      break;
    }

    __android_log_write(prio, "RPCS3", text.c_str());
  }
} static g_androidLogListener;

struct GraphicsFrame : GSFrameBase {
  mutable ANativeWindow *activeNativeWindow = nullptr;
  mutable int width = 0;
  mutable int height = 0;

  ~GraphicsFrame() {
    if (activeNativeWindow != nullptr) {
      ANativeWindow_release(activeNativeWindow);
    }
  }

  ANativeWindow *getNativeWindow() const {
    ANativeWindow *result;
    while ((result = g_native_window.load()) == nullptr) [[unlikely]] {
      if (Emu.IsStopped()) {
        return nullptr;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (result != activeNativeWindow) [[unlikely]] {
      ANativeWindow_acquire(result);

      if (activeNativeWindow != nullptr) {
        ANativeWindow_release(activeNativeWindow);
      }

      activeNativeWindow = result;

      width = ANativeWindow_getWidth(result);
      height = ANativeWindow_getHeight(result);
    }

    return result;
  }

  void close() override {}
  void reset() override {}
  bool shown() override { return true; }
  void hide() override {}
  void show() override {}
  void toggle_fullscreen() override {}

  void delete_context(draw_context_t ctx) override {}
  draw_context_t make_context() override { return nullptr; }
  void set_current(draw_context_t ctx) override {}
  void flip(draw_context_t ctx, bool skip_frame = false) override {}
  int client_width() override { return width; }
  int client_height() override { return height; }
  f64 client_display_rate() override { return 30.f; }
  bool has_alpha() override {
    return ANativeWindow_getFormat(getNativeWindow()) ==
           WINDOW_FORMAT_RGBA_8888;
  }

  display_handle_t handle() const override { return getNativeWindow(); }

  bool can_consume_frame() const override { return false; }

  void present_frame(std::vector<u8> &data, u32 pitch, u32 width, u32 height,
                     bool is_bgra) const override {}

  void take_screenshot(std::vector<u8> &&sshot_data, u32 sshot_width,
                       u32 sshot_height, bool is_bgra) override {}
};

void jit_announce(uptr, usz, std::string_view);

[[noreturn]] void report_fatal_error(std::string_view _text,
                                     bool is_html = false,
                                     bool include_help_text = true) {
  std::string buf;

  buf = std::string(_text);

  // Check if thread id is in string
  if (_text.find("\nThread id = "sv) == umax && !thread_ctrl::is_main()) {
    // Append thread id if it isn't already, except on main thread
    fmt::append(buf, "\n\nThread id = %u.", thread_ctrl::get_tid());
  }

  if (!g_tls_serialize_name.empty()) {
    fmt::append(buf, "\nSerialized Object: %s", g_tls_serialize_name);
  }

  const system_state state = Emu.GetStatus(false);

  if (state == system_state::stopped) {
    fmt::append(buf, "\nEmulation is stopped");
  } else {
    const std::string &name = Emu.GetTitleAndTitleID();
    fmt::append(buf, "\nTitle: \"%s\" (emulation is %s)",
                name.empty() ? "N/A" : name.data(),
                state == system_state::stopping ? "stopping" : "running");
  }

  fmt::append(buf, "\nBuild: \"%s\"", rpcs3::get_verbose_version());
  fmt::append(buf, "\nDate: \"%s\"", std::chrono::system_clock::now());

  __android_log_write(ANDROID_LOG_FATAL, "RPCS3", buf.c_str());

  jit_announce(0, 0, "");
  utils::trap();
  std::abort();
  std::terminate();
}

void qt_events_aware_op(int repeat_duration_ms,
                        std::function<bool()> wrapped_op) {
  /// ?????
}

static std::string unwrap(JNIEnv *env, jstring string) {
  auto resultBuffer = env->GetStringUTFChars(string, nullptr);
  std::string result(resultBuffer);
  env->ReleaseStringUTFChars(string, resultBuffer);
  return result;
}
static jstring wrap(JNIEnv *env, const std::string &string) {
  return env->NewStringUTF(string.c_str());
}
static jstring wrap(JNIEnv *env, const char *string) {
  return env->NewStringUTF(string);
}

static std::string fix_dir_path(std::string string) {
  if (!string.empty() && !string.ends_with('/')) {
    string += '/';
  }

  return string;
}

enum class FileType {
  Unknown,
  Pup,
  Pkg,
  Edat,
  Rap,
  Iso,
};

static FileType getFileType(const fs::file &file) {
  file.seek(0);
  if (PUPHeader pupHeader; file.read(pupHeader)) {
    if (pupHeader.magic == "SCEUF\0\0\0"_u64) {
      return FileType::Pup;
    }
  }

  file.seek(0);
  if (PKGHeader pkgHeader; file.read(pkgHeader)) {
    if (pkgHeader.pkg_magic == std::bit_cast<le_t<u32>>("\x7FPKG"_u32)) {
      return FileType::Pkg;
    }
  }

  file.seek(0);
  if (NPD_HEADER npdHeader; file.read(npdHeader)) {
    if (npdHeader.magic == "NPD\0"_u32) {
      return FileType::Edat;
    }
  }

  if (file.size() == 16) {
    return FileType::Rap;
  }

  if (iso_fs::open(std::make_unique<file_view_block_dev>(file))) {
    return FileType::Iso;
  }

  return FileType::Unknown;
}

#define MAKE_STRING(id, x) [int(localized_string_id::id)] = {x, U##x}

static std::pair<std::string, std::u32string> g_strings[] = {
    MAKE_STRING(RSX_OVERLAYS_COMPILING_SHADERS, "Compiling shaders"),
    MAKE_STRING(RSX_OVERLAYS_COMPILING_PPU_MODULES, "Compiling PPU Modules"),
    MAKE_STRING(RSX_OVERLAYS_MSG_DIALOG_YES, "Yes"),
    MAKE_STRING(RSX_OVERLAYS_MSG_DIALOG_NO, "No"),
    MAKE_STRING(RSX_OVERLAYS_MSG_DIALOG_CANCEL, "Back"),
    MAKE_STRING(RSX_OVERLAYS_MSG_DIALOG_OK, "OK"),
    MAKE_STRING(RSX_OVERLAYS_SAVE_DIALOG_TITLE, "Save Dialog"),
    MAKE_STRING(RSX_OVERLAYS_SAVE_DIALOG_DELETE, "Delete Save"),
    MAKE_STRING(RSX_OVERLAYS_SAVE_DIALOG_LOAD, "Load Save"),
    MAKE_STRING(RSX_OVERLAYS_SAVE_DIALOG_SAVE, "Save"),
    MAKE_STRING(RSX_OVERLAYS_OSK_DIALOG_ACCEPT, "Enter"),
    MAKE_STRING(RSX_OVERLAYS_OSK_DIALOG_CANCEL, "Back"),
    MAKE_STRING(RSX_OVERLAYS_OSK_DIALOG_SPACE, "Space"),
    MAKE_STRING(RSX_OVERLAYS_OSK_DIALOG_BACKSPACE, "Backspace"),
    MAKE_STRING(RSX_OVERLAYS_OSK_DIALOG_SHIFT, "Shift"),
    MAKE_STRING(RSX_OVERLAYS_OSK_DIALOG_ENTER_TEXT, "[Enter Text]"),
    MAKE_STRING(RSX_OVERLAYS_OSK_DIALOG_ENTER_PASSWORD, "[Enter Password]"),
    MAKE_STRING(RSX_OVERLAYS_MEDIA_DIALOG_TITLE, "Select media"),
    MAKE_STRING(RSX_OVERLAYS_MEDIA_DIALOG_TITLE_PHOTO_IMPORT,
                "Select photo to import"),
    MAKE_STRING(RSX_OVERLAYS_MEDIA_DIALOG_EMPTY, "No media found."),
    MAKE_STRING(RSX_OVERLAYS_LIST_SELECT, "Enter"),
    MAKE_STRING(RSX_OVERLAYS_LIST_CANCEL, "Back"),
    MAKE_STRING(RSX_OVERLAYS_LIST_DENY, "Deny"),
    MAKE_STRING(CELL_OSK_DIALOG_TITLE, "On Screen Keyboard"),
    MAKE_STRING(
        CELL_OSK_DIALOG_BUSY,
        "The Home Menu can't be opened while the On Screen Keyboard is busy!"),
    MAKE_STRING(CELL_SAVEDATA_CB_BROKEN, "Error - Save data corrupted"),
    MAKE_STRING(CELL_SAVEDATA_CB_FAILURE, "Error - Failed to save or load"),
    MAKE_STRING(CELL_SAVEDATA_CB_NO_DATA, "Error - Save data cannot be found"),
    MAKE_STRING(CELL_SAVEDATA_NO_DATA, "There is no saved data."),
    MAKE_STRING(CELL_SAVEDATA_NEW_SAVED_DATA_TITLE, "New Saved Data"),
    MAKE_STRING(CELL_SAVEDATA_NEW_SAVED_DATA_SUB_TITLE,
                "Select to create a new entry"),
    MAKE_STRING(CELL_SAVEDATA_SAVE_CONFIRMATION,
                "Do you want to save this data?"),
    MAKE_STRING(CELL_SAVEDATA_AUTOSAVE, "Saving..."),
    MAKE_STRING(CELL_SAVEDATA_AUTOLOAD, "Loading..."),
    MAKE_STRING(
        CELL_CROSS_CONTROLLER_FW_MSG,
        "If your system software version on the PS Vita system is earlier than "
        "1.80, you must update the system software to the latest version."),
    MAKE_STRING(CELL_NP_RECVMESSAGE_DIALOG_TITLE, "Select Message"),
    MAKE_STRING(CELL_NP_RECVMESSAGE_DIALOG_TITLE_INVITE, "Select Invite"),
    MAKE_STRING(CELL_NP_RECVMESSAGE_DIALOG_TITLE_ADD_FRIEND, "Add Friend"),
    MAKE_STRING(CELL_NP_RECVMESSAGE_DIALOG_FROM, "From:"),
    MAKE_STRING(CELL_NP_RECVMESSAGE_DIALOG_SUBJECT, "Subject:"),
    MAKE_STRING(CELL_NP_SENDMESSAGE_DIALOG_TITLE, "Select Message To Send"),
    MAKE_STRING(CELL_NP_SENDMESSAGE_DIALOG_TITLE_INVITE, "Send Invite"),
    MAKE_STRING(CELL_NP_SENDMESSAGE_DIALOG_TITLE_ADD_FRIEND, "Add Friend"),
    MAKE_STRING(RECORDING_ABORTED, "Recording aborted!"),
    MAKE_STRING(RPCN_NO_ERROR, "RPCN: No Error"),
    MAKE_STRING(RPCN_ERROR_INVALID_INPUT,
                "RPCN: Invalid Input (Wrong Host/Port)"),
    MAKE_STRING(RPCN_ERROR_WOLFSSL, "RPCN Connection Error: WolfSSL Error"),
    MAKE_STRING(RPCN_ERROR_RESOLVE, "RPCN Connection Error: Resolve Error"),
    MAKE_STRING(RPCN_ERROR_CONNECT, "RPCN Connection Error"),
    MAKE_STRING(RPCN_ERROR_LOGIN_ERROR,
                "RPCN Login Error: Identification Error"),
    MAKE_STRING(RPCN_ERROR_ALREADY_LOGGED,
                "RPCN Login Error: User Already Logged In"),
    MAKE_STRING(RPCN_ERROR_INVALID_LOGIN, "RPCN Login Error: Invalid Username"),
    MAKE_STRING(RPCN_ERROR_INVALID_PASSWORD,
                "RPCN Login Error: Invalid Password"),
    MAKE_STRING(RPCN_ERROR_INVALID_TOKEN, "RPCN Login Error: Invalid Token"),
    MAKE_STRING(RPCN_ERROR_INVALID_PROTOCOL_VERSION,
                "RPCN Misc Error: Protocol Version Error (outdated RPCS3?)"),
    MAKE_STRING(RPCN_ERROR_UNKNOWN, "RPCN: Unknown Error"),
    MAKE_STRING(RPCN_SUCCESS_LOGGED_ON, "Successfully logged on RPCN!"),
    MAKE_STRING(HOME_MENU_TITLE, "Home Menu"),
    MAKE_STRING(HOME_MENU_EXIT_GAME, "Exit Game"),
    MAKE_STRING(HOME_MENU_RESUME, "Resume Game"),
    MAKE_STRING(HOME_MENU_FRIENDS, "Friends"),
    MAKE_STRING(HOME_MENU_FRIENDS_REQUESTS, "Pending Friend Requests"),
    MAKE_STRING(HOME_MENU_FRIENDS_BLOCKED, "Blocked Users"),
    MAKE_STRING(HOME_MENU_FRIENDS_STATUS_ONLINE, "Online"),
    MAKE_STRING(HOME_MENU_FRIENDS_STATUS_OFFLINE, "Offline"),
    MAKE_STRING(HOME_MENU_FRIENDS_STATUS_BLOCKED, "Blocked"),
    MAKE_STRING(HOME_MENU_FRIENDS_REQUEST_SENT, "You sent a friend request"),
    MAKE_STRING(HOME_MENU_FRIENDS_REQUEST_RECEIVED,
                "Sent you a friend request"),
    MAKE_STRING(HOME_MENU_FRIENDS_REJECT_REQUEST, "Reject Request"),
    MAKE_STRING(HOME_MENU_FRIENDS_NEXT_LIST, "Next list"),
    MAKE_STRING(HOME_MENU_RESTART, "Restart Game"),
    MAKE_STRING(HOME_MENU_SETTINGS, "Settings"),
    MAKE_STRING(HOME_MENU_SETTINGS_SAVE, "Save custom configuration?"),
    MAKE_STRING(HOME_MENU_SETTINGS_SAVE_BUTTON, "Save"),
    MAKE_STRING(HOME_MENU_SETTINGS_DISCARD,
                "Discard the current settings' changes?"),
    MAKE_STRING(HOME_MENU_SETTINGS_DISCARD_BUTTON, "Discard"),
    MAKE_STRING(HOME_MENU_SETTINGS_AUDIO, "Audio"),
    MAKE_STRING(HOME_MENU_SETTINGS_AUDIO_MASTER_VOLUME, "Master Volume"),
    MAKE_STRING(HOME_MENU_SETTINGS_AUDIO_BACKEND, "Audio Backend"),
    MAKE_STRING(HOME_MENU_SETTINGS_AUDIO_BUFFERING, "Enable Buffering"),
    MAKE_STRING(HOME_MENU_SETTINGS_AUDIO_BUFFER_DURATION,
                "Desired Audio Buffer Duration"),
    MAKE_STRING(HOME_MENU_SETTINGS_AUDIO_TIME_STRETCHING,
                "Enable Time Stretching"),
    MAKE_STRING(HOME_MENU_SETTINGS_AUDIO_TIME_STRETCHING_THRESHOLD,
                "Time Stretching Threshold"),
    MAKE_STRING(HOME_MENU_SETTINGS_VIDEO, "Video"),
    MAKE_STRING(HOME_MENU_SETTINGS_VIDEO_FRAME_LIMIT, "Frame Limit"),
    MAKE_STRING(HOME_MENU_SETTINGS_VIDEO_ANISOTROPIC_OVERRIDE,
                "Anisotropic Filter Override"),
    MAKE_STRING(HOME_MENU_SETTINGS_VIDEO_OUTPUT_SCALING, "Output Scaling"),
    MAKE_STRING(HOME_MENU_SETTINGS_VIDEO_RCAS_SHARPENING,
                "FidelityFX CAS Sharpening Intensity"),
    MAKE_STRING(HOME_MENU_SETTINGS_VIDEO_STRETCH_TO_DISPLAY,
                "Stretch To Display Area"),
    MAKE_STRING(HOME_MENU_SETTINGS_INPUT, "Input"),
    MAKE_STRING(HOME_MENU_SETTINGS_INPUT_BACKGROUND_INPUT,
                "Background Input Enabled"),
    MAKE_STRING(HOME_MENU_SETTINGS_INPUT_KEEP_PADS_CONNECTED,
                "Keep Pads Connected"),
    MAKE_STRING(HOME_MENU_SETTINGS_INPUT_SHOW_PS_MOVE_CURSOR,
                "Show PS Move Cursor"),
    MAKE_STRING(HOME_MENU_SETTINGS_INPUT_CAMERA_FLIP, "Camera Flip"),
    MAKE_STRING(HOME_MENU_SETTINGS_INPUT_PAD_MODE, "Pad Handler Mode"),
    MAKE_STRING(HOME_MENU_SETTINGS_INPUT_PAD_SLEEP, "Pad Handler Sleep"),
    MAKE_STRING(HOME_MENU_SETTINGS_INPUT_FAKE_MOVE_ROTATION_CONE_H,
                "Fake PS Move Rotation Cone (Horizontal)"),
    MAKE_STRING(HOME_MENU_SETTINGS_INPUT_FAKE_MOVE_ROTATION_CONE_V,
                "Fake PS Move Rotation Cone (Vertical)"),
    MAKE_STRING(HOME_MENU_SETTINGS_ADVANCED, "Advanced"),
    MAKE_STRING(HOME_MENU_SETTINGS_ADVANCED_PREFERRED_SPU_THREADS,
                "Preferred SPU Threads"),
    MAKE_STRING(HOME_MENU_SETTINGS_ADVANCED_MAX_CPU_PREEMPTIONS,
                "Max Power Saving CPU-Preemptions"),
    MAKE_STRING(HOME_MENU_SETTINGS_ADVANCED_ACCURATE_RSX_RESERVATION_ACCESS,
                "Accurate RSX reservation access"),
    MAKE_STRING(HOME_MENU_SETTINGS_ADVANCED_SLEEP_TIMERS_ACCURACY,
                "Sleep Timers Accuracy"),
    MAKE_STRING(HOME_MENU_SETTINGS_ADVANCED_MAX_SPURS_THREADS,
                "Max SPURS Threads"),
    MAKE_STRING(HOME_MENU_SETTINGS_ADVANCED_DRIVER_WAKE_UP_DELAY,
                "Driver Wake-Up Delay"),
    MAKE_STRING(HOME_MENU_SETTINGS_ADVANCED_VBLANK_FREQUENCY,
                "VBlank Frequency"),
    MAKE_STRING(HOME_MENU_SETTINGS_ADVANCED_VBLANK_NTSC, "VBlank NTSC Fixup"),
    MAKE_STRING(HOME_MENU_SETTINGS_OVERLAYS, "Overlays"),
    MAKE_STRING(HOME_MENU_SETTINGS_OVERLAYS_SHOW_TROPHY_POPUPS,
                "Show Trophy Popups"),
    MAKE_STRING(HOME_MENU_SETTINGS_OVERLAYS_SHOW_RPCN_POPUPS,
                "Show RPCN Popups"),
    MAKE_STRING(HOME_MENU_SETTINGS_OVERLAYS_SHOW_SHADER_COMPILATION_HINT,
                "Show Shader Compilation Hint"),
    MAKE_STRING(HOME_MENU_SETTINGS_OVERLAYS_SHOW_PPU_COMPILATION_HINT,
                "Show PPU Compilation Hint"),
    MAKE_STRING(HOME_MENU_SETTINGS_OVERLAYS_SHOW_AUTO_SAVE_LOAD_HINT,
                "Show Autosave/Autoload Hint"),
    MAKE_STRING(HOME_MENU_SETTINGS_OVERLAYS_SHOW_PRESSURE_INTENSITY_TOGGLE_HINT,
                "Show Pressure Intensity Toggle Hint"),
    MAKE_STRING(HOME_MENU_SETTINGS_OVERLAYS_SHOW_ANALOG_LIMITER_TOGGLE_HINT,
                "Show Analog Limiter Toggle Hint"),
    MAKE_STRING(HOME_MENU_SETTINGS_OVERLAYS_SHOW_MOUSE_AND_KB_TOGGLE_HINT,
                "Show Mouse And Keyboard Toggle Hint"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY, "Performance Overlay"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_ENABLE,
                "Enable Performance Overlay"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_ENABLE_FRAMERATE_GRAPH,
                "Enable Framerate Graph"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_ENABLE_FRAMETIME_GRAPH,
                "Enable Frametime Graph"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_DETAIL_LEVEL,
                "Detail level"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_FRAMERATE_DETAIL_LEVEL,
                "Framerate Graph Detail Level"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_FRAMETIME_DETAIL_LEVEL,
                "Frametime Graph Detail Level"),
    MAKE_STRING(
        HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_FRAMERATE_DATAPOINT_COUNT,
        "Framerate Datapoints"),
    MAKE_STRING(
        HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_FRAMETIME_DATAPOINT_COUNT,
        "Frametime Datapoints"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_UPDATE_INTERVAL,
                "Metrics Update Interval"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_POSITION, "Position"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_CENTER_X,
                "Center Horizontally"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_CENTER_Y,
                "Center Vertically"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_MARGIN_X,
                "Horizontal Margin"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_MARGIN_Y,
                "Vertical Margin"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_FONT_SIZE, "Font Size"),
    MAKE_STRING(HOME_MENU_SETTINGS_PERFORMANCE_OVERLAY_OPACITY, "Opacity"),
    MAKE_STRING(HOME_MENU_SETTINGS_DEBUG, "Debug"),
    MAKE_STRING(HOME_MENU_SETTINGS_DEBUG_OVERLAY, "Debug Overlay"),
    MAKE_STRING(HOME_MENU_SETTINGS_DEBUG_INPUT_OVERLAY, "Input Debug Overlay"),
    MAKE_STRING(HOME_MENU_SETTINGS_DEBUG_DISABLE_VIDEO_OUTPUT,
                "Disable Video Output"),
    MAKE_STRING(HOME_MENU_SETTINGS_DEBUG_TEXTURE_LOD_BIAS,
                "Texture LOD Bias Addend"),
    MAKE_STRING(HOME_MENU_SCREENSHOT, "Take Screenshot"),
    MAKE_STRING(HOME_MENU_SAVESTATE, "SaveState"),
    MAKE_STRING(HOME_MENU_SAVESTATE_SAVE, "Save Emulation State"),
    MAKE_STRING(HOME_MENU_SAVESTATE_AND_EXIT, "Save Emulation State And Exit"),
    MAKE_STRING(HOME_MENU_RELOAD_SAVESTATE, "Reload Last Emulation State"),
    MAKE_STRING(HOME_MENU_RECORDING, "Start/Stop Recording"),
    MAKE_STRING(HOME_MENU_TROPHIES, "Trophies"),
    MAKE_STRING(HOME_MENU_TROPHY_HIDDEN_TITLE, "Hidden trophy"),
    MAKE_STRING(HOME_MENU_TROPHY_HIDDEN_DESCRIPTION, "This trophy is hidden"),
    MAKE_STRING(HOME_MENU_TROPHY_PLATINUM_RELEVANT, "Platinum relevant"),
    MAKE_STRING(HOME_MENU_TROPHY_GRADE_BRONZE, "Bronze"),
    MAKE_STRING(HOME_MENU_TROPHY_GRADE_SILVER, "Silver"),
    MAKE_STRING(HOME_MENU_TROPHY_GRADE_GOLD, "Gold"),
    MAKE_STRING(HOME_MENU_TROPHY_GRADE_PLATINUM, "Platinum"),
    MAKE_STRING(AUDIO_MUTED, "Audio muted"),
    MAKE_STRING(AUDIO_UNMUTED, "Audio unmuted"),
    MAKE_STRING(PROGRESS_DIALOG_PROGRESS, "Progress:"),
    MAKE_STRING(PROGRESS_DIALOG_PROGRESS_ANALYZING, "Progress: analyzing..."),
    MAKE_STRING(PROGRESS_DIALOG_REMAINING, "remaining"),
    MAKE_STRING(PROGRESS_DIALOG_DONE, "done"),
    MAKE_STRING(PROGRESS_DIALOG_FILE, "file"),
    MAKE_STRING(PROGRESS_DIALOG_MODULE, "module"),
    MAKE_STRING(PROGRESS_DIALOG_OF, "of"),
    MAKE_STRING(PROGRESS_DIALOG_PLEASE_WAIT, "Please wait"),
    MAKE_STRING(PROGRESS_DIALOG_STOPPING_PLEASE_WAIT,
                "Stopping. Please wait..."),
    MAKE_STRING(PROGRESS_DIALOG_SAVESTATE_PLEASE_WAIT,
                "Creating savestate. Please wait..."),
    MAKE_STRING(PROGRESS_DIALOG_SCANNING_PPU_EXECUTABLE,
                "Scanning PPU Executable..."),
    MAKE_STRING(PROGRESS_DIALOG_ANALYZING_PPU_EXECUTABLE,
                "Analyzing PPU Executable..."),
    MAKE_STRING(PROGRESS_DIALOG_SCANNING_PPU_MODULES,
                "Scanning PPU Modules..."),
    MAKE_STRING(PROGRESS_DIALOG_LOADING_PPU_MODULES, "Loading PPU Modules..."),
    MAKE_STRING(PROGRESS_DIALOG_COMPILING_PPU_MODULES,
                "Compiling PPU Modules..."),
    MAKE_STRING(PROGRESS_DIALOG_LINKING_PPU_MODULES, "Linking PPU Modules..."),
    MAKE_STRING(PROGRESS_DIALOG_APPLYING_PPU_CODE, "Applying PPU Code..."),
    MAKE_STRING(PROGRESS_DIALOG_BUILDING_SPU_CACHE, "Building SPU Cache..."),
    MAKE_STRING(EMULATION_PAUSED_RESUME_WITH_START,
                "Press and hold the START button to resume"),
    MAKE_STRING(EMULATION_RESUMING, "Resuming...!"),
    MAKE_STRING(EMULATION_FROZEN,
                "The PS3 application has likely crashed, you can close it."),
    MAKE_STRING(
        SAVESTATE_FAILED_DUE_TO_SAVEDATA,
        "SaveState failed: Game saving is in progress, wait until finished."),
    MAKE_STRING(SAVESTATE_FAILED_DUE_TO_VDEC,
                "SaveState failed: VDEC-base video/cutscenes are in order, "
                "wait for them to end or enable libvdec.sprx."),
    MAKE_STRING(SAVESTATE_FAILED_DUE_TO_MISSING_SPU_SETTING,
                "SaveState failed: Failed to lock SPU state, enabling "
                "SPU-Compatible mode may fix it."),
    MAKE_STRING(SAVESTATE_FAILED_DUE_TO_SPU,
                "SaveState failed: Failed to lock SPU state, using SPU ASMJIT "
                "will fix it."),
    MAKE_STRING(INVALID, "Invalid"),
};

enum GameFlags {
  kGameFlagLocked = 1 << 0,
  kGameFlagTrial = 1 << 1,
};

struct GameInfo {
  std::string path;
  std::string name;
  std::string iconPath;
  int flags = 0;
};

class Progress {
  JNIEnv *env;
  jlong progressId;
  jclass progressRepositoryClass;
  jmethodID onProgressEventMethodId;

public:
  Progress(JNIEnv *env, jlong progressId) : env(env), progressId(progressId) {
    progressRepositoryClass =
        ensure(env->FindClass("net/rpcs3/ProgressRepository"));
    onProgressEventMethodId = env->GetStaticMethodID(
        progressRepositoryClass, "onProgressEvent", "(JJJLjava/lang/String;)Z");
  }

  bool report(jlong value, jlong max, const std::string &message = {}) {
    return env->CallStaticBooleanMethod(
        progressRepositoryClass, onProgressEventMethodId, progressId, value,
        max, message.empty() ? nullptr : wrap(env, message));
  }

  void failure(const std::string &message = {}) { report(-1, 0, message); }

  void success(jlong value, const std::string &message = {}) {
    value = std::max<jlong>(value, 1);
    report(value, value, message);
  }

  jlong getProgressId() const { return progressId; }
};

static void sendFirmwareInstalled(JNIEnv *env, std::string version) {
  auto fwRepositoryClass =
      ensure(env->FindClass("net/rpcs3/FirmwareRepository"));
  auto methodId = ensure(env->GetStaticMethodID(
      fwRepositoryClass, "onFirmwareInstalled", "(Ljava/lang/String;)V"));

  env->CallStaticVoidMethod(fwRepositoryClass, methodId, wrap(env, version));
}

static void sendFirmwareCompiled(JNIEnv *env, std::string version) {
  auto fwRepositoryClass =
      ensure(env->FindClass("net/rpcs3/FirmwareRepository"));
  auto methodId = ensure(env->GetStaticMethodID(
      fwRepositoryClass, "onFirmwareCompiled", "(Ljava/lang/String;)V"));

  env->CallStaticVoidMethod(fwRepositoryClass, methodId, wrap(env, version));
}

static void sendGameInfo(JNIEnv *env, jlong progressId,
                         std::span<const GameInfo> infos) {
  auto gameRepositoryClass = ensure(env->FindClass("net/rpcs3/GameRepository"));
  auto addMethodId = ensure(env->GetStaticMethodID(
      gameRepositoryClass, "add", "([Lnet/rpcs3/GameInfo;J)V"));
  auto gameClass = ensure(env->FindClass("net/rpcs3/GameInfo"));

  jmethodID gameConstructor = ensure(env->GetMethodID(
      gameClass, "<init>",
      "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)V"));

  std::vector<jobject> objects;
  objects.reserve(infos.size());

  for (const auto &info : infos) {
    auto path = Emu.GetCallbacks().resolve_path(info.path);
    if (path.ends_with('/')) {
      path.resize(path.size() - 1);
    }

    objects.push_back(env->NewObject(
        gameClass, gameConstructor, wrap(env, path), wrap(env, info.name),
        wrap(env, Emu.GetCallbacks().resolve_path(info.iconPath)),
        jint(info.flags)));
  }

  auto result = env->NewObjectArray(objects.size(), gameClass, nullptr);

  for (std::size_t i = 0; i < objects.size(); ++i) {
    env->SetObjectArrayElement(result, i, objects[i]);
  }

  env->CallStaticVoidMethod(gameRepositoryClass, addMethodId, result,
                            progressId);
}

static void sendVshBootable(JNIEnv *env, jlong progressId) {
  auto dev_flash = g_cfg_vfs.get_dev_flash();

  sendGameInfo(
      env, progressId,
      {{GameInfo{
          .path = dev_flash + "/vsh/module/vsh.self",
          .name = "VSH",
          .iconPath = dev_flash + "vsh/resource/explore/icon/icon_home.png",
      }}});
}

static bool tryUnlockGame(const psf::registry &psf) {
  auto contentId = psf::get_string(psf, "CONTENT_ID");

  if (contentId.empty()) {
    return true;
  }

  const auto licenseDir = fmt::format(
      "%shome/%s/exdata/", rpcs3::utils::get_hdd0_dir(), Emu.GetUsr());

  const auto licenseFile = fmt::format("%s%s", licenseDir, contentId);
  if (std::filesystem::is_regular_file(licenseFile + ".rap")) {
    return true;
  }

  if (std::filesystem::is_regular_file(licenseFile + ".edat")) {
    return true;
  }

  return false;
}

static void collectGamePaths(std::vector<std::string> &paths,
                             const std::string &rootDir) {
  std::error_code ec;
  std::vector<std::filesystem::path> workList;
  workList.reserve(32);
  if (!std::filesystem::is_directory(rootDir)) {
    auto rootPath = std::filesystem::path(rootDir).parent_path();
    if (rootPath.filename() == "USRDIR") {
      rootPath = rootPath.parent_path();
    }
    if (rootPath.filename() == "PS3_GAME") {
      rootPath = rootPath.parent_path();
    }

    workList.push_back(rootPath);
  } else {
    workList.push_back(rootDir);
  }

  while (!workList.empty()) {
    auto dir = std::move(workList.back());
    workList.pop_back();

    for (auto entry : std::filesystem::directory_iterator(dir, ec)) {
      if (entry.is_directory()) {
        if (entry.path().filename() != "C00") {
          workList.push_back(entry.path());
        }

        continue;
      }

      if (entry.is_regular_file() && entry.path().filename() == "PARAM.SFO") {
        paths.push_back(entry.path().parent_path().string());
        continue;
      }
    }
  }
}

static std::string locateEbootPath(const std::string &root) {
  if (std::filesystem::is_regular_file(root)) {
    return root;
  }

  for (auto suffix : {
           "/EBOOT.BIN",
           "/USRDIR/EBOOT.BIN",
           "/USRDIR/ISO.BIN.EDAT",
           "/PS3_GAME/USRDIR/EBOOT.BIN",
       }) {
    std::string tryPath = root + suffix;

    if (std::filesystem::is_regular_file(tryPath)) {
      return tryPath;
    }
  }

  return {};
}

static std::optional<GameInfo> fetchGameInfo(const psf::registry &psf) {
  auto titleId = std::string(psf::get_string(psf, "TITLE_ID"));
  auto name = std::string(psf::get_string(psf, "TITLE"));
  auto bootable = psf::get_integer(psf, "BOOTABLE", 0);
  auto category = psf::get_string(psf, "CATEGORY");

  if (!bootable || titleId.empty()) {
    return {};
  }

  bool isDiscGame = category == "DG";

  auto path = isDiscGame
                  ? fs::get_config_dir() + "games/" + titleId + "/"
                  : rpcs3::utils::get_hdd0_dir() + "game/" + titleId + "/";
  auto dataPath = isDiscGame ? path + "PS3_GAME/" : path;
  auto iconPath = dataPath + "ICON0.PNG";
  auto moviePath = dataPath + "ICON1.PAM";

  int flags = 0;

  if (!isDiscGame) {
    auto ebootPath = locateEbootPath(path);

    bool isLocked = false;

    if (!ebootPath.empty()) {
      if (fs::file eboot{ebootPath};
          eboot && eboot.size() >= 4 && eboot.read<u32>() == "SCE\0"_u32) {
        isLocked = !decrypt_self(eboot);
      }
    }

    if (isLocked) {
      flags |= kGameFlagLocked;
      rpcs3_android.warning("game %s is locked", path);
    }

    auto c00Path = path + "/C00";

    bool isTrial = std::filesystem::is_directory(c00Path);

    if (isTrial) {
      if (!tryUnlockGame(psf)) {
        flags |= kGameFlagTrial;
        rpcs3_android.warning("game %s is trial", path);
      } else {
        auto c00IconPath = c00Path + "/ICON0.PNG";
        if (std::filesystem::is_regular_file(c00IconPath)) {
          iconPath = c00IconPath;
        }

        auto c00SfoPath = c00Path + "/PARAM.SFO";

        if (std::filesystem::is_regular_file(c00IconPath)) {
          auto c00Sfo = psf::load_object(c00SfoPath);
          titleId = psf::get_string(c00Sfo, "TITLE_ID", titleId);
          name = psf::get_string(c00Sfo, "TITLE", name);
        }
      }
    }
  }

  return GameInfo{
      .path = std::move(path),
      .name = std::move(name),
      .iconPath = std::move(iconPath),
      .flags = flags,
  };
}

static void collectGameInfo(JNIEnv *env, jlong progressId,
                            std::vector<std::string> rootDirs) {
  std::vector<std::string> paths;
  for (auto &&rootDir : rootDirs) {
    collectGamePaths(paths, rootDir);

    rpcs3_android.notice("collectGameInfo: processed %s", rootDir);
  }

  rpcs3_android.notice("collectGameInfo: found %d paths", paths.size());

  Progress progress(env, progressId);
  progress.report(0, paths.size());

  std::vector<GameInfo> gameInfos;
  gameInfos.reserve(10);
  std::size_t processed = 0;

  auto submit = [&] {
    if (gameInfos.empty()) {
      return;
    }

    sendGameInfo(env, progressId, gameInfos);
    progress.report(processed, paths.size());
    gameInfos.clear();
  };

  for (auto &&path : paths) {
    processed++;

    if (!std::filesystem::is_regular_file(path + "/PARAM.SFO")) {
      continue;
    }

    const auto psf = psf::load_object(path + "/PARAM.SFO");

    rpcs3_android.notice("collectGameInfo: sfo at %s", path);

    if (auto gameInfo = fetchGameInfo(psf)) {
      gameInfos.push_back(std::move(*gameInfo));

      if (gameInfos.size() >= 10) {
        submit();
      }
    }
  }

  submit();

  progress.success(processed);
}

class MainThreadProcessor {
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<std::pair<std::function<void(JNIEnv *)>, atomic_t<u32> *>> queue;

public:
  void push(std::function<void(JNIEnv *)> cb, atomic_t<u32> *wakeUp = nullptr) {
    std::lock_guard lock(mutex);
    queue.push_back({std::move(cb), wakeUp});
    cv.notify_one();
  }

  void push(std::function<void()> cb, atomic_t<u32> *wakeUp = nullptr) {
    push([cb = std::move(cb)](JNIEnv *) { cb(); }, wakeUp);
  }

  void process(JNIEnv *env) {
    while (true) {
      std::function<void(JNIEnv *)> cb;
      atomic_t<u32> *wakeUp = nullptr;

      {
        std::unique_lock lock(mutex);
        if (queue.empty()) {
          cv.wait(lock);
          continue;
        }

        auto item = std::move(queue.front());
        queue.pop_front();

        cb = std::move(item.first);
        wakeUp = item.second;
      }

      cb(env);
      if (wakeUp) {
        *wakeUp = true;
        wakeUp->notify_all();
      }
    }
  }
} static g_mainThreadProcessor;

static void invokeAsync(std::function<void(JNIEnv *)> cb) {
  g_mainThreadProcessor.push(std::move(cb));
}

static void invokeSync(std::function<void(JNIEnv *)> cb) {
  atomic_t<u32> wakeup{false};
  g_mainThreadProcessor.push(std::move(cb), &wakeup);

  while (wakeup.load() == false) {
    wakeup.wait(false);
  }
}

struct ProgressMessageDialog : MsgDialogBase {
  jlong progressId;
  jlong value = 0;
  jlong max = 0;

  ProgressMessageDialog(jlong progressId) : progressId(progressId) {}

  void Create(const std::string &msg, const std::string &title) override {
    rpcs3_android.warning("ProgressMessageDialog::Create(%s, %s)", msg, title);
    max = 100;
    invokeSync([this, &msg](JNIEnv *env) {
      Progress progress(env, progressId);
      progress.report(0, 0, msg);
    });
  }

  jlong getValue() const {
    return value == max && max != 0 ? value - 1 : value;
  }

  void Close(bool success) override {
    rpcs3_android.warning("ProgressMessageDialog::Close(%s)", success);
    invokeSync([this, success](JNIEnv *env) {
      Progress progress(env, progressId);
      progress.report(0, 0);
    });

    //   Progress progress(env, progressId);
    //   if (success) {
    //     progress.success(0);
    //   } else {
    //     progress.failure();
    //   }
    // });
  }

  void SetMsg(const std::string &msg) override {
    rpcs3_android.warning("ProgressMessageDialog::SetMsg(%s)", msg);
    invokeSync([this, msg](JNIEnv *env) {
      Progress(env, progressId).report(getValue(), max, msg);
    });
  }

  void ProgressBarSetMsg(u32 progressBarIndex,
                         const std::string &msg) override {
    rpcs3_android.warning("ProgressMessageDialog::ProgressBarSetMsg(%d, %s)",
                          progressBarIndex, msg);
    if (progressBarIndex != 0) {
      report_fatal_error("Unexpected progress index in progress dialog");
    }

    invokeSync([this, msg](JNIEnv *env) {
      Progress(env, progressId).report(getValue(), max, msg);
    });
  }

  void ProgressBarReset(u32 progressBarIndex) override {
    rpcs3_android.warning("ProgressMessageDialog::ProgressBarReset(%d)",
                          progressBarIndex);

    if (progressBarIndex != 0) {
      report_fatal_error("Unexpected progress index in progress dialog");
    }

    value = 0;
    invokeSync(
        [this](JNIEnv *env) { Progress(env, progressId).report(value, max); });
  }

  void ProgressBarInc(u32 progressBarIndex, u32 delta) override {
    rpcs3_android.warning("ProgressMessageDialog::ProgressBarInc(%d, %d)",
                          progressBarIndex, delta);

    if (progressBarIndex != 0) {
      report_fatal_error("Unexpected progress index in progress dialog");
    }

    value += delta;

    invokeSync([this](JNIEnv *env) {
      Progress(env, progressId).report(getValue(), max);
    });
  }

  void ProgressBarSetValue(u32 progressBarIndex, u32 value) override {
    rpcs3_android.warning("ProgressMessageDialog::ProgressBarSetValue(%d, %d)",
                          progressBarIndex, value);

    if (progressBarIndex != 0) {
      report_fatal_error("Unexpected progress index in progress dialog");
    }

    this->value = value;

    invokeSync([this](JNIEnv *env) {
      Progress(env, progressId).report(getValue(), max);
    });
  }
  void ProgressBarSetLimit(u32 index, u32 limit) override {
    rpcs3_android.warning("ProgressMessageDialog::ProgressBarSetLimit(%d, %d)",
                          index, limit);

    if (index != 0) {
      report_fatal_error("Unexpected progress index in progress dialog");
    }

    max = limit;

    invokeSync([this](JNIEnv *env) {
      Progress(env, progressId).report(getValue(), max);
    });
  }
};

struct UiMessageDialog : MsgDialogBase {
  // FIXME: implement

  void Create(const std::string &msg, const std::string &title) override {}
  void Close(bool success) override {}
  void SetMsg(const std::string &msg) override {}
  void ProgressBarSetMsg(u32 progressBarIndex,
                         const std::string &msg) override {}
  void ProgressBarReset(u32 progressBarIndex) override {}
  void ProgressBarInc(u32 progressBarIndex, u32 delta) override {}
  void ProgressBarSetValue(u32 progressBarIndex, u32 value) override {}
  void ProgressBarSetLimit(u32 index, u32 limit) override {}
};

struct MessageDialog : MsgDialogBase {
  std::unique_ptr<MsgDialogBase> impl = nullptr;

  void Create(const std::string &msg, const std::string &title) override {
    auto progressId = s_pendingProgressId.load();

    rpcs3_android.warning("MessageDialog::Create(%s, %s): source %s, id %d",
                          msg, title, source, progressId);

    if (progressId != -1) {
      impl = std::make_unique<ProgressMessageDialog>(progressId);
    } else {
      impl = std::make_unique<UiMessageDialog>();
    }

    impl->type = type;
    impl->source = source;
    impl->Create(msg, title);
  }

  void Close(bool success) override { impl->Close(success); }

  void SetMsg(const std::string &msg) override { impl->SetMsg(msg); }

  void ProgressBarSetMsg(u32 progressBarIndex,
                         const std::string &msg) override {
    impl->ProgressBarSetMsg(progressBarIndex, msg);
  }

  void ProgressBarReset(u32 progressBarIndex) override {
    impl->ProgressBarReset(progressBarIndex);
  }

  void ProgressBarInc(u32 progressBarIndex, u32 delta) override {
    impl->ProgressBarInc(progressBarIndex, delta);
  }

  void ProgressBarSetValue(u32 progressBarIndex, u32 value) override {
    impl->ProgressBarSetValue(progressBarIndex, value);
  }

  void ProgressBarSetLimit(u32 index, u32 limit) override {
    impl->ProgressBarSetLimit(index, limit);
  }

  static void pushPendingProgressId(jlong id) {
    jlong value = -1;

    while (!s_pendingProgressId.compare_exchange_weak(value, id)) {
      s_pendingProgressId.wait(value);
      value = -1;
    }
  }

  static bool popPendingProgressId(jlong id) {
    return s_pendingProgressId.compare_exchange_strong(id, -1);
  }

private:
  static std::atomic<jlong> s_pendingProgressId;
};

std::atomic<jlong> MessageDialog::s_pendingProgressId = -1;

struct CompilationWorkload {
  jlong progressId;
  std::string path;
};

extern bool ppu_load_exec(const ppu_exec_object &, bool virtual_load,
                          const std::string &, utils::serial * = nullptr);
extern void spu_load_exec(const spu_exec_object &);
extern void spu_load_rel_exec(const spu_rel_object &);
extern void ppu_precompile(std::vector<std::string> &dir_queue,
                           std::vector<ppu_module<lv2_obj> *> *loaded_prx);
extern bool ppu_initialize(const ppu_module<lv2_obj> &, bool check_only = false,
                           u64 file_size = 0);
extern void ppu_finalize(const ppu_module<lv2_obj> &);
extern bool ppu_load_rel_exec(const ppu_rel_object &);

class CompilationQueue {
  std::atomic<std::uint64_t> nextWorkTag{0};
  std::uint64_t lastProcessedTag = 0;
  std::mutex queueMutex;
  std::deque<CompilationWorkload> queue;

public:
  void push(CompilationWorkload workload) {
    {
      std::lock_guard lock(queueMutex);
      queue.push_back(std::move(workload));
    }

    nextWorkTag.fetch_add(1);
  }

  void push(Progress &progress, std::string path) {
    progress.report(0, 0);

    push({
        .progressId = progress.getProgressId(),
        .path = std::move(path),
    });
  }

  void process(JNIEnv *env) {
    while (true) {
      auto nextWorkTagValue = nextWorkTag.load();

      if (nextWorkTagValue == lastProcessedTag) {
        nextWorkTag.wait(lastProcessedTag);
      }

      if (nextWorkTagValue == lastProcessedTag || queue.empty()) {
        continue;
      }

      CompilationWorkload workload;

      {
        std::lock_guard lock(queueMutex);

        if (queue.empty()) {
          continue;
        }

        workload = std::move(queue.front());
        queue.pop_front();
      }

      impl(env, std::move(workload));
      lastProcessedTag++;
    }
  }

private:
  void impl(JNIEnv *env, CompilationWorkload workload) {
    if (workload.path.empty()) {
      Progress(env, workload.progressId).success(0);
      return;
    }

    rpcs3_android.error("Creating cache initiated, state %d",
                        (int)Emu.GetStatus(false));

    while (true) {
      auto state = Emu.GetStatus(false);

      if (state == system_state::stopped || state == system_state::ready) {
        break;
      }

      rpcs3_android.error("Creating cache wait, state %d", (int)state);
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    bool is_vsh = workload.path.ends_with("/vsh.self");

    Emu.SetTestMode();

    MessageDialog::pushPendingProgressId(workload.progressId);

    vm::init();
    g_fxo->init<named_thread<progress_dialog_server>>();
    g_fxo->init<main_ppu_module<lv2_obj>>();

    void init_ppu_functions(utils::serial * ar, bool full);
    init_ppu_functions(nullptr, true);

    g_fxo->init(false, nullptr);
    auto rootPath = std::filesystem::path(workload.path);

    if (is_vsh) {
      rootPath = g_cfg_vfs.get_dev_flash() + "sys/external/";
    } else {
      if (!std::filesystem::is_directory(rootPath)) {
        rootPath = rootPath.parent_path();
        if (rootPath.filename() == "USRDIR") {
          rootPath = rootPath.parent_path();
        }
      }
    }

    g_cfg.core.llvm_precompilation.set(true);
    g_cfg.core.spu_cache.set(true);
    g_cfg.core.llvm_threads.set(2);
    g_cfg.core.spu_decoder.set(spu_decoder_type::llvm);
    g_cfg.core.ppu_decoder.set(ppu_decoder_type::llvm);

    g_cfg.core.libraries_control.set_set([]() {
      std::set<std::string> set;

      extern const std::map<std::string_view, int> g_prx_list;

      for (const auto &lib : g_prx_list) {
        set.emplace(std::string(lib.first) + ":lle");
      }

      return set;
    }());

    auto &_main = *ensure(g_fxo->try_get<main_ppu_module<lv2_obj>>());

    if (fs::is_file(workload.path)) {
      if (!is_vsh) {
        auto sfoPath = rootPath / "PARAM.SFO";

        if (std::filesystem::is_regular_file(sfoPath)) {
          const auto psf = psf::load_object(sfoPath);
          rpcs3_android.warning("title id is %s",
                                psf::get_string(psf, "TITLE_ID"));

          Emu.SetTitleID(std::string(psf::get_string(psf, "TITLE_ID")));
        } else {
          rpcs3_android.warning("param.sfo not found");
        }
      }

      // Compile binary first
      rpcs3_android.notice("Trying to load binary: %s", workload.path);

      fs::file src{workload.path};
      src = decrypt_self(src);

      const ppu_exec_object obj = src;

      if (obj == elf_error::ok && ppu_load_exec(obj, true, workload.path)) {
        _main.path = workload.path;
      } else {
        rpcs3_android.error("Failed to load binary '%s' (%s)", workload.path,
                            obj.get_error());
      }
    }

    std::vector<std::string> dir_queue;
    dir_queue.push_back(rootPath.string());

    for (auto entry : std::filesystem::recursive_directory_iterator(rootPath)) {
      if (entry.is_directory()) {
        dir_queue.push_back(entry.path().string());
      }
    }

    std::vector<ppu_module<lv2_obj> *> mod_list;
    rpcs3_android.error("Going to analyze executable");

    // FIXME: split states
    if (!is_vsh) {
      if (_main.analyse(0, _main.elf_entry, _main.seg0_code_end,
                        _main.applied_patches, std::vector<u32>{})) {
        Emu.ConfigurePPUCache();
        Emu.SetTestMode();
        rpcs3_android.error("Going to precompile main PPU module");
        ppu_initialize(_main);
        mod_list.emplace_back(&_main);
      }
    }

    rpcs3_android.error("Going to precompile PPU");
    ppu_precompile(dir_queue, mod_list.empty() ? nullptr : &mod_list);
    rpcs3_android.error("Going to precompile SPU");
    spu_cache::initialize(false);

    rpcs3_android.error("Finalization");
    Emu.Kill();

    MessageDialog::popPendingProgressId(workload.progressId);

    Progress(env, workload.progressId).success(0);
  }
} static g_compilationQueue;

static void setupCallbacks() {
  Emu.SetCallbacks({
      .call_from_main_thread =
          [](std::function<void()> cb, atomic_t<u32> *wake_up) {
            cb();
            if (wake_up) {
              *wake_up = true;
            }
          },
      .on_run = [](auto...) {},
      .on_pause = [](auto...) {},
      .on_resume = [](auto...) {},
      .on_stop = [](auto...) {},
      .on_ready = [](auto...) {},
      .on_missing_fw = [](auto...) {},
      .on_emulation_stop_no_response = [](auto...) {},
      .on_save_state_progress = [](auto...) {},
      .enable_disc_eject = [](auto...) {},
      .enable_disc_insert = [](auto...) {},
      .try_to_quit = [](auto...) { return true; },
      .handle_taskbar_progress = [](auto...) {},
      .init_kb_handler =
          [](auto...) {
            ensure(g_fxo->init<KeyboardHandlerBase, NullKeyboardHandler>(
                Emu.DeserialManager()));
          },
      .init_mouse_handler =
          [](auto...) {
            ensure(g_fxo->init<MouseHandlerBase, NullMouseHandler>(
                Emu.DeserialManager()));
          },
      .init_pad_handler =
          [](auto...) {
            ensure(g_fxo->init<named_thread<pad_thread>>(nullptr, nullptr, ""));
          },
      .update_emu_settings = [](auto...) {},
      .save_emu_settings =
          [](auto...) {
            Emulator::SaveSettings(g_cfg.to_string(), Emu.GetTitleID());
          },
      .close_gs_frame = [](auto...) {},
      .get_gs_frame = [] { return std::make_unique<GraphicsFrame>(); },
      .get_camera_handler =
          [](auto...) { return std::make_shared<null_camera_handler>(); },
      .get_music_handler =
          [](auto...) { return std::make_shared<null_music_handler>(); },
      .init_gs_render =
          [](utils::serial *ar) {
            switch (g_cfg.video.renderer.get()) {
            case video_renderer::null:
              g_fxo->init<rsx::thread, named_thread<NullGSRender>>(ar);
              break;
            case video_renderer::vulkan:
              g_fxo->init<rsx::thread, named_thread<VKGSRender>>(ar);
              break;

            default:
              break;
            }
          },
      .get_audio =
          [](auto...) {
            std::shared_ptr<AudioBackend> result =
                std::make_shared<CubebBackend>();
            if (!result->Initialized()) {
              rpcs3_android.error(
                  "Audio renderer %s could not be initialized, using a Null "
                  "renderer instead. Make sure that no other application is "
                  "running that might block audio access (e.g. Netflix).",
                  result->GetName());
              result = std::make_shared<NullAudioBackend>();
            }
            return result;
          },
      .get_audio_enumerator = [](auto...) { return nullptr; },
      .get_msg_dialog = [] { return std::make_shared<MessageDialog>(); },
      .get_osk_dialog = [](auto...) { return nullptr; },
      .get_save_dialog = [](auto...) { return nullptr; },
      .get_sendmessage_dialog = [](auto...) { return nullptr; },
      .get_recvmessage_dialog = [](auto...) { return nullptr; },
      .get_trophy_notification_dialog = [](auto...) { return nullptr; },
      .get_localized_string = [](localized_string_id id,
                                 const char *) -> std::string {
        if (int(id) < std::size(g_strings)) {
          return g_strings[int(id)].first;
        }
        return "";
      },
      .get_localized_u32string = [](localized_string_id id,
                                    const char *) -> std::u32string {
        if (int(id) < std::size(g_strings)) {
          return g_strings[int(id)].second;
        }
        return U"";
      },
      .get_localized_setting = [](auto...) { return ""; },
      .play_sound = [](auto...) {},
      .get_image_info = [](auto...) { return false; },
      .get_scaled_image = [](auto...) { return false; },
      .resolve_path =
          [](std::string_view arg) {
            std::error_code ec;
            auto result =
                std::filesystem::weakly_canonical(
                    std::filesystem::path(fmt::replace_all(arg, "\\", "/")), ec)
                    .string();
            return ec ? std::string(arg) : result;
          },
      .get_font_dirs = [](auto...) { return std::vector<std::string>(); },
      .on_install_pkgs = [](auto...) { return false; },
      .add_breakpoint = [](auto...) {},
      .display_sleep_control_supported = [](auto...) { return false; },
      .enable_display_sleep = [](auto...) {},
      .check_microphone_permissions = [](auto...) {},
  });
}

static bool initVirtualPad(const std::shared_ptr<Pad> &pad) {
  u32 pclass_profile = 0;
  pad->Init(CELL_PAD_STATUS_CONNECTED,
            CELL_PAD_CAPABILITY_PS3_CONFORMITY |
                CELL_PAD_CAPABILITY_PRESS_MODE |
                CELL_PAD_CAPABILITY_HP_ANALOG_STICK |
                CELL_PAD_CAPABILITY_ACTUATOR //| CELL_PAD_CAPABILITY_SENSOR_MODE
            ,
            CELL_PAD_DEV_TYPE_STANDARD, CELL_PAD_PCLASS_TYPE_STANDARD,
            pclass_profile, 0, 0, 50);

  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, std::set<u32>{},
                              CELL_PAD_CTRL_UP);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, std::set<u32>{},
                              CELL_PAD_CTRL_DOWN);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, std::set<u32>{},
                              CELL_PAD_CTRL_LEFT);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, std::set<u32>{},
                              CELL_PAD_CTRL_RIGHT);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, std::set<u32>{},
                              CELL_PAD_CTRL_CROSS);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, std::set<u32>{},
                              CELL_PAD_CTRL_SQUARE);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, std::set<u32>{},
                              CELL_PAD_CTRL_CIRCLE);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, std::set<u32>{},
                              CELL_PAD_CTRL_TRIANGLE);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, std::set<u32>{},
                              CELL_PAD_CTRL_L1);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, std::set<u32>{},
                              CELL_PAD_CTRL_L2);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, std::set<u32>{},
                              CELL_PAD_CTRL_L3);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, std::set<u32>{},
                              CELL_PAD_CTRL_R1);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, std::set<u32>{},
                              CELL_PAD_CTRL_R2);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, std::set<u32>{},
                              CELL_PAD_CTRL_R3);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, std::set<u32>{},
                              CELL_PAD_CTRL_START);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, std::set<u32>{},
                              CELL_PAD_CTRL_SELECT);
  pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, std::set<u32>{},
                              CELL_PAD_CTRL_PS);

  pad->m_sticks[0] = AnalogStick(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X, {}, {});
  pad->m_sticks[1] = AnalogStick(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y, {}, {});
  pad->m_sticks[2] = AnalogStick(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X, {}, {});
  pad->m_sticks[3] = AnalogStick(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y, {}, {});

  pad->m_sensors[0] =
      AnalogSensor(CELL_PAD_BTN_OFFSET_SENSOR_X, 0, 0, 0, DEFAULT_MOTION_X);
  pad->m_sensors[1] =
      AnalogSensor(CELL_PAD_BTN_OFFSET_SENSOR_Y, 0, 0, 0, DEFAULT_MOTION_Y);
  pad->m_sensors[2] =
      AnalogSensor(CELL_PAD_BTN_OFFSET_SENSOR_Z, 0, 0, 0, DEFAULT_MOTION_Z);
  pad->m_sensors[3] =
      AnalogSensor(CELL_PAD_BTN_OFFSET_SENSOR_G, 0, 0, 0, DEFAULT_MOTION_G);

  pad->m_vibrateMotors[0] = VibrateMotor(true, 0);
  pad->m_vibrateMotors[1] = VibrateMotor(false, 0);

  if (pad->m_player_id == 0) {
    std::lock_guard lock(g_virtual_pad_mutex);
    g_virtual_pad = pad;
  }
  return true;
}

extern "C" JNIEXPORT jboolean JNICALL Java_net_rpcs3_RPCS3_overlayPadData(
    JNIEnv *env, jobject, jint digital1, jint digital2, jint leftStickX,
    jint leftStickY, jint rightStickX, jint rightStickY) {

  auto pad = [] {
    std::shared_ptr<Pad> result;
    std::lock_guard lock(g_virtual_pad_mutex);
    result = g_virtual_pad;
    return result;
  }();

  if (pad == nullptr) {
    return false;
  }

  for (auto &btn : pad->m_buttons) {
    if (btn.m_offset == CELL_PAD_BTN_OFFSET_DIGITAL1) {
      btn.m_pressed = (digital1 & btn.m_outKeyCode) != 0;
      btn.m_value = btn.m_pressed ? 127 : 0;
    } else if (btn.m_offset == CELL_PAD_BTN_OFFSET_DIGITAL2) {
      btn.m_pressed = (digital2 & btn.m_outKeyCode) != 0;
      btn.m_value = btn.m_pressed ? 127 : 0;
    }
  }

  pad->m_sticks[0].m_value = leftStickX;
  pad->m_sticks[1].m_value = leftStickY;
  pad->m_sticks[2].m_value = rightStickX;
  pad->m_sticks[3].m_value = rightStickY;
  return true;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_net_rpcs3_RPCS3_initialize(JNIEnv *env, jobject, jstring rootDir) {
  auto rootDirStr = fix_dir_path(unwrap(env, rootDir));

  if (g_android_executable_dir != rootDirStr) {
    g_android_executable_dir = rootDirStr;
    g_android_config_dir = rootDirStr + "config/";
    g_android_cache_dir = rootDirStr + "cache/";

    std::filesystem::create_directories(g_android_config_dir);
    std::error_code ec;
    // std::filesystem::remove_all(g_android_cache_dir, ec);
    std::filesystem::create_directories(g_android_cache_dir);
  }

  if (g_initialized) {
    return true;
  }

  g_initialized = true;

  if (int r = libusb_set_option(nullptr, LIBUSB_OPTION_NO_DEVICE_DISCOVERY,
                                nullptr);
      r != 0) {
    rpcs3_android.warning(
        "libusb_set_option(LIBUSB_OPTION_NO_DEVICE_DISCOVERY) -> %d", r);
  }

  logs::stored_message ver{rpcs3_android.always()};
  ver.text = fmt::format("RPCS3 v%s", rpcs3::get_verbose_version());

  // Write System information
  logs::stored_message sys{rpcs3_android.always()};
  sys.text = utils::get_system_info();

  // Write OS version
  logs::stored_message os{rpcs3_android.always()};
  os.text = utils::get_OS_version_string();

  // Write current time
  logs::stored_message time{rpcs3_android.always()};
  time.text = fmt::format("Current Time: %s", std::chrono::system_clock::now());

  logs::set_init(
      {std::move(ver), std::move(sys), std::move(os), std::move(time)});

  auto set_rlim = [](int resource, std::uint64_t limit) {
    rlimit64 rlim{};
    if (getrlimit64(resource, &rlim) != 0) {
      rpcs3_android.error("failed to get rlimit for %d", resource);
      return;
    }

    rlim.rlim_cur = std::min<std::size_t>(rlim.rlim_max, limit);
    rpcs3_android.error("rlimit[%d] = %u (requested %u, max %u)", resource,
                        rlim.rlim_cur, limit, rlim.rlim_max);

    if (setrlimit64(resource, &rlim) != 0) {
      rpcs3_android.error("failed to set rlimit for %d", resource);
      return;
    }
  };

  set_rlim(RLIMIT_MEMLOCK, 0x80000000);
  set_rlim(RLIMIT_NOFILE, 0x10000);
  set_rlim(RLIMIT_STACK, 128 * 1024 * 1024);

  virtual_pad_handler::set_on_connect_cb(initVirtualPad);
  setupCallbacks();
  Emu.SetHasGui(false);
  Emu.Init();

  g_cfg_input.player1.handler.set(pad_handler::virtual_pad);
  g_cfg_input.player1.device.from_string("Virtual");

  g_cfg_input.save("", g_cfg_input_configs.default_config);

  // g_cfg_vfs.dev_hdd0.to_string().ends_with("/")
  g_cfg.video.resolution.set(video_resolution::_720p);
  g_cfg.video.renderer.set(video_renderer::vulkan);
  g_cfg.video.shadermode.set(shader_mode::async_recompiler);
  g_cfg.core.ppu_decoder.set(ppu_decoder_type::llvm);
  g_cfg.core.spu_decoder.set(spu_decoder_type::llvm);
  g_cfg.core.llvm_cpu.from_string("cortex-a34");
  g_cfg.net.net_active.set(np_internet_status::enabled);
  // g_cfg.core.llvm_cpu.from_string(fallback_cpu_detection());
  g_cfg.video.perf_overlay.perf_overlay_enabled.set(true);

  Emulator::SaveSettings(g_cfg.to_string(), Emu.GetTitleID());
  return true;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_net_rpcs3_RPCS3_processCompilationQueue(JNIEnv *env, jobject) {
  g_compilationQueue.process(env);
  return true;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_net_rpcs3_RPCS3_startMainThreadProcessor(JNIEnv *env, jobject) {
  g_mainThreadProcessor.process(env);
  return true;
}

extern "C" JNIEXPORT jboolean JNICALL Java_net_rpcs3_RPCS3_collectGameInfo(
    JNIEnv *env, jobject, jstring jrootDir, jlong progressId) {

  if (std::filesystem::is_regular_file(g_cfg_vfs.get_dev_flash() +
                                       "/vsh/module/vsh.self")) {
    sendVshBootable(env, progressId);
  }

  collectGameInfo(env, progressId, {unwrap(env, jrootDir)});
  return true;
}

extern "C" JNIEXPORT void JNICALL Java_net_rpcs3_RPCS3_shutdown(JNIEnv *env,
                                                                jobject) {
  Emu.Kill();
}

extern "C" JNIEXPORT jboolean JNICALL Java_net_rpcs3_RPCS3_boot(JNIEnv *env,
                                                                jobject,
                                                                jstring jpath) {
  Emu.SetForceBoot(true);
  auto path = unwrap(env, jpath);
  while (path.ends_with('/')) {
    path.pop_back();
  }
  Emu.BootGame(path, "", false, cfg_mode::global);
  return true;
}

extern "C" JNIEXPORT jboolean JNICALL Java_net_rpcs3_RPCS3_surfaceEvent(
    JNIEnv *env, jobject, jobject surface, jint event) {
  rpcs3_android.warning("surface event %p, %d", surface, event);

  if (event == 2) {
    auto prevWindow = g_native_window.exchange(nullptr);
    if (prevWindow != nullptr) {
      ANativeWindow_release(prevWindow);
    }

    Emu.Pause();
  } else {
    auto newWindow = ANativeWindow_fromSurface(env, surface);

    if (newWindow == nullptr) {
      rpcs3_android.fatal("returned native window is null, surface %p",
                          surface);
      return false;
    }

    auto prevWindow = g_native_window.exchange(newWindow);

    if (newWindow != prevWindow) {
      ANativeWindow_acquire(newWindow);

      if (prevWindow != nullptr) {
        ANativeWindow_release(prevWindow);
      }
    }

    if (event == 0 && Emu.IsPaused()) {
      Emu.Resume();
    }
  }

  return true;
}

extern "C" JNIEXPORT jboolean JNICALL Java_net_rpcs3_RPCS3_usbDeviceEvent(
    JNIEnv *env, jobject, jint fd, jint vendorId, jint productId, jint event) {
  rpcs3_android.warning(
      "usb device event %d fd: %d, vendorId: %d, productId: %d", event, fd,
      vendorId, productId);

  {
    std::lock_guard lock(g_android_usb_devices_mutex);

    if (event == 0) {
      g_android_usb_devices.push_back({
          .fd = int(fd),
          .vendorId = u16(vendorId),
          .productId = u16(productId),
      });
    } else {
      auto filter = [fd](auto device) { return device.fd == fd; };
      if (auto it = std::ranges::find_if(g_android_usb_devices, filter);
          it != g_android_usb_devices.end()) {
        g_android_usb_devices.erase(it);
      }
    }
  }

  {
    auto selectedHandler = g_cfg_input.player1.handler.get();
    std::string selectedDevice;

    std::map<pad_handler, std::pair<std::unique_ptr<PadHandlerBase>,
                                    std::vector<std::string>>>
        handlerToDevices;

    auto collectDevices = [&]<typename T>(T handler) {
      handler->Init();

      std::vector<std::string> devices;
      for (const auto &device : handler->list_connected_devices()) {
        devices.push_back(device.name);
      }

      auto type = handler->m_type;

      handlerToDevices[type] = std::pair{
          std::move(handler),
          std::move(devices),
      };
    };

    collectDevices(std::make_unique<dualsense_pad_handler>());
    collectDevices(std::make_unique<ds4_pad_handler>());
    collectDevices(std::make_unique<ds3_pad_handler>());

    if (handlerToDevices[selectedHandler].second.empty()) {
      selectedHandler = pad_handler::null;
    }

    if (!handlerToDevices[pad_handler::dualsense].second.empty()) {
      selectedHandler = pad_handler::dualsense;
    } else if (!handlerToDevices[pad_handler::ds4].second.empty()) {
      selectedHandler = pad_handler::ds4;
    } else if (!handlerToDevices[pad_handler::ds3].second.empty()) {
      selectedHandler = pad_handler::ds3;
    }

    if (selectedHandler == pad_handler::null) {
      selectedHandler = pad_handler::virtual_pad;
    }

    if (selectedHandler != g_cfg_input.player1.handler.get()) {
      rpcs3_android.warning("install %s pad handler", selectedHandler);

      g_cfg_input.player1.handler.set(selectedHandler);

      if (selectedHandler == pad_handler::null) {
        g_cfg_input.player1.device.from_default();
      } else if (selectedHandler == pad_handler::virtual_pad) {
        g_cfg_input.player1.handler.set(pad_handler::virtual_pad);
        g_cfg_input.player1.device.from_string("Virtual");
      } else {
        g_cfg_input.player1.device.from_string(
            handlerToDevices[selectedHandler].second.front());
        handlerToDevices[selectedHandler].first->init_config(
            &g_cfg_input.player1.config);
        if (selectedHandler != pad_handler::virtual_pad) {
          std::lock_guard lock(g_virtual_pad_mutex);
          g_virtual_pad = nullptr;
        }
      }

      g_cfg_input.save("", g_cfg_input_configs.default_config);

      if (!Emu.IsStopped()) {
        pad::reset(Emu.GetTitleID());
      }
    }
  }

  return true;
}

static bool installPup(JNIEnv *env, fs::file &&pup_f, jlong progressId) {
  Progress progress(env, progressId);

  pup_object pup(std::move(pup_f));
  AtExit atExit{[&] { pup.file().release_handle(); }};

  if (static_cast<pup_error>(pup) == pup_error::hash_mismatch) {
    rpcs3_android.fatal("installFw: invalid PUP");
    progress.failure("Selected file is not firmware update file");
    return false;
  }

  if (static_cast<pup_error>(pup) != pup_error::ok) {
    rpcs3_android.fatal("installFw: invalid PUP");
    progress.failure("Firmware update file is broken");
    return false;
  }

  fs::file update_files_f = pup.get_file(0x300);

  const usz update_files_size = update_files_f ? update_files_f.size() : 0;

  if (!update_files_size) {
    rpcs3_android.fatal("installFw: invalid PUP");
    progress.failure("Firmware update file is broken");
    return false;
  }

  tar_object update_files(update_files_f);

  auto update_filenames = update_files.get_filenames();
  update_filenames.erase(std::remove_if(update_filenames.begin(),
                                        update_filenames.end(),
                                        [](const std::string &s) {
                                          return !s.starts_with("dev_flash_");
                                        }),
                         update_filenames.end());

  if (update_filenames.empty()) {
    rpcs3_android.fatal("installFw: invalid PUP");
    progress.failure("Firmware update file is broken");
    return false;
  }

  std::string version_string;

  if (fs::file version = pup.get_file(0x100)) {
    version_string = version.to_string();
  }

  if (const usz version_pos = version_string.find('\n');
      version_pos != std::string::npos) {
    version_string.erase(version_pos);
  }

  if (version_string.empty()) {
    rpcs3_android.fatal("installFw: invalid PUP");
    progress.failure("Firmware update file is broken");
    return false;
  }

  sendVshBootable(env, progressId);

  jlong processed = 0;
  for (const auto &update_filename : update_filenames) {
    auto update_file_stream = update_files.get_file(update_filename);

    if (update_file_stream->m_file_handler) {
      // Forcefully read all the data
      update_file_stream->m_file_handler->handle_file_op(
          *update_file_stream, 0, update_file_stream->get_size(umax), nullptr);
    }

    fs::file update_file = fs::make_stream(std::move(update_file_stream->data));

    SCEDecrypter self_dec(update_file);
    self_dec.LoadHeaders();
    self_dec.LoadMetadata(SCEPKG_ERK, SCEPKG_RIV);
    self_dec.DecryptData();

    auto dev_flash_tar_f = self_dec.MakeFile();

    if (dev_flash_tar_f.size() < 3) {
      rpcs3_android.error(
          "Firmware installation failed: Firmware could not be decompressed");

      progress.failure("Firmware update file could not be decompressed");
      return false;
    }

    tar_object dev_flash_tar(dev_flash_tar_f[2]);

    if (!dev_flash_tar.extract()) {

      rpcs3_android.error("Error while installing firmware: TAR contents are "
                          "invalid. (package=%s)",
                          update_filename);

      progress.failure(fmt::format("TAR contents are invalid (package=%s)",
                                   update_filename));
      return false;
    }

    if (!progress.report(processed++, update_filenames.size())) {
      // Installation was cancelled
      return false;
    }
  }

  sendFirmwareInstalled(env, utils::get_firmware_version());

  g_compilationQueue.push(progress,
                          g_cfg_vfs.get_dev_flash() + "/vsh/module/vsh.self");
  return true;
}

static bool installPkg(JNIEnv *env, fs::file &&file, jlong progressId) {
  Progress progress(env, progressId);

  std::deque<package_reader> readers;
  std::deque<std::string> bootable_paths;
  readers.emplace_back("dummy.pkg", std::move(file));

  AtExit atExit{[&] {
    for (auto &reader : readers) {
      reader.file().release_handle();
    }
  }};

  package_install_result result = {};
  named_thread worker("PKG Installer", [&readers, &result, &bootable_paths] {
    result = package_reader::extract_data(readers, bootable_paths);
    return result.error == package_install_result::error_type::no_error;
  });

  for (auto &reader : readers) {
    if (auto gameInfo = fetchGameInfo(reader.get_psf())) {
      sendGameInfo(env, progressId, {{*gameInfo}});
    }
  }

  const jlong maxProgress = 10000;

  while (true) {
    std::uint64_t totalProgress = 0;
    for (std::size_t index = 0; auto &reader : readers) {
      if (result.error != package_install_result::error_type::no_error) {
        progress.failure("Installation failed");
        for (package_reader &reader : readers) {
          reader.abort_extract();
        }
        return false;
      }

      totalProgress += reader.get_progress(maxProgress);
    }

    if (totalProgress == maxProgress * readers.size()) {
      break;
    }

    totalProgress /= readers.size();

    if (!progress.report(totalProgress, maxProgress)) {
      for (package_reader &reader : readers) {
        reader.abort_extract();
      }

      return false;
    }

    std::this_thread::sleep_for(std::chrono::seconds(2));
  }

  if (worker()) {
    auto paths = std::vector(bootable_paths.begin(), bootable_paths.end());
    collectGameInfo(env, -1, paths);

    for (auto &path : paths) {
      g_compilationQueue.push(progress, std::move(path));
    }
  }

  return true;
}

static bool installEdat(JNIEnv *env, fs::file &&file, jlong progressId,
                        std::string rootPath = {}) {
  Progress progress(env, progressId);

  NPD_HEADER npdHeader;
  if (!file.read(npdHeader)) {
    progress.failure("Invalid EDAT file");
    return false;
  }

  if (!rootPath.empty()) {
    auto ebootPath = locateEbootPath(rootPath);
    auto sfoPath = std::filesystem::path(rootPath) / "PARAM.SFO";

    if (!std::filesystem::is_regular_file(sfoPath)) {
      progress.failure("Game is broken: PARAM.SFO not found");
      return false;
    }

    auto psf = psf::load_object(sfoPath);
    auto contentId = psf::get_string(psf, "CONTENT_ID");

    if (contentId != npdHeader.content_id) {
      progress.failure(fmt::format("File cannot be used for this game. EDAT "
                                   "content ID missmatch %s vs %s",
                                   contentId, npdHeader.content_id));
      return false;
    }
  }

  const auto licenseFile =
      fmt::format("%shome/%s/exdata/%s.edat", rpcs3::utils::get_hdd0_dir(),
                  Emu.GetUsr(), npdHeader.content_id);

  file.seek(0);

  std::vector<std::uint8_t> bytes(file.size());
  if (!file.read(bytes)) {
    progress.failure("Failed to read key");
    return false;
  }

  if (!fs::write_file(licenseFile, fs::open_mode::create + fs::open_mode::trunc,
                      bytes)) {
    progress.failure(fmt::format("Failed to write EDAT to %s", licenseFile));
    return false;
  }

  if (rootPath.empty()) {
    rootPath = rpcs3::utils::get_hdd0_dir() + "game";
  }

  collectGameInfo(env, progressId, {rootPath});
  return true;
}

static bool installRap(JNIEnv *env, fs::file &&file, jlong progressId,
                       const std::string &rootPath) {
  Progress progress(env, progressId);

  auto ebootPath = locateEbootPath(rootPath);

  std::vector<std::uint8_t> bytes;
  if (!file.read(bytes, 16)) {
    progress.failure("Failed to read key");
    return false;
  }

  SelfAdditionalInfo info;
  decrypt_self(fs::file(ebootPath), nullptr, &info);

  auto npd = [&]() -> NPD_HEADER * {
    for (auto &supplemental : info.supplemental_hdr) {
      if (supplemental.type == 3) {
        return &supplemental.PS3_npdrm_header.npd;
      }
    }

    return nullptr;
  }();

  if (npd == nullptr) {
    progress.failure("Failed to fetch NPDRM of SELF");
    return false;
  }

  const auto licenseFile =
      fmt::format("%shome/%s/exdata/%s.rap", rpcs3::utils::get_hdd0_dir(),
                  Emu.GetUsr(), npd->content_id);

  if (!fs::write_file(licenseFile, fs::open_mode::create + fs::open_mode::trunc,
                      bytes)) {
    progress.failure(fmt::format("Failed to write key to %s", licenseFile));
    return false;
  }

  if (!decrypt_self(fs::file(ebootPath))) {
    progress.failure("Provided key is invalid for selected game");
    fs::remove_file(licenseFile);
    return false;
  }

  collectGameInfo(env, -1, {rootPath});
  g_compilationQueue.push(progress, std::move(ebootPath));
  return true;
}

static bool installIso(JNIEnv *env, fs::file &&file, jlong progressId) {
  auto optIso = iso_fs::open(std::make_unique<file_view_block_dev>(file));
  Progress progress(env, progressId);

  if (!optIso) {
    progress.failure("Failed to read ISO");
    return false;
  }

  auto iso = std::move(*optIso);
  auto sfo_file = iso.open("PS3_GAME/PARAM.SFO");

  if (!sfo_file) {
    progress.failure("Failed to locate PARAM.SFO in ISO");
    return false;
  }

  auto sfo = psf::load_object(sfo_file, "iso://PS3_GAME/PARAM.SFO");
  auto title_id = psf::get_string(sfo, "TITLE_ID");

  if (title_id.empty()) {
    progress.failure("Failed to fetch TITLE_ID from PARAM.SFO in ISO");
    return false;
  }

  if (auto gameInfo = fetchGameInfo(sfo)) {
    sendGameInfo(env, progressId, {{*gameInfo}});
  }

  std::filesystem::path destinationPath =
      fs::get_config_dir() + "games/" + std::string(title_id);
  std::size_t filesCount = 0;

  auto roots = [&] {
    std::vector<std::filesystem::path> result;
    std::vector<std::filesystem::path> workList;
    workList.push_back({});
    result.push_back({});

    while (!workList.empty()) {
      auto path = std::move(workList.back());
      workList.pop_back();

      for (auto entry : iso.open_dir(path)) {
        if (entry.name == "." || entry.name == "..") {
          continue;
        }
        if (entry.name == "PS3_UPDATE" && path.empty()) {
          continue;
        }

        if (entry.is_directory) {
          result.push_back(path / entry.name);
          workList.push_back(path / entry.name);
        } else {
          filesCount++;
        }
      }
    }

    return result;
  }();

  progress.report(0, filesCount);

  std::size_t processedFiles = 0;
  std::error_code ec;

  for (auto &root : roots) {
    auto rootDestPath = root.empty() ? destinationPath : destinationPath / root;

    std::filesystem::create_directories(rootDestPath, ec);
    if (ec) {
      progress.failure(fmt::format("Failed to create dir %s: %s",
                                   rootDestPath.string(), ec.message()));
      return false;
    }

    for (auto entry : iso.open_dir(root)) {
      if (entry.name == "." || entry.name == "..") {
        continue;
      }

      auto entryDestPath = rootDestPath / entry.name;

      if (entry.is_directory) {
        std::filesystem::create_directories(entryDestPath, ec);
        if (ec) {
          progress.failure(fmt::format("Failed to create dir %s: %s",
                                       entryDestPath.string(), ec.message()));
          return false;
        }

        continue;
      }
      auto file = iso.open(root / entry.name);

      if (!file) {
        progress.failure(fmt::format("Failed to open file in ISO: %s",
                                     (root / entry.name).string()));
        return false;
      }

      if (!fs::write_file(entryDestPath,
                          fs::open_mode::create + fs::open_mode::trunc,
                          file.to_vector<std::uint8_t>())) {
        progress.failure(fmt::format("Failed to write file: %s, dest %s",
                                     entryDestPath.string(),
                                     destinationPath.string()));
        return false;
      }

      progress.report(processedFiles++, filesCount);
    }
  }

  collectGameInfo(env, -1, {destinationPath});
  auto ebootPath = locateEbootPath(destinationPath);
  g_compilationQueue.push(progress, std::move(ebootPath));
  return true;
}

extern "C" JNIEXPORT jboolean JNICALL Java_net_rpcs3_RPCS3_installFw(
    JNIEnv *env, jobject, jint fd, jlong progressId) {
  return installPup(env, fs::file::from_native_handle(fd), progressId);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_net_rpcs3_RPCS3_install(JNIEnv *env, jobject, jint fd, jlong progressId) {
  auto file = fs::file::from_native_handle(fd);
  AtExit atExit{[&] { file.release_handle(); }};

  auto type = getFileType(file);
  file.seek(0);

  switch (type) {
  case FileType::Unknown:
    Progress(env, progressId).failure("Unsupported file type");
    return false;

  case FileType::Pup:
    return installPup(env, std::move(file), progressId);

  case FileType::Pkg:
    return installPkg(env, std::move(file), progressId);

  case FileType::Edat:
    return installEdat(env, std::move(file), progressId);

  case FileType::Iso:
    return installIso(env, std::move(file), progressId);

  case FileType::Rap:
    Progress(env, progressId)
        .failure("RAP file cannot be preinstalled. Use lock button on "
                 "installed game instead");
    return false;
  }

  return true;
}

extern "C" JNIEXPORT jboolean JNICALL Java_net_rpcs3_RPCS3_installKey(
    JNIEnv *env, jobject, jint fd, jlong progressId, jstring gamePath) {
  auto file = fs::file::from_native_handle(fd);
  AtExit atExit{[&] { file.release_handle(); }};

  auto type = getFileType(file);
  file.seek(0);

  if (type == FileType::Rap) {
    return installRap(env, std::move(file), progressId, unwrap(env, gamePath));
  }

  if (type == FileType::Edat) {
    return installEdat(env, std::move(file), progressId, unwrap(env, gamePath));
  }

  Progress(env, progressId).failure("Unsupported key type");
  return false;
}

extern "C" JNIEXPORT jstring JNICALL
Java_net_rpcs3_RPCS3_systemInfo(JNIEnv *env, jobject) {
  std::string result;

  result += utils::get_system_info();
  result += "\n";
  result += "LLVM CPU: ";
  result += fallback_cpu_detection();
  result += "\n";

  {
    vk::instance device_enum_context;
    if (device_enum_context.create("RPCS3", true)) {
      device_enum_context.bind();
      const std::vector<vk::physical_device> &gpus =
          device_enum_context.enumerate_devices();

      for (const auto &gpu : gpus) {
        result += "GPU: ";
        result += gpu.get_name();
        result += "\n";
      }
    }
  }

  return wrap(env, result);
}

static cfg::_base *find_cfg_node(cfg::_base *root, std::string_view path) {
  auto pathList = fmt::split(path, {"@@"});
  std::ranges::reverse(pathList);

  while (!pathList.empty()) {
    auto elem = pathList.back();
    pathList.pop_back();
    if (elem.empty()) {
      continue;
    }

    auto root_node = dynamic_cast<cfg::node *>(root);
    if (root_node == nullptr) {
      return nullptr;
    }

    cfg::_base *child_node = nullptr;

    for (auto node : root_node->get_nodes()) {
      if (node->get_name() == elem) {
        child_node = node;
        break;
      }
    }

    if (child_node == nullptr) {
      return nullptr;
    }

    root = child_node;
  }

  return root;
}

extern "C" JNIEXPORT jstring JNICALL
Java_net_rpcs3_RPCS3_settingsGet(JNIEnv *env, jobject, jstring jpath) {
  auto root = find_cfg_node(&g_cfg, unwrap(env, jpath));

  if (root == nullptr) {
    return nullptr;
  }

  return wrap(env, root->to_json().dump(4));
}

extern "C" JNIEXPORT jboolean JNICALL Java_net_rpcs3_RPCS3_settingsSet(
    JNIEnv *env, jobject, jstring jpath, jstring jvalue) {
  nlohmann::json value;
  try {
    value = nlohmann::json::parse(unwrap(env, jvalue));
  } catch (...) {
    rpcs3_android.error("settingsSet: node %s passed with invalid json '%s'", unwrap(env, jpath), unwrap(env, jvalue));
    return false;
  }

  auto root = find_cfg_node(&g_cfg, unwrap(env, jpath));

  if (root == nullptr) {
    rpcs3_android.error("settingsSet: node %s not found", unwrap(env, jpath));
    return false;
  }

  if (root->from_json(value, !Emu.IsStopped())) {
    rpcs3_android.error("settingsSet: node %s not accepts value '%s'", unwrap(env, jpath), value.dump());
    Emulator::SaveSettings(g_cfg.to_string(), "");
    return true;
  }

  return false;
}

extern "C" JNIEXPORT jboolean JNICALL Java_net_rpcs3_utils_GpuDriverHelper_supportsCustomDriverLoading(JNIEnv *env, jobject instance) {
  return access("/dev/kgsl-3d0", F_OK) == 0;
}

// extern "C" JNIEXPORT void JNICALL Java_net_rpcs3_utils_GpuDriverHelper_forceMaxGpuClocks(JNIEnv *env, jobject instance, jboolean enable) {
//     adrenotools_set_turbo(enable);
// }
