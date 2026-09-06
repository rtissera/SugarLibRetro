#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

// NOTE: this file used to #define NO_MULTITHREAD/NOFILTER/NOZLIB/NO_RAW_FORMAT
// here to build a leaner CPCCoreEmu for libretro. CPCCoreEmu's own CMakeLists
// never applies those as compile definitions to the library itself, though --
// they only affected this translation unit's view of CPCCoreEmu's headers
// (e.g. Tape.h/DiskContainer.h gate real members behind `#ifndef NOZLIB`).
// That's an ODR violation: this file computed a different (smaller) sizeof()
// for classes like Motherboard/EmulatorEngine than the actually-linked
// CPCCoreEmu.a was built with, so `new EmulatorEngine()` undersized its
// allocation -- a heap-buffer-overflow confirmed live under ASan
// (BreakpointHandler::BreakpointHandler() writing past the end of an
// undersized EmulatorEngine, manifesting downstream as "double free or
// corruption" at retro_deinit). Do not redefine these without also making
// CPCCoreEmu's CMakeLists pass them to the library build.

#include "Machine.h"
#include "Cartridge.h"
#include "IDirectories.h"
#include "Inotify.h"
#include "libretro.h"
#include <string>
#include <vector>

//#define WIDTH  768
#define WIDTH  640
//#define HEIGHT 277
#define HEIGHT 480

//#define OFFSET_X 143
#define OFFSET_X 207
//#define OFFSET_Y 47
#define OFFSET_Y 84


#define M_PI    3.14159265358979323846264338327950288   /* pi */

static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_environment_t environ_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

// Display
class RetroDisplay : public IDisplay
{
public:
   RetroDisplay()
   {
      // Init
      video_buffer = new int[1024 * 1024];
      memset(video_buffer, 0, 1024 * 1024 * sizeof(int));
      pitch_ = 1024 * sizeof(unsigned int);
   };
   virtual ~RetroDisplay() {};

   virtual void SetScanlines(int scan) {};
   virtual void Display() {};
   virtual bool AFrameIsReady() { return true; };
   virtual void Config() {};
   virtual const char* GetInformations() { return "Libretro GDI"; };
   virtual int GetWidth() { return WIDTH; };
   virtual int GetHeight() { return HEIGHT; };
   virtual void VSync(bool bDbg)
   {
      video_cb(&video_buffer[OFFSET_X  + 1024*OFFSET_Y], WIDTH, HEIGHT, pitch_);
   }
   virtual void StartSync(){};
   virtual void WaitVbl() {};
   virtual void SyncOnFrame(bool set) {};
   virtual int* GetVideoBuffer(int y)
   {
      return &video_buffer[1024 * y*2];
   }
   virtual void Reset() {};
   virtual void Screenshot(const char* scr_path) {};
   virtual void ScreenshotEveryFrame(int bSetOn) {};
   virtual bool IsEveryFrameScreened() {
      return false;
   }
   virtual bool IsDisplayed() { return true;/* m_bShow;*/ };
   virtual void FullScreenToggle() {};
   virtual void ForceFullScreen(bool bSetFullScreen) {}
   virtual void WindowChanged(int xIn, int yIn, int wndWidth, int wndHeight) {};
   virtual bool SetSyncWithVbl(int speed) { return false; };
   virtual bool IsWaitHandled() { return false; };
   virtual bool GetBlackScreenInterval() { return false; };
   virtual void SetBlackScreenInterval(bool bBS) { };

   void Init();
   void Show(bool bShow);

   virtual void SetSize(SizeEnum size) {};
   virtual SizeEnum  GetSize() { return S_STANDARD; };

   virtual void ResetLoadingMedia() {};
   virtual void SetLoadingMedia() {};

   virtual void ResetDragnDropDisplay() {};
   virtual void SetDragnDropDisplay(int type) {};
   virtual void SetCurrentPart(int x, int y) {};
   virtual int GetDnDPart() { return 0; };

protected:
   int * video_buffer;
   unsigned int pitch_;
};

// Real input wiring.
//
// EmulatorEngine::GetKeyboardHandler() returns CPCCoreEmu's OWN native
// KeyboardHandler (Machine.cpp: Motherboard's constructor is hardwired to
// &keyboardhandler_, its internal member -- there is no way to inject a
// different IKeyboardHandler). A prior "Keyboard : IKeyboardHandler" class
// used to live here implementing that interface directly, ported forward
// from Thomas's 2020 pre-EmulatorEngine prototype where the frontend built
// its own Motherboard and could pass in a custom handler -- but nothing
// ever constructs a Motherboard with it any more, so every gamepad_button_*
// write into it went nowhere. Removed; real input goes through
// KeyboardHandler::ForceKeyboardState(unsigned char[10]), which writes
// directly into the live matrix the AY-3-8912 PSG polls each frame
// (PSG.cpp: keyboard_handler_->GetKeyboardMap(line) on register 14 reads).
//
// CPC keyboard/joystick matrix (10 lines x 8 bits, bit=0 means pressed) --
// row/bit assignments from CPCCoreEmu/Keyboards/101_keyboard_linux's own
// comments, the canonical Amstrad CPC hardware matrix:
//   line 0: CurUp CurRight CurDown F9 F6 F3 EnterNumpad F.Numpad
//   line 1: CurLeft Copy F7 F8 F5 F1 F2 F0Numpad
//   line 2: Clr [{ Return ]} F4 Shift `\ Ctrl
//   line 3: ^(caret) =- @| P +; *: ?/ >,
//   line 4: _0 )9 O I L K M <.
//   line 5: (8 '7 U Y H J N Space
//   line 6: &6 %5 R T G F B V
//   line 7: $4 #3 E W S D C X
//   line 8: !1 "2 Esc Q Tab A CapsLock Z
//   line 9: Joy0Up Joy0Down Joy0Left Joy0Right Joy0Fire1 Joy0Fire2 unused Del
struct KeyMapEntry { unsigned retrok; int line; int bit; };
static const KeyMapEntry kKeyMap[] = {
   { RETROK_UP,          0, 0 }, { RETROK_RIGHT,   0, 1 }, { RETROK_DOWN,   0, 2 },
   { RETROK_F9,          0, 3 }, { RETROK_F6,      0, 4 }, { RETROK_F3,     0, 5 },
   { RETROK_KP_ENTER,    0, 6 }, { RETROK_KP_PERIOD, 0, 7 },
   { RETROK_LEFT,        1, 0 }, { RETROK_INSERT,  1, 1 }, { RETROK_F7,     1, 2 },
   { RETROK_F8,          1, 3 }, { RETROK_F5,      1, 4 }, { RETROK_F1,     1, 5 },
   { RETROK_F2,          1, 6 }, { RETROK_KP0,     1, 7 },
   { RETROK_HOME,        2, 0 }, { RETROK_LEFTBRACKET, 2, 1 }, { RETROK_RETURN, 2, 2 },
   { RETROK_RIGHTBRACKET, 2, 3 }, { RETROK_F4,     2, 4 },
   { RETROK_LSHIFT,      2, 5 }, { RETROK_RSHIFT,  2, 5 },
   { RETROK_BACKQUOTE,   2, 6 }, { RETROK_BACKSLASH, 2, 6 },
   { RETROK_LCTRL,       2, 7 }, { RETROK_RCTRL,   2, 7 },
   { RETROK_CARET,       3, 0 }, { RETROK_MINUS,   3, 1 }, { RETROK_EQUALS, 3, 1 },
   { RETROK_AT,          3, 2 }, { RETROK_p,       3, 3 },
   { RETROK_SEMICOLON,   3, 4 }, { RETROK_COLON,   3, 5 },
   { RETROK_SLASH,       3, 6 }, { RETROK_COMMA,   3, 7 },
   { RETROK_0,           4, 0 }, { RETROK_9,       4, 1 }, { RETROK_o,      4, 2 },
   { RETROK_i,           4, 3 }, { RETROK_l,       4, 4 }, { RETROK_k,      4, 5 },
   { RETROK_m,           4, 6 }, { RETROK_PERIOD,  4, 7 },
   // Row 3 bit 5 is the "*:" key (colon/asterisk), NOT quote -- the
   // apostrophe/quote key is row 5 bit 1 ("'7", shift+7 on a real CPC).
   // This was wrong in the original table (mapped RETROK_QUOTE to 3,5).
   { RETROK_8,           5, 0 }, { RETROK_7,       5, 1 }, { RETROK_QUOTE, 5, 1 },
   { RETROK_u,           5, 2 },
   { RETROK_y,           5, 3 }, { RETROK_h,       5, 4 }, { RETROK_j,      5, 5 },
   { RETROK_n,           5, 6 }, { RETROK_SPACE,   5, 7 },
   { RETROK_6,           6, 0 }, { RETROK_5,       6, 1 }, { RETROK_r,      6, 2 },
   { RETROK_t,           6, 3 }, { RETROK_g,       6, 4 }, { RETROK_f,      6, 5 },
   { RETROK_b,           6, 6 }, { RETROK_v,       6, 7 },
   { RETROK_4,           7, 0 }, { RETROK_3,       7, 1 }, { RETROK_e,      7, 2 },
   { RETROK_w,           7, 3 }, { RETROK_s,       7, 4 }, { RETROK_d,      7, 5 },
   { RETROK_c,           7, 6 }, { RETROK_x,       7, 7 },
   { RETROK_1,           8, 0 }, { RETROK_2,       8, 1 }, { RETROK_ESCAPE, 8, 2 },
   { RETROK_q,           8, 3 }, { RETROK_TAB,     8, 4 }, { RETROK_a,      8, 5 },
   { RETROK_CAPSLOCK,    8, 6 }, { RETROK_z,       8, 7 },
   { RETROK_BACKSPACE,   9, 7 }, { RETROK_DELETE,  9, 7 },
};

// Autorun: types RUN" + Enter once, shortly after a fresh disk/tape load.
// The CPC needs this to launch anything that isn't a .cpr cartridge -- real
// hardware behavior (AMSDOS's RUN" with no filename loads/runs the first
// program file on the disc), not something specific to this core.
//
// This does NOT use EmulatorEngine::Paste()/CharPressed() -- that path
// resolves typed characters against KeyboardHandler::keyboard_map_, which
// is only ever populated from CONF/KeyboardMaps.ini via
// ConfigurationManager, and that path was deliberately stubbed to return
// sentinel defaults (see ConfigurationManager::GetConfiguration* above) to
// silence log spam from ~600 undeclared per-key lookups -- so
// keyboard_map_ has no real char associations and Paste() would silently
// do nothing. Instead this drives the same matrix update_input() already
// writes each frame via ForceKeyboardState(), which is proven working.
struct AutorunKey { char c; int line; int bit; bool shift; };
static const AutorunKey kAutorunKeys[] = {
   { 'R', 6, 2, false }, { 'U', 5, 2, false }, { 'N', 5, 6, false },
   { '"', 8, 1, true  }, { '\r', 2, 2, false },
};
static const char kAutorunSequence[] = "RUN\"\r";
enum AutorunState { AUTORUN_IDLE, AUTORUN_WAITING, AUTORUN_PRESS, AUTORUN_RELEASE, AUTORUN_DONE };
static AutorunState autorun_state_ = AUTORUN_IDLE;
static int autorun_timer_ = 0;
static unsigned autorun_char_index_ = 0;

// ~2s at 50Hz before typing (mirrors CPCCoreEmu's own Paste() gate, which
// waits for 2000ms of emulated time before considering the machine ready),
// ~150ms per press and per release -- generous for the CPC's keyboard scan
// rate, avoids a dropped keystroke.
static void ArmAutorun() { autorun_state_ = AUTORUN_WAITING; autorun_timer_ = 100; autorun_char_index_ = 0; }

static void TickAutorun(unsigned char matrix[10])
{
   switch (autorun_state_)
   {
   case AUTORUN_IDLE:
   case AUTORUN_DONE:
      return;
   case AUTORUN_WAITING:
      if (--autorun_timer_ <= 0)
      {
         autorun_state_ = AUTORUN_PRESS;
         autorun_timer_ = 8;
      }
      return;
   case AUTORUN_PRESS:
   case AUTORUN_RELEASE:
      break;
   }

   const char c = kAutorunSequence[autorun_char_index_];
   if (c == '\0')
   {
      autorun_state_ = AUTORUN_DONE;
      return;
   }

   const AutorunKey* key = nullptr;
   for (size_t i = 0; i < sizeof(kAutorunKeys) / sizeof(kAutorunKeys[0]); ++i)
   {
      if (kAutorunKeys[i].c == c) { key = &kAutorunKeys[i]; break; }
   }
   if (key == nullptr)
   {
      ++autorun_char_index_;
      return;
   }

   if (autorun_state_ == AUTORUN_PRESS)
   {
      matrix[key->line] &= ~(1 << key->bit);
      if (key->shift) matrix[2] &= ~(1 << 5);
      if (--autorun_timer_ <= 0) { autorun_state_ = AUTORUN_RELEASE; autorun_timer_ = 8; }
   }
   else // AUTORUN_RELEASE
   {
      if (--autorun_timer_ <= 0)
      {
         ++autorun_char_index_;
         autorun_state_ = AUTORUN_PRESS;
         autorun_timer_ = 8;
      }
   }
}

class ConfigurationManager : public IConfiguration
{
public:
   // libretro has no on-disk config file of its own -- settings come from
   // RETRO_ENVIRONMENT_GET_VARIABLE (core options). These are no-ops.
   virtual void OpenFile(const char* config_file) {}
   virtual void CloseFile() {}

   // Section/key enumeration is for iterating an INI structure we don't
   // maintain; empty enumeration is the correct "no sections" answer.
   virtual const char* GetFirstSection() { return nullptr; }
   virtual const char* GetNextSection() { return nullptr; }
   virtual const char* GetFirstKey(const char* section) { return nullptr; }
   virtual const char* GetNextKey() { return nullptr; }

   virtual void SetConfiguration(const char* section, const char* cle, const char* valeur)
   {
      SetConfiguration(section, cle, valeur, nullptr);
   }
   virtual unsigned int GetConfiguration(const char* section, const char* cle, const char* default_value, char* out_buffer, unsigned int buffer_size)
   {
      return GetConfiguration(section, cle, default_value, out_buffer, buffer_size, nullptr);
   }
   virtual unsigned int GetConfigurationInt(const char* section, const char* cle, unsigned int default_value)
   {
      return GetConfigurationInt(section, cle, default_value, nullptr);
   }

   virtual void SetConfiguration(const char* section, const char* cle, const char* valeur, const char* file)
   {
      struct retro_variable var;
      std::string key;
      key = section + std::string("_") + std::string(cle);
      var.key = key.c_str();
      var.value = valeur;
      environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, &var);
   }
   virtual unsigned int GetConfiguration(const char* section, const char* cle, const char* default_value, char* out_buffer, unsigned int buffer_size, const char* file)
   {
      // file != nullptr means the caller wants a real on-disk INI (this is
      // how KeyboardHandler::GetKeyValues() reads CONF/KeyboardMaps.ini for
      // the CharPressed/CharReleased "paste text" feature) -- we don't have
      // an INI parser wired up for that, and routing it through
      // RETRO_ENVIRONMENT_GET_VARIABLE instead floods the log with
      // "Invalid value" for every one of its ~600+ undeclared per-key
      // lookups. Real-time key input (ForceKeyboardState, see kKeyMap
      // above) doesn't go through this path at all, so this only affects
      // paste-to-type, which just falls back to its documented default.
      if (section == nullptr || cle == nullptr || file != nullptr)
      {
         strncpy(out_buffer, default_value, buffer_size);
         return true;
      }
      struct retro_variable var;
      std::string key;
      key = section + std::string("_") + std::string(cle);
      var.key = key.c_str();
      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      {
         strncpy(out_buffer, var.value, buffer_size);
         return true;
      }
      else
      {
         strncpy(out_buffer, default_value, buffer_size);
         return true;
      }
   }
   virtual unsigned int GetConfigurationInt(const char* section, const char* cle, unsigned int default_value, const char* file)
   {
      if (file != nullptr)
         return default_value;
      struct retro_variable var;
      std::string key;
      key = section + std::string("_") + std::string(cle);
      var.key = key.c_str();
      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      {
         return atoi(var.value);

      }
      else
      {
         return default_value;
      }
   }
};

// EmulatorEngine::LoadRom() joins this with "ROM/<filename>" to open lower/
// upper ROM files (see Machine.cpp's ROMPath). RetroArch's system directory
// is where REG-Linux configures BIOS-style assets (equivalent to
// /userdata/bios/amstradcpc/ on REG-Linux); a bundled default ROM set ships
// under SugarLibRetro/system/amstradcpc/ROM/ in this repo for that path, and
// the same layout lets a user drop their own dumps there to override it.
class RetroDirectories : public IDirectories
{
public:
   virtual const char* GetBaseDirectory()
   {
      if (base_directory_.empty())
      {
         const char* system_dir = nullptr;
         if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir) && system_dir)
            base_directory_ = std::string(system_dir) + "/amstradcpc";
         else
            base_directory_ = "amstradcpc";
      }
      return base_directory_.c_str();
   }
private:
   std::string base_directory_;
};



static struct retro_log_callback logging;
static retro_log_printf_t log_cb;
static bool use_audio_cb;
static float last_aspect;
static float last_sample_rate;

// FDC::LoadDisk() only reports success/failure through this notifier (see
// EmulatorEngine::LoadDisk's switch on the return code, which -- in the
// upstream Qt app -- feeds a message box; here it just logs so a failed
// load isn't silently indistinguishable from a working one in the core log).
class RetroFdcNotify : public IFdcNotify
{
public:
   virtual void ItemLoaded(const char* disk_path, int load_ok, int drive_number)
   {
      if (log_cb == nullptr)
         return;
      const char* what = (load_ok == 0) ? "OK" : (load_ok == -1) ? "file not found" : "unknown/unsupported format";
      log_cb(RETRO_LOG_INFO, "FDC: drive %d load '%s': %s (%d).\n", drive_number, disk_path, what, load_ok);
   }
   virtual void DiskEject() {}
   virtual void DiskRunning(bool on) {}
   virtual void TrackChanged(int nb_tracks) {}
};

// Definition of emulator
static EmulatorEngine* emulator_ = nullptr;
static Motherboard * motherboard_ = nullptr;
static ConfigurationManager conf_manager_;
static RetroDisplay display_;
static RetroDirectories directories_;
static MachineSettings machine_settings_;
static RetroFdcNotify fdc_notify_;

static void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
   (void)level;
   va_list va;
   va_start(va, fmt);
   vfprintf(stderr, fmt, va);
   va_end(va);
}

int LoadCprFromBuffer(unsigned char* buffer, int size)
{
   // Check RIFF chunk
   int index = 0;
   if (size >= 12
      && (memcmp(&buffer[0], "RIFF", 4) == 0)
      && (memcmp(&buffer[8], "AMS!", 4) == 0)
      )
   {
      // Reinit Cartridge
      motherboard_->EjectCartridge();

      // Ok, it's correct.
      index += 4;
      // Check the whole size

      int chunk_size = buffer[index]
         + (buffer[index + 1] << 8)
         + (buffer[index + 2] << 16)
         + (buffer[index + 3] << 24);

      index += 8;

      // 'fmt ' chunk ? skip it
      if (index + 8 < size && (memcmp(&buffer[index], "fmt ", 4) == 0))
      {
         index += 8;
      }

      // Good.
      // Now we are at the first cbxx
      while (index + 8 < size)
      {
         if (buffer[index] == 'c' && buffer[index + 1] == 'b')
         {
            index += 2;
            char buffer_block_number[3] = { 0 };
            memcpy(buffer_block_number, &buffer[index], 2);
            int block_number = (buffer_block_number[0] - '0') * 10 + (buffer_block_number[1] - '0');
            index += 2;

            // Read size
            int block_size = buffer[index]
               + (buffer[index + 1] << 8)
               + (buffer[index + 2] << 16)
               + (buffer[index + 3] << 24);
            index += 4;

            if (block_size <= size && block_number < 256)
            {
               // Copy datas to proper ROM
               unsigned char* rom = motherboard_->GetCartridge(block_number);
               memset(rom, 0, 0x1000);
               memcpy(rom, &buffer[index], block_size);
               index += block_size;
            }
            else
            {
               return -1;
            }
         }
         else
         {
            return -1;
         }
      }
   }
   else
   {
      // Incorrect headers
      return -1;
   }

   return 0;
}

static void ApplyMachineType(const char* model);

void retro_init(void)
{
   // Boots through the real EmulatorEngine/MachineSettings facade -- model
   // selection (464/664/6128/plus6128/gx4000, see ApplyMachineType) is a
   // config change, not a rewrite.
   emulator_ = new EmulatorEngine();
   emulator_->SetDirectories(&directories_);
   emulator_->SetConfigurationManager(&conf_manager_);
   emulator_->Init(&display_, nullptr);
   emulator_->SetNotifier(&fdc_notify_);

   struct retro_variable var = { "amstradcpc_model", nullptr };
   environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var);
   ApplyMachineType(var.value ? var.value : "6128");

   motherboard_ = emulator_->GetMotherboard();
   motherboard_->GetPSG()->InitSound(nullptr);
   emulator_->OnOff();
}

void retro_deinit(void)
{
   delete emulator_;
}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
   log_cb(RETRO_LOG_INFO, "Plugging device %u into port %u.\n", device, port);
}

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name = "Sugarbox";
   info->library_version = "v1.00";
   info->need_fullpath = true;
   // MediaManager/DskTypeManager auto-detects the real format from content
   // (magic bytes), not just the extension -- this list just tells the
   // frontend what to offer/accept as content for this core. cpr is the
   // Plus/GX4000 cartridge format (see LoadCprFromBuffer); the rest are disk
   // formats EmulatorEngine::LoadDisk() dispatches to CPCCoreEmu's own
   // FormatType* parsers for (FormatTypeDSK/EDSK/IPF/CTRAW/RAW/HFE/HFEv3/SCP).
   info->valid_extensions = "cpr|dsk|edsk|ipf|ctr|raw|hfe|scp|m3u|cdt|tap|tzx|csw|voc|wav";
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   float aspect = 4.0f / 3.0f;
   struct retro_variable var = { "sugarbox_aspect_ratio", nullptr };
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "4:3"))
         aspect = 4.0f / 3.0f;
      else if (!strcmp(var.value, "16:9"))
         aspect = 16.0f / 9.0f;
   }

   float sampling_rate = 44100.0f;
   var.key = "sugarbox_sample_rate";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      sampling_rate = strtof(var.value, NULL);

   //info->timing = (struct retro_system_timing);
   info->timing.fps = 50.0;
   info->timing.sample_rate = sampling_rate;
   
   //info->geometry = (struct retro_game_geometry) {
   info->geometry.base_width = WIDTH;
   info->geometry.base_height = HEIGHT;

   info->geometry.max_width = WIDTH;
   info->geometry.max_height = HEIGHT;
   info->geometry.aspect_ratio = aspect;

   last_aspect = aspect;
   last_sample_rate = sampling_rate;
}

static struct retro_rumble_interface rumble;

void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;

   static const struct retro_variable vars[] = {
      { "sugarbox_aspect_ratio", "Aspect Ratio; 4:3|16:9" },
      { "sugarbox_sample_rate", "Sample Rate; 44100|48000|30000|20000" },
      // Model switch takes effect immediately (no restart needed):
      // ApplyMachineType() is called both here at load and from
      // check_variables() on RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE.
      { "amstradcpc_model", "CPC Model; 6128|664|464|plus6128|gx4000" },
      // The CPC needs a typed RUN"/CAT to launch anything that isn't a
      // .cpr cartridge (real hardware behavior, not an emulator quirk) --
      // this automatically types RUN" + Enter once, ~2s after a fresh
      // disk/tape load, matching cap32's own cap32_autorun option.
      { "sugarbox_autorun", "Autorun disk/tape; enabled|disabled" },
      { NULL, NULL },
   };

   cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars);

   // The machine boots on its own in retro_init() (real EmulatorEngine/
   // MachineSettings bring-up, Phase 1) -- a cartridge/disk/tape is optional
   // content, not a requirement to run at all.
   bool no_content = true;
   cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_content);

   if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
      log_cb = logging.log;
   else
      log_cb = fallback_log;

   // Real description -- the previous "Dummy Controller #1/#2"/"Augmented
   // Joypad" entries were unedited libretro-common sample-core placeholder
   // text, visible to real users in RetroArch's port-config UI. Only one
   // joystick port is actually wired (row 9 of the CPC keyboard matrix,
   // see kKeyMap/update_input above), matching real CPC hardware (one
   // joystick port on the base machine).
   static const struct retro_controller_description controllers[] = {
      { "Amstrad CPC Joystick", RETRO_DEVICE_JOYPAD },
   };

   static const struct retro_controller_info ports[] = {
      { controllers, 1 },
      { NULL, 0 },
   };

   cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);



}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
   audio_cb = cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
   input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
   input_state_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}

static unsigned x_coord;
static unsigned y_coord;
static unsigned phase;
static int mouse_rel_x;
static int mouse_rel_y;

void retro_reset(void)
{
   x_coord = 0;
   y_coord = 0;
}

static void update_input(void)
{
   int dir_x = 0;
   int dir_y = 0;
   bool button_X = false;
   bool button_A = false;

   input_poll_cb();
   if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP))
      dir_y--;
   if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN))
      dir_y++;
   if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT))
      dir_x--;
   if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT))
      dir_x++;
   button_X = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X);
   button_A = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A);

   int16_t mouse_x = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
   int16_t mouse_y = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);
   bool mouse_l = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT);
   bool mouse_r = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT);
   bool mouse_down = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_WHEELDOWN);
   bool mouse_up = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_WHEELUP);
   bool mouse_middle = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_MIDDLE);
   if (mouse_x)
      log_cb(RETRO_LOG_INFO, "Mouse X: %d\n", mouse_x);
   if (mouse_y)
      log_cb(RETRO_LOG_INFO, "Mouse Y: %d\n", mouse_y);
   if (mouse_l)
      log_cb(RETRO_LOG_INFO, "Mouse L pressed.\n");
   if (mouse_r)
      log_cb(RETRO_LOG_INFO, "Mouse R pressed.\n");
   if (mouse_down)
      log_cb(RETRO_LOG_INFO, "Mouse wheeldown pressed.\n");
   if (mouse_up)
      log_cb(RETRO_LOG_INFO, "Mouse wheelup pressed.\n");
   if (mouse_middle)
      log_cb(RETRO_LOG_INFO, "Mouse middle pressed.\n");

   mouse_rel_x += mouse_x;
   mouse_rel_y += mouse_y;
   if (mouse_rel_x >= 310)
      mouse_rel_x = 309;
   else if (mouse_rel_x < 10)
      mouse_rel_x = 10;
   if (mouse_rel_y >= 230)
      mouse_rel_y = 229;
   else if (mouse_rel_y < 10)
      mouse_rel_y = 10;

   bool pointer_pressed = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED);
   int16_t pointer_x = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
   int16_t pointer_y = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);
   if (pointer_pressed)
      log_cb(RETRO_LOG_INFO, "Pointer: (%6d, %6d).\n", pointer_x, pointer_y);

   dir_x += input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X) / 5000;
   dir_y += input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y) / 5000;
   dir_x += input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X) / 5000;
   dir_y += input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y) / 5000;

   // dir_x>0/dir_y>0 mean RIGHT/DOWN were pressed (libretro convention,
   // matches the ++ / -- above) -- map to the matching CPC joystick bit,
   // not the opposite one (this was inverted on all 4 axes before).
   const bool joy_left = dir_x < 0;
   const bool joy_right = dir_x > 0;
   const bool joy_up = dir_y < 0;
   const bool joy_down = dir_y > 0;

   x_coord = (x_coord + dir_x) & 31;
   y_coord = (y_coord + dir_y) & 31;

   // Real input wiring: build the full CPC 10-line keyboard/joystick matrix
   // (bit=0 means pressed) and push it straight into CPCCoreEmu's own
   // KeyboardHandler -- see the KeyMapEntry comment above for why this is
   // the correct entry point (ForceKeyboardState, not a custom
   // IKeyboardHandler, since Motherboard is hardwired to its internal one).
   unsigned char matrix[10];
   memset(matrix, 0xFF, sizeof(matrix));

   // Joystick 0, row 9: up/down/left/right/fire1/fire2.
   if (joy_up)    matrix[9] &= ~0x01;
   if (joy_down)  matrix[9] &= ~0x02;
   if (joy_left)  matrix[9] &= ~0x04;
   if (joy_right) matrix[9] &= ~0x08;
   if (button_X)  matrix[9] &= ~0x10;
   if (button_A)  matrix[9] &= ~0x20;

   for (size_t i = 0; i < sizeof(kKeyMap) / sizeof(kKeyMap[0]); ++i)
   {
      if (input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, kKeyMap[i].retrok))
         matrix[kKeyMap[i].line] &= ~(1 << kKeyMap[i].bit);
   }

   TickAutorun(matrix);

   static unsigned char prev_matrix[10] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
   for (int i = 0; i < 10; ++i)
   {
      if (matrix[i] != prev_matrix[i])
         log_cb(RETRO_LOG_DEBUG, "CPC key matrix line %d: 0x%02X -> 0x%02X\n", i, prev_matrix[i], matrix[i]);
   }
   memcpy(prev_matrix, matrix, sizeof(matrix));

   if (emulator_ != nullptr)
      emulator_->GetKeyboardHandler()->ForceKeyboardState(matrix);

   if (rumble.set_rumble_state)
   {
      static bool old_start;
      static bool old_select;
      uint16_t strength_strong = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2) ? 0x4000 : 0xffff;
      uint16_t strength_weak = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2) ? 0x4000 : 0xffff;
      bool start = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START);
      bool select = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT);
      if (old_start != start)
         log_cb(RETRO_LOG_INFO, "Strong rumble: %s.\n", start ? "ON" : "OFF");
      rumble.set_rumble_state(0, RETRO_RUMBLE_STRONG, start * strength_strong);

      if (old_select != select)
         log_cb(RETRO_LOG_INFO, "Weak rumble: %s.\n", select ? "ON" : "OFF");
      rumble.set_rumble_state(0, RETRO_RUMBLE_WEAK, select * strength_weak);

      old_start = start;
      old_select = select;
   }
}



// Bundled ROM filenames under <system_directory>/amstradcpc/ROM/ -- see
// RetroDirectories above. A user's own dumps of the same name override the
// bundled default; that's the whole point of routing this through
// IDirectories/GetBaseDirectory() instead of embedding these too.
static std::string last_applied_model_;

static void ApplyMachineType(const char* model)
{
   if (model == nullptr || last_applied_model_ == model)
      return;
   last_applied_model_ = model;

   MachineSettings::HardwareType hw;
   const char* lower_rom;
   const char* upper_rom;
   MachineSettings::RamCfg ram;
   bool tape_plugged = true;
   bool fdc_plugged = true;
   const char* cartridge_file = nullptr; // non-null => Plus/GX4000-style cartridge boot

   if (!strcmp(model, "664"))
   {
      hw = MachineSettings::OLD_664;
      lower_rom = "os664.rom";
      upper_rom = "basic664.rom";
      ram = MachineSettings::M64_K;
   }
   else if (!strcmp(model, "464"))
   {
      hw = MachineSettings::OLD_464;
      lower_rom = "os464.rom";
      upper_rom = "basic464.rom";
      ram = MachineSettings::M64_K;
   }
   else if (!strcmp(model, "gx4000"))
   {
      // GX4000 is a games console, not a computer: no keyboard, no disk
      // drive, no tape deck. Same cartridge-plus-ROM boot as plus6128, but
      // tape/FDC are correctly absent -- RunFullSpeed() dispatches to a
      // different StartOptimizedPlus<...> instantiation based on these
      // flags, so this isn't just cosmetic.
      hw = MachineSettings::GX400;
      lower_rom = "os6128.rom";
      upper_rom = "basic6128.rom";
      ram = MachineSettings::M64_K;
      tape_plugged = false;
      fdc_plugged = false;
      cartridge_file = "system.cpr";
   }
   else if (!strcmp(model, "plus6128"))
   {
      // Plus/GX4000 hardware has no separate "Plus OS" ROM: the ASIC's
      // extensions live entirely in the system cartridge (system.cpr,
      // below), and the plain lower/upper ROM banks stay backward-compatible
      // with a standard 6128's OS+BASIC. Confirmed against Abdess/retrobios
      // (github.com/Abdess/retrobios/tree/main/bios/Amstrad/CPC, a
      // source-verified BIOS collection cross-checked against emulator
      // source): it ships no Plus-specific os/basic pair either, only the
      // same os6128.rom/basic6128.rom plus one shared system.cpr for both
      // Plus and GX4000 -- which is byte-identical to the plus_en.cpr this
      // repo already had from CPCCore's own test assets.
      hw = MachineSettings::PLUS_6128;
      lower_rom = "os6128.rom";
      upper_rom = "basic6128.rom";
      ram = MachineSettings::M128_K;
      cartridge_file = "system.cpr";
   }
   else // "6128", and the fallback for anything unrecognised
   {
      hw = MachineSettings::OLD_6128;
      lower_rom = "os6128.rom";
      upper_rom = "basic6128.rom";
      ram = MachineSettings::M128_K;
   }

   machine_settings_.SetHardwareType(hw);
   machine_settings_.SetRamCfg(ram);
   // SetLowerRom/SetUpperRom take non-const char* (see MachineSettings.h) but
   // only ever read from it here -- const_cast is safe, not UB, since these
   // string literals are never written through.
   machine_settings_.SetLowerRom(const_cast<char*>(lower_rom));
   machine_settings_.SetUpperRom(0, upper_rom);
   machine_settings_.SetCRTCType(CRTC::AMS40226);
   machine_settings_.SetTapePlugged(tape_plugged);
   machine_settings_.SetFDCPlugged(fdc_plugged);
   machine_settings_.SetPALPlugged(true);

   std::string cart_path;
   if (cartridge_file != nullptr)
   {
      // LoadCpr() takes this as a plain fopen() path, unlike LoadRom() --
      // it does not join it with GetBaseDirectory() itself, so the full
      // path has to be built here.
      cart_path = std::string(directories_.GetBaseDirectory()) + "/ROM/" + cartridge_file;
      machine_settings_.SetDefaultCartridge(cart_path.c_str());
   }

   if (emulator_ != nullptr)
   {
      emulator_->ChangeSettings(&machine_settings_); // calls UpdateComputer()
      // UpdateComputer() only auto-calls LoadCpr() for PLUS_6128/PLUS_464
      // (see its hardware_type check in Machine.cpp) -- GX400 is not in that
      // list, so load it explicitly here for every cartridge-booting model.
      // Calling LoadCpr() a second time for Plus (already auto-loaded) is
      // harmless -- it just re-reads the same banks.
      if (!cart_path.empty())
         emulator_->LoadCpr(cart_path.c_str());
   }
}

static bool autorun_enabled_ = true;

static void check_variables(void)
{
   struct retro_variable var = { 0 };
   var.key = "amstradcpc_model";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      ApplyMachineType(var.value);

   var.key = "sugarbox_autorun";
   var.value = nullptr;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      autorun_enabled_ = (strcmp(var.value, "enabled") == 0);

   float last = last_aspect;
   float last_rate = last_sample_rate;
   struct retro_system_av_info info;
   retro_get_system_av_info(&info);

   if ((last != last_aspect && last != 0.0f) || (last_rate != last_sample_rate && last_rate != 0.0f))
   {
      // SET_SYSTEM_AV_INFO can only be called within retro_run().
      // check_variables() is called once in retro_load_game(), but the checks
      // on last and last_rate ensures this path is never hit that early.
      // last_aspect and last_sample_rate are not updated until retro_get_system_av_info(),
      // which must come after retro_load_game().
      bool ret;
      if (last_rate != last_sample_rate && last_rate != 0.0f) // If audio rate changes, go through SET_SYSTEM_AV_INFO.
         ret = environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &info);
      else // If only aspect changed, take the simpler path.
         ret = environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &info.geometry);
      log_cb(RETRO_LOG_INFO, "SET_SYSTEM_AV_INFO/SET_GEOMETRY = %u.\n", ret);
   }
}

static void audio_callback(void)
{
   for (unsigned i = 0; i < 30000 / 60; i++, phase++)
   {
      int16_t val = 0x800 * sinf(2.0f * M_PI * phase * 300.0f / 30000.0f);
      audio_cb(val, val);
   }

   phase %= 100;
}

static void audio_set_state(bool enable)
{
   (void)enable;
}

void retro_run(void)
{
   // RunFullSpeed() reads current_settings_ (TapePlugged/FDCPlugged/expansion
   // count) and dispatches to the right StartOptimizedPlus<...> instantiation
   // itself -- this is what makes retro_run model-agnostic. Its time_slice_
   // default (20ms) is exactly one 50Hz frame, so one call per retro_run is
   // correct (the old hardcoded call ran a 20-frame slice per retro_run,
   // apparently a bug/quirk of the GX4000-only prototype).
   emulator_->RunFullSpeed();

   update_input();

   if (!use_audio_cb)
      audio_callback();

   bool updated = false;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      check_variables();
}

static void keyboard_cb(bool down, unsigned keycode,
   uint32_t character, uint16_t mod)
{
   log_cb(RETRO_LOG_INFO, "Down: %s, Code: %d, Char: %u, Mod: %u.\n",
      down ? "yes" : "no", keycode, character, mod);
}

// ---------------------------------------------------------------------------
// Disk swapping (RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE).
//
// EmulatorEngine::LoadDisk(const char*, drive_number) already auto-detects
// the real format from content (DiskGen::CreateDisk -> DiskBuilder::CanLoad,
// dispatching to CPCCoreEmu's own FormatType{DSK,EDSK,IPF,CTRAW,RAW,HFE,SCP}
// parsers) -- there's no MediaManager plumbing to wire up here beyond
// telling it which path to open. This is drive 0 (floppy A:) only; drive 1
// (B:) is a later addition if it turns out to matter for real-world CPC
// software (most titles are single-drive).
static std::vector<std::string> disk_images_;
static unsigned current_disk_index_ = 0;
static bool disk_ejected_ = false;

static bool HasExtension(const char* path, const char* ext_no_dot)
{
   const char* ext = strrchr(path, '.');
   if (ext == nullptr)
      return false;
   ++ext; // skip the dot
   size_t i = 0;
   for (; ext[i] != '\0' && ext_no_dot[i] != '\0'; ++i)
   {
      char a = ext[i], b = ext_no_dot[i];
      if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
      if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
      if (a != b)
         return false;
   }
   return ext[i] == '\0' && ext_no_dot[i] == '\0';
}

static bool IsCartridgeFile(const char* path)
{
   return HasExtension(path, "cpr");
}

// CTape::InsertTape()'s magic-byte auto-detect covers ZXTape!/.tzx (which
// .cdt files also are, just under a CPC-specific extension -- no special
// case needed), CSW, WAV, and Creative Voice File; .tap has no distinct
// magic header, so it's the one format the deferred loader (InsertTapeDelayed)
// falls back to checking the extension for. This list only needs to match
// what CTape can actually end up loading.
static bool IsTapeFile(const char* path)
{
   return HasExtension(path, "cdt") || HasExtension(path, "tap") ||
          HasExtension(path, "tzx") || HasExtension(path, "csw") ||
          HasExtension(path, "voc") || HasExtension(path, "wav");
}

static bool dc_set_eject_state(bool ejected)
{
   if (emulator_ == nullptr)
      return false;
   if (ejected)
      emulator_->Eject(0);
   else if (current_disk_index_ < disk_images_.size())
      emulator_->LoadDisk(disk_images_[current_disk_index_].c_str(), 0);
   disk_ejected_ = ejected;
   return true;
}

static bool dc_get_eject_state(void)
{
   return disk_ejected_;
}

static unsigned dc_get_image_index(void)
{
   return current_disk_index_;
}

static bool dc_set_image_index(unsigned index)
{
   // Per the libretro spec this may only be called while ejected; setting
   // it just records the index; the frontend is expected to reinsert
   // (set_eject_state(false)) afterwards, which is what actually loads it.
   if (!disk_ejected_)
      return false;
   current_disk_index_ = index; // index >= size() is the valid "no disk" state
   return true;
}

static unsigned dc_get_num_images(void)
{
   return (unsigned)disk_images_.size();
}

static bool dc_replace_image_index(unsigned index, const struct retro_game_info* info)
{
   if (index >= disk_images_.size())
      return false;
   if (info == nullptr)
      disk_images_.erase(disk_images_.begin() + index);
   else
      disk_images_[index] = info->path;
   return true;
}

static bool dc_add_image_index(void)
{
   disk_images_.push_back(std::string());
   return true;
}

static struct retro_disk_control_callback disk_control_cb = {
   dc_set_eject_state,
   dc_get_eject_state,
   dc_get_image_index,
   dc_set_image_index,
   dc_get_num_images,
   dc_replace_image_index,
   dc_add_image_index,
};

bool retro_load_game(const struct retro_game_info *info)
{
   // Matches what update_input() actually reads (RETRO_DEVICE_ID_JOYPAD_
   // UP/DOWN/LEFT/RIGHT/X/A -> CPC joystick row 9) -- the previous list was
   // missing both fire buttons.
   struct retro_input_descriptor desc[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Fire 1" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Fire 2" },
      { 0 },
   };

   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);

   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      log_cb(RETRO_LOG_INFO, "XRGB8888 is not supported.\n");
      return false;
   }

   struct retro_keyboard_callback cb = { keyboard_cb };
   environ_cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &cb);
   if (environ_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble))
      log_cb(RETRO_LOG_INFO, "Rumble environment supported.\n");
   else
      log_cb(RETRO_LOG_INFO, "Rumble environment not supported.\n");

   struct retro_audio_callback audio_cb = { audio_callback, audio_set_state };
   use_audio_cb = environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK, &audio_cb);

   environ_cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE, &disk_control_cb);
   disk_images_.clear();
   current_disk_index_ = 0;
   disk_ejected_ = false;

   check_variables();

   if (info != nullptr && info->path != nullptr)
   {
      if (IsCartridgeFile(info->path))
      {
         // Cartridge banks are a distinct memory region from ROM/disk (see
         // LoadCprFromBuffer) -- a fresh cartridge genuinely does need the
         // machine reset, same as swapping a real GX4000 cartridge requires
         // a power cycle.
         FILE* f = fopen(info->path, "rb");
         if (f != nullptr)
         {
            fseek(f, 0, SEEK_END);
            unsigned int buffer_size_ = ftell(f);
            rewind(f);
            unsigned char* buffer_ = new unsigned char[buffer_size_];

            fread(buffer_, buffer_size_, 1, f);
            LoadCprFromBuffer(buffer_, buffer_size_);
            delete[] buffer_;
            fclose(f);
         }

         motherboard_->GetPSG()->Reset();
         motherboard_->GetSig()->Reset();
         motherboard_->InitStartOptimizedPlus();
         motherboard_->OnOff();
      }
      else if (IsTapeFile(info->path))
      {
         // Tape, same no-reset reasoning as disk: EmulatorEngine::LoadTape()
         // just queues it (CTape::InsertTape sets pending_tape_ and defers
         // the actual format auto-detect/read to InsertTapeDelayed(), which
         // fires on its own during normal emulation ticking) -- no manual
         // "run one frame to process the deferred load" call needed here.
         emulator_->LoadTape(info->path);
         if (autorun_enabled_) ArmAutorun();
      }
      else
      {
         // Disk: EmulatorEngine::LoadDisk() auto-detects the real format
         // from content and inserts it into drive A: (0) without needing a
         // machine reset -- inserting a floppy doesn't reboot a real CPC
         // either. Also seed the disk-control image list with it so
         // set_image_index()/get_num_images() have something to report even
         // before the frontend calls add_image_index() for an M3U's other
         // entries.
         disk_images_.push_back(info->path);
         emulator_->LoadDisk(info->path, 0);
         if (autorun_enabled_) ArmAutorun();
      }
   }

   return true;
}

void retro_unload_game(void)
{
   last_aspect = 0.0f;
   last_sample_rate = 0.0f;
}

unsigned retro_get_region(void)
{
   // The Amstrad CPC is a European 50Hz machine (matches
   // retro_get_system_av_info's info->timing.fps = 50.0) -- was reporting
   // NTSC, leftover from the libretro-common sample core.
   return RETRO_REGION_PAL;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
   if (type != 0x200)
      return false;
   if (num != 2)
      return false;
   return retro_load_game(NULL);
}

// CSnapshot (Snapshot.h/.cpp) only exposes a file-path API
// (SaveSnapshot/LoadSnapshot write/read a real .sna file) -- there is no
// in-memory-buffer serializer to call directly, unlike LoadDisk/LoadTape.
// libretro's retro_serialize/unserialize work against a caller-owned memory
// buffer, so this bridges the two through a scratch file under the system
// directory: SaveSnapshot() writes it, then its bytes get read back into the
// libretro buffer (and the reverse for unserialize). Not zero-copy, but a
// full CPC snapshot is a few hundred KB at most -- one extra file round
// trip per save/load is not a meaningful cost.
static std::string GetScratchSnapshotPath()
{
   return std::string(directories_.GetBaseDirectory()) + "/savestate.tmp.sna";
}

// EmulatorEngine::SaveSnapshot() does NOT write synchronously -- it just
// arms a flag (do_snapshot_) and stops the Z80 on its next instruction-fetch
// boundary; the real write happens inside HandleSnapshots(), which only runs
// as part of RunFullSpeed(). So triggering a save means: delete any stale
// file at this path first (SaveSnapshot()'s own "did it succeed" return
// value only reflects "flag armed", not "file written" -- can't be used to
// detect completion), call SaveSnapshot(), then keep ticking the emulator
// until the file actually exists on disk. A Z80 fetch boundary happens
// every few cycles, so this should resolve within the first RunFullSpeed()
// call in practice; the iteration cap is just a safety net against a wedged
// emulator, not the expected path.
static bool RunUntilSnapshotWritten(const std::string& path)
{
   remove(path.c_str());
   if (!emulator_->SaveSnapshot(path.c_str()))
      return false;
   for (int i = 0; i < 50; ++i)
   {
      FILE* probe = fopen(path.c_str(), "rb");
      if (probe != nullptr)
      {
         fclose(probe);
         return true;
      }
      emulator_->RunFullSpeed();
   }
   return false;
}

size_t retro_serialize_size(void)
{
   if (emulator_ == nullptr)
      return 0;
   const std::string path = GetScratchSnapshotPath();
   if (!RunUntilSnapshotWritten(path))
      return 0;
   FILE* f = fopen(path.c_str(), "rb");
   if (f == nullptr)
      return 0;
   fseek(f, 0, SEEK_END);
   const long size = ftell(f);
   fclose(f);
   remove(path.c_str());
   // RetroArch queries this once and allocates a buffer of exactly this
   // size for every later retro_serialize() call -- pad generously since a
   // real save later (different game state, different tape/disk position)
   // can legitimately produce a slightly larger .sna than this first probe.
   return size > 0 ? (size_t)size + 4096 : 0;
}

bool retro_serialize(void *data_, size_t size)
{
   if (emulator_ == nullptr)
      return false;
   const std::string path = GetScratchSnapshotPath();
   if (!RunUntilSnapshotWritten(path))
      return false;

   FILE* f = fopen(path.c_str(), "rb");
   if (f == nullptr)
      return false;
   fseek(f, 0, SEEK_END);
   const long file_size = ftell(f);
   rewind(f);

   bool ok = false;
   if (file_size > 0 && (size_t)file_size <= size)
   {
      const size_t read_bytes = fread(data_, 1, (size_t)file_size, f);
      ok = (read_bytes == (size_t)file_size);
      // Zero the rest so a shorter save doesn't leave stale bytes from a
      // previous, larger one lying around in the frontend's buffer.
      if (ok && (size_t)file_size < size)
         memset((uint8_t*)data_ + file_size, 0, size - (size_t)file_size);
   }
   fclose(f);
   remove(path.c_str());
   return ok;
}

bool retro_unserialize(const void *data_, size_t size)
{
   if (emulator_ == nullptr || size == 0)
      return false;
   const std::string path = GetScratchSnapshotPath();

   FILE* f = fopen(path.c_str(), "wb");
   if (f == nullptr)
      return false;
   const size_t written = fwrite(data_, 1, size, f);
   fclose(f);
   if (written != size)
   {
      remove(path.c_str());
      return false;
   }

   // LoadSnapshotNow(), not LoadSnapshot(): the latter just queues the load
   // for HandleSnapshots() to pick up on a later RunFullSpeed() tick (same
   // deferred pattern as SaveSnapshot()); retro_unserialize's contract needs
   // the state fully applied before it returns, so this needs the
   // synchronous variant.
   const bool ok = emulator_->LoadSnapshotNow(path.c_str());
   remove(path.c_str());
   return ok;
}

void *retro_get_memory_data(unsigned id)
{
   (void)id;
   return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
   (void)id;
   return 0;
}

void retro_cheat_reset(void)
{}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   (void)index;
   (void)enabled;
   (void)code;
}
