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
#include <mutex>
#include <unistd.h>

// Visible window cut out of the emulator's internal raster buffer, which is
// 1024 ints wide with rows written at 2y (see RetroDisplay::GetVideoBuffer),
// so roughly 1008 x 576 of it is real picture.
//
// "normal" is the long-standing crop: picture plus a thin border, which is
// what most software expects. "full" widens it to show the CPC's overscan
// region, which demos and a fair amount of French software draw into. The
// frontend is told the full size as its maximum so the geometry can change at
// runtime without a reinit; cap32 exposes the same idea as cap32_scr_crop.
#define WIDTH  640
#define HEIGHT 480
#define OFFSET_X 207
#define OFFSET_Y 84

#define FULL_WIDTH  800
#define FULL_HEIGHT 560
#define FULL_OFFSET_X 112
#define FULL_OFFSET_Y 8


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
   // Monitor type (gap #9a). The real CPC shipped with either the CTM644
   // colour monitor or the GT65 green-phosphor monochrome one, and a lot of
   // period software was designed against the latter.
   enum MonitorType { MONITOR_COLOR = 0, MONITOR_GREEN, MONITOR_AMBER };

   RetroDisplay()
   {
      // Init
      video_buffer = new int[1024 * 1024];
      memset(video_buffer, 0, 1024 * 1024 * sizeof(int));
      mono_buffer_ = new int[FULL_WIDTH * FULL_HEIGHT];
      memset(mono_buffer_, 0, FULL_WIDTH * FULL_HEIGHT * sizeof(int));
      pitch_ = 1024 * sizeof(unsigned int);
   };
   virtual ~RetroDisplay() { delete[] mono_buffer_; };

   void SetMonitorType(MonitorType type) { monitor_type_ = type; }

   void SetFullBorder(bool on)
   {
      full_border_ = on;
      crop_w_ = on ? FULL_WIDTH : WIDTH;
      crop_h_ = on ? FULL_HEIGHT : HEIGHT;
      crop_x_ = on ? FULL_OFFSET_X : OFFSET_X;
      crop_y_ = on ? FULL_OFFSET_Y : OFFSET_Y;
   }
   int CropWidth() const { return crop_w_; }
   int CropHeight() const { return crop_h_; }

   virtual void SetScanlines(int scan) {};
   virtual void Display() {};
   virtual bool AFrameIsReady() { return true; };
   virtual void Config() {};
   virtual const char* GetInformations() { return "Libretro GDI"; };
   virtual int GetWidth() { return crop_w_; };
   virtual int GetHeight() { return crop_h_; };
   virtual void VSync(bool bDbg)
   {
      const int* src = &video_buffer[crop_x_ + 1024 * crop_y_];
      if (monitor_type_ == MONITOR_COLOR)
      {
         video_cb(src, crop_w_, crop_h_, pitch_);
         return;
      }

      // Monochrome monitor emulation, done here on the finished frame
      // rather than by overriding IDisplay::ConvertRGB(). ConvertRGB() is
      // only consulted when GateArray::SetMonitor() builds the classic
      // gate-array palette (VGA.cpp) -- the Plus/ASIC path writes
      // ink_list_ directly as packed RGB (Memory::UpdateAsicPalette in
      // Memoire.cpp) and never goes through it, so a palette-level hook
      // would silently do nothing on 6128+/GX4000. Converting the frame
      // covers every model. Writes to a separate buffer: the emulator's
      // own frame buffer persists across frames, so an in-place conversion
      // would compound every frame.
      //
      // Rec.601 luma, tinted to the phosphor colour.
      for (int y = 0; y < crop_h_; ++y)
      {
         const int* in = src + 1024 * y;
         int* out = mono_buffer_ + crop_w_ * y;
         for (int x = 0; x < crop_w_; ++x)
         {
            const unsigned int p = (unsigned int)in[x];
            const unsigned int r = (p >> 16) & 0xFF;
            const unsigned int g = (p >> 8) & 0xFF;
            const unsigned int b = p & 0xFF;
            const unsigned int luma = (77 * r + 150 * g + 29 * b) >> 8;
            unsigned int outr, outg, outb;
            if (monitor_type_ == MONITOR_GREEN)
            {
               outr = (luma * 40) >> 8;
               outg = luma;
               outb = (luma * 40) >> 8;
            }
            else // MONITOR_AMBER
            {
               outr = luma;
               outg = (luma * 190) >> 8;
               outb = (luma * 25) >> 8;
            }
            out[x] = (int)(0xFF000000u | (outr << 16) | (outg << 8) | outb);
         }
      }
      video_cb(mono_buffer_, crop_w_, crop_h_, crop_w_ * sizeof(unsigned int));
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
   int * mono_buffer_ = nullptr;
   MonitorType monitor_type_ = MONITOR_COLOR;
   bool full_border_ = false;
   int crop_w_ = WIDTH, crop_h_ = HEIGHT, crop_x_ = OFFSET_X, crop_y_ = OFFSET_Y;
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
   { RETROK_SLASH,       3, 6 }, { RETROK_PERIOD,  3, 7 },
   { RETROK_0,           4, 0 }, { RETROK_9,       4, 1 }, { RETROK_o,      4, 2 },
   { RETROK_i,           4, 3 }, { RETROK_l,       4, 4 }, { RETROK_k,      4, 5 },
   { RETROK_m,           4, 6 }, { RETROK_COMMA,   4, 7 },
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

// AZERTY host-keyboard overrides (gap #8).
//
// RetroArch reports RETROK_* by PHYSICAL key position against a US/QWERTY
// reference, not by the host's active layout -- verified empirically: with
// a French layout active, `xdotool key a` (the key labelled A, physically
// where QWERTY has Q) arrived as RETROK_q. The bundled OS ROMs are the UK
// ones, so the emulated machine always decodes a matrix position to the
// English character. The result for an AZERTY user is that the key they
// press and the character the CPC prints disagree.
//
// This remaps the affected physical keys to the matrix position whose
// ENGLISH character matches what is actually printed on the AZERTY keycap
// (letter positions cross-checked against SugarboxV2's own
// CONF/KeyboardMaps.ini [FRENCH] vs [ENGLISH] sections, which differ in
// exactly 26 entries).
//
// Deliberately limited to the letter/punctuation swaps: AZERTY's digit row
// is shifted (unshifted gives &e"'( ...), which cannot be corrected by
// remapping positions alone -- it needs character-level input synthesis
// (injecting/suppressing the CPC's own shift), a much larger change. Digits
// and symbols therefore still follow the English ROM.
static const KeyMapEntry kKeyMapOverridesFR[] = {
   { RETROK_a,         8, 3 }, // physical A -> AZERTY 'Q'
   { RETROK_q,         8, 5 }, // physical Q -> AZERTY 'A'
   { RETROK_w,         8, 7 }, // physical W -> AZERTY 'Z'
   { RETROK_z,         7, 3 }, // physical Z -> AZERTY 'W'
   { RETROK_SEMICOLON, 4, 6 }, // physical ; -> AZERTY 'M'
   { RETROK_m,         4, 7 }, // physical M -> AZERTY ','
   { RETROK_COMMA,     3, 4 }, // physical , -> AZERTY ';'
   { RETROK_PERIOD,    3, 5 }, // physical . -> AZERTY ':'
};

// kKeyMap with the active layout's overrides applied; rebuilt by
// ApplyKeyboardLayout() whenever the core option changes.
static KeyMapEntry active_keymap_[sizeof(kKeyMap) / sizeof(kKeyMap[0])];
static size_t active_keymap_size_ = 0;

static void ApplyKeyboardLayout(const char* layout)
{
   memcpy(active_keymap_, kKeyMap, sizeof(kKeyMap));
   active_keymap_size_ = sizeof(kKeyMap) / sizeof(kKeyMap[0]);
   if (layout == nullptr || strcmp(layout, "fr") != 0)
      return;
   for (size_t o = 0; o < sizeof(kKeyMapOverridesFR) / sizeof(kKeyMapOverridesFR[0]); ++o)
   {
      for (size_t i = 0; i < active_keymap_size_; ++i)
      {
         if (active_keymap_[i].retrok == kKeyMapOverridesFR[o].retrok)
            active_keymap_[i] = kKeyMapOverridesFR[o];
      }
   }
}

// Autorun: types a launch command shortly after a fresh disk/tape load.
// The CPC needs one to start anything that isn't a .cpr cartridge -- real
// hardware behaviour, not something specific to this core.
//
// The command itself comes from the engine, not guessed here: FDC::
// GetAutorun(drive, buf, len) -> DiskGen::GetAutorun() reads the inserted
// disk's actual catalogue and applies a rule table (DiskGen.cpp), returning
// AUTO_FILE + a filename (-> RUN"<file>) or AUTO_CPM (-> |CPM). This works
// for every disk format the engine can parse, including the flux ones,
// because it goes through IDisk::GetCat() rather than parsing an image
// here. Same call SugarboxV2's own Emulation::ItemLoaded() uses.
//
// This does NOT use EmulatorEngine::Paste()/CharPressed() -- that path
// resolves typed characters against KeyboardHandler::keyboard_map_, which
// is only ever populated from CONF/KeyboardMaps.ini via
// ConfigurationManager, and that path was deliberately stubbed to return
// sentinel defaults (see ConfigurationManager::GetConfiguration* above) to
// silence log spam from ~600 undeclared per-key lookups -- so
// keyboard_map_ has no real char associations and Paste() would silently
// do nothing. Instead this drives the same matrix update_input() already
// writes each frame, which is proven working.
//
// Character -> (line, bit, shift) for everything an AMSDOS command line can
// need, derived from the canonical matrix documented above (in each of that
// table's two-character annotations the FIRST character is the shifted
// result, the second the unshifted one -- verified empirically for
// shift+2 = '"').
struct AutorunKey { char c; int line; int bit; bool shift; };
static const AutorunKey kAutorunKeys[] = {
   // Letters (unshifted; AMSDOS filenames are case-insensitive)
   { 'A', 8, 5, false }, { 'B', 6, 6, false }, { 'C', 7, 6, false },
   { 'D', 7, 5, false }, { 'E', 7, 2, false }, { 'F', 6, 5, false },
   { 'G', 6, 4, false }, { 'H', 5, 4, false }, { 'I', 4, 3, false },
   { 'J', 5, 5, false }, { 'K', 4, 5, false }, { 'L', 4, 4, false },
   { 'M', 4, 6, false }, { 'N', 5, 6, false }, { 'O', 4, 2, false },
   { 'P', 3, 3, false }, { 'Q', 8, 3, false }, { 'R', 6, 2, false },
   { 'S', 7, 4, false }, { 'T', 6, 3, false }, { 'U', 5, 2, false },
   { 'V', 6, 7, false }, { 'W', 7, 3, false }, { 'X', 7, 7, false },
   { 'Y', 5, 3, false }, { 'Z', 8, 7, false },
   // Digits (unshifted)
   { '0', 4, 0, false }, { '1', 8, 0, false }, { '2', 8, 1, false },
   { '3', 7, 1, false }, { '4', 7, 0, false }, { '5', 6, 1, false },
   { '6', 6, 0, false }, { '7', 5, 1, false }, { '8', 5, 0, false },
   { '9', 4, 1, false },
   // Punctuation an AMSDOS filename / RSX command can contain
   { ' ',  5, 7, false }, { '\r', 2, 2, false },
   { '.',  3, 7, false }, { ',',  4, 7, false }, { ':',  3, 5, false },
   { ';',  3, 4, false }, { '/',  3, 6, false }, { '-',  3, 1, false },
   { '@',  3, 2, false }, { '[',  2, 1, false }, { ']',  2, 3, false },
   { '\\', 2, 6, false },
   // Shifted forms
   { '"', 8, 1, true }, { '|', 3, 2, true }, { '!', 8, 0, true },
   { '*', 3, 5, true }, { '+', 3, 4, true }, { '?', 3, 6, true },
   { '=', 3, 1, true }, { '<', 4, 7, true }, { '>', 3, 7, true },
   { '(', 5, 0, true }, { ')', 4, 1, true }, { '_', 4, 0, true },
   { '\'', 5, 1, true }, { '&', 6, 0, true }, { '%', 6, 1, true },
   { '$', 7, 0, true }, { '#', 7, 1, true }, { '^', 3, 0, true },
};

// Filled by ArmAutorun(); "RUN\"<file>\r", "|CPM\r", "CAT\r" or "RUN\"\r".
static char autorun_sequence_[64] = { 0 };
enum AutorunState { AUTORUN_IDLE, AUTORUN_WAITING, AUTORUN_PRE_SHIFT, AUTORUN_PRESS, AUTORUN_POST_SHIFT, AUTORUN_RELEASE, AUTORUN_DONE };
static AutorunState autorun_state_ = AUTORUN_IDLE;
static int autorun_timer_ = 0;
static unsigned autorun_char_index_ = 0;

// ~3s at 50Hz before typing (mirrors CPCCoreEmu's own Paste() gate, which
// waits for 2000ms of emulated time before considering the machine ready),
// ~400ms per phase -- generous for the CPC's keyboard scan rate.
// Uppercased on the way in so a lowercase catalogue entry still matches
// kAutorunKeys (AMSDOS filenames are case-insensitive).
static void ArmAutorun(const char* command)
{
   if (command == nullptr || *command == '\0')
      return;
   size_t i = 0;
   for (; command[i] != '\0' && i < sizeof(autorun_sequence_) - 1; ++i)
   {
      char c = command[i];
      if (c >= 'a' && c <= 'z')
         c -= 'a' - 'A';
      autorun_sequence_[i] = c;
   }
   autorun_sequence_[i] = '\0';
   autorun_state_ = AUTORUN_WAITING;
   autorun_timer_ = 150;
   autorun_char_index_ = 0;
}

static void TickAutorun(unsigned char matrix[10])
{
   if (autorun_state_ == AUTORUN_IDLE || autorun_state_ == AUTORUN_DONE)
      return;

   if (autorun_state_ == AUTORUN_WAITING)
   {
      if (--autorun_timer_ <= 0)
      {
         autorun_state_ = AUTORUN_PRE_SHIFT;
         autorun_timer_ = 20;
      }
      return;
   }

   const char c = autorun_sequence_[autorun_char_index_];
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

   // Staggered phases so a shifted key produces two SEPARATE scan
   // transitions (shift-alone, then the letter while shift is already
   // stable) instead of both bits changing in the same instant. Found
   // necessary empirically: a real screenshot showed the simultaneous
   // shift+2 combo landing as a bare unshifted '2' (BASIC read "run2"),
   // while single-key presses (R/U/N) worked once landing in the cached
   // keyboard buffer at all (see the ForceKeyboardState -> GetKeyboardState
   // fix above) -- the CPC's keyboard-scan/debounce logic appears to only
   // register one new key-transition per scan.
   //   unshifted key: PRE_SHIFT (skipped) -> PRESS (key) -> RELEASE (none)
   //   shifted key:   PRE_SHIFT (shift) -> PRESS (shift+key) ->
   //                  POST_SHIFT (shift only) -> RELEASE (none)
   switch (autorun_state_)
   {
   case AUTORUN_PRE_SHIFT:
      if (!key->shift)
      {
         autorun_state_ = AUTORUN_PRESS;
         autorun_timer_ = 20;
         break;
      }
      matrix[2] &= ~(1 << 5);
      if (--autorun_timer_ <= 0) { autorun_state_ = AUTORUN_PRESS; autorun_timer_ = 20; }
      break;

   case AUTORUN_PRESS:
      matrix[key->line] &= ~(1 << key->bit);
      if (key->shift) matrix[2] &= ~(1 << 5);
      if (--autorun_timer_ <= 0)
      {
         autorun_state_ = key->shift ? AUTORUN_POST_SHIFT : AUTORUN_RELEASE;
         autorun_timer_ = 20;
      }
      break;

   case AUTORUN_POST_SHIFT:
      matrix[2] &= ~(1 << 5); // key released, shift still held
      if (--autorun_timer_ <= 0) { autorun_state_ = AUTORUN_RELEASE; autorun_timer_ = 20; }
      break;

   case AUTORUN_RELEASE:
      if (--autorun_timer_ <= 0)
      {
         ++autorun_char_index_;
         autorun_state_ = AUTORUN_PRE_SHIFT;
         autorun_timer_ = 20;
      }
      break;

   default:
      break;
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
static float last_aspect;
static float last_sample_rate;

// ---------------------------------------------------------------------------
// PSG audio bridge (was completely missing -- see below).
//
// EmulatorEngine::Init(IDisplay*, ISoundFactory*) -- the overload this file
// calls -- takes a sound-factory-shaped parameter but never uses it (see its
// body in Machine.cpp: display_ is stored, `sound` is not referenced at
// all). The real hookup point is the separate EmulatorEngine::InitSound
// (ISound*), which wires SoundMixer::Init(sound, GetTape()) -- this file
// never called it, calling Motherboard::GetPSG()->InitSound(nullptr)
// directly instead (bypassing EmulatorEngine's wrapper) with a null ISound.
// Confirmed via a real audio capture (parecord on the actual PulseAudio
// output while a game ran) that this produced zero audio output --
// digital silence, not just "not forwarded to the frontend" -- for this
// entire project prior to this fix.
//
// SoundMixer runs its own background thread once given a non-null ISound
// (SoundMixer::Init -> PrepareBufferThread -> Loop(), gated on
// `#ifndef NO_MULTITHREAD`, which is NOT defined for the real CPCCoreEmu.a
// build -- removed from this file too, see the ODR-violation note above --
// so this is genuinely concurrent). GetFreeBuffer()/AddBufferToPlay() are
// therefore called from that background thread; DrainToLibretro() must
// only ever be called from the main thread (inside retro_run()) so
// audio_batch_cb() itself never has to be assumed thread-safe.
//
// Min/max/bit-depth/channel values match SugarboxV2's own real, working
// ISound implementation (Sugarbox/ALSoundMixer.cpp -- GetMaxValue()
// returns (1<<16)-1, GetMinValue() returns 0, 16-bit stereo), the same
// reference used for the floppy-sound assets below.
#define AUDIO_BUFFER_FRAMES 1024
#define AUDIO_NUM_BUFFERS 8

struct RetroWaveHDR : public IWaveHDR
{
   int16_t samples[AUDIO_BUFFER_FRAMES * 2]; // interleaved stereo
};

// ---------------------------------------------------------------------------
// Floppy-drive sound effects (gap #9b from the cap32/crocods audit).
//
// Ported from SugarboxV2's Emulation::ItemLoaded/DiskEject/TrackChanged
// (Emulation.cpp) -- the exact same IFdcNotify interface RetroFdcNotify
// below already implements, currently only for the "FDC: ... OK (0)" log
// line. WAV assets copied verbatim from the same app (see fdc_wav_data.h).
#include "fdc_wav_data.h"

// Real, empirically-discovered format mismatch: these 5 WAV assets are NOT
// uniformly encoded. seek_short/seek_long/drive_mo are 16-bit/44100Hz, but
// insert/eject are 8-bit (unsigned PCM, per the WAV spec)/22257Hz --
// confirmed by decoding each array's own header directly. An earlier
// version of this parser hardcoded "must be 16-bit" and silently rejected
// the 8-bit pair (parse=0), so ItemLoaded()'s insert-disk chime and
// DiskEject()'s eject chime never played. Supports both.
static bool ParseWavPcmMono(const unsigned char* wav, const unsigned char** out_data, size_t* out_count, unsigned* out_rate, unsigned* out_bits)
{
   // Canonical 44-byte PCM WAV header (RIFF/WAVEfmt , 16-byte fmt chunk,
   // 8-byte data-chunk header) -- same fixed-offset layout SugarboxV2's own
   // ALSoundMixer::AddWav() parses (ALSoundMixer.cpp:346-356). Reading the
   // real data size from the header itself (offset 0x28) rather than
   // trusting a caller-supplied length is what makes this immune to
   // Emulation.cpp's own AddWav() call-site bug (every call there passes
   // sizeof(seek_short_wav) regardless of which array).
   if (memcmp(wav, "RIFF", 4) != 0 || memcmp(wav + 8, "WAVE", 4) != 0)
      return false;
   const uint16_t channels = wav[0x16] | (wav[0x17] << 8);
   const uint32_t rate = wav[0x18] | (wav[0x19] << 8) | (wav[0x1A] << 16) | (wav[0x1B] << 24);
   const uint16_t bits = wav[0x22] | (wav[0x23] << 8);
   const uint32_t data_size = wav[0x28] | (wav[0x29] << 8) | (wav[0x2A] << 16) | (wav[0x2B] << 24);
   if (channels != 1 || (bits != 8 && bits != 16))
      return false;
   *out_data = wav + 0x2C;
   *out_count = data_size / (bits / 8);
   *out_rate = rate;
   *out_bits = bits;
   return true;
}

// One-shot at a time (a new trigger replaces whatever's playing) -- matches
// real hardware, where a single floppy drive can't seek-short and
// seek-long simultaneously. Only ever touched from the main thread (both
// PlayFdcSfx, called from RetroFdcNotify's callbacks during RunFullSpeed(),
// and MixFdcSound, called from DrainToLibretro() -- no locking needed.
static const unsigned char* fdc_sfx_data_ = nullptr;
static size_t fdc_sfx_total_ = 0;
static unsigned fdc_sfx_bits_ = 16;
static double fdc_sfx_step_ = 1.0;   // source-rate -> output-rate resample step
static double fdc_sfx_srcpos_ = 0.0;

static void PlayFdcSfx(const unsigned char* wav, unsigned output_rate)
{
   const unsigned char* data = nullptr;
   size_t count = 0;
   unsigned rate = 44100;
   unsigned bits = 16;
   if (!ParseWavPcmMono(wav, &data, &count, &rate, &bits))
      return;
   fdc_sfx_data_ = data;
   fdc_sfx_total_ = count;
   fdc_sfx_bits_ = bits;
   fdc_sfx_srcpos_ = 0.0;
   fdc_sfx_step_ = (output_rate > 0) ? (double)rate / (double)output_rate : 1.0;
}

// Nearest-neighbor resample + additive mix at -6dB (so a seek click never
// buries the PSG music/SFX it's layered under) with hard clamping.
// Adequate for short mechanical one-shots; not intended for music-quality
// resampling. 8-bit WAV PCM is unsigned (128 = silence); 16-bit is signed.
static void MixFdcSound(int16_t* stereo_buffer, size_t frames)
{
   if (fdc_sfx_data_ == nullptr)
      return;
   for (size_t i = 0; i < frames; i++)
   {
      const size_t src_index = (size_t)fdc_sfx_srcpos_;
      if (src_index >= fdc_sfx_total_)
      {
         fdc_sfx_data_ = nullptr;
         break;
      }
      const int16_t sample = (fdc_sfx_bits_ == 8)
         ? (int16_t)((fdc_sfx_data_[src_index] - 128) * 256)
         : reinterpret_cast<const int16_t*>(fdc_sfx_data_)[src_index];
      const int add = sample / 2;
      int mixed_l = stereo_buffer[i * 2] + add;
      int mixed_r = stereo_buffer[i * 2 + 1] + add;
      if (mixed_l > 32767) mixed_l = 32767; else if (mixed_l < -32768) mixed_l = -32768;
      if (mixed_r > 32767) mixed_r = 32767; else if (mixed_r < -32768) mixed_r = -32768;
      stereo_buffer[i * 2] = (int16_t)mixed_l;
      stereo_buffer[i * 2 + 1] = (int16_t)mixed_r;
      fdc_sfx_srcpos_ += fdc_sfx_step_;
   }
}

class RetroSound : public ISound
{
public:
   RetroSound()
   {
      for (auto& buf : buffers_)
      {
         buf.data_ = reinterpret_cast<char*>(buf.samples);
         buf.buffer_length_ = sizeof(buf.samples);
         buf.status_ = IWaveHDR::UNUSED;
      }
   }

   // ICfg -- no on-disk config, same reasoning as ConfigurationManager below.
   virtual void SetDefaultConfiguration() {}
   virtual void SaveConfiguration(const char*, const char*) {}
   virtual bool LoadConfiguration(const char*, const char*) { return true; }

   // ISound
   virtual bool Init(int, int, int) { return true; }
   virtual void Reinit() {}
   virtual unsigned int GetMaxValue() { return 65535; }
   virtual unsigned int GetMinValue() { return 0; }
   virtual unsigned int GetSampleRate() { return sample_rate_; }
   virtual unsigned int GetBitDepth() { return 16; }
   virtual unsigned int GetNbChannels() { return 2; }
   virtual void CheckBuffersStatus() {}

   virtual IWaveHDR* GetFreeBuffer()
   {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto& buf : buffers_)
      {
         if (buf.status_ == IWaveHDR::UNUSED)
         {
            buf.status_ = IWaveHDR::USED;
            return &buf;
         }
      }
      return nullptr; // SoundMixer tolerates this (discards the sound) -- see ConvertToWav.
   }

   virtual void AddBufferToPlay(IWaveHDR* buf)
   {
      std::lock_guard<std::mutex> lock(mutex_);
      buf->status_ = IWaveHDR::INQUEUE;
      ready_.push_back(static_cast<RetroWaveHDR*>(buf));
   }

   virtual void SyncOnSound(bool) {}
   virtual void SyncWithSound() {}

   void SetSampleRate(unsigned rate) { sample_rate_ = rate; }

   // Main thread only (called once per retro_run()). DEBUG-level (--verbose
   // only) heartbeat kept as a diagnostic aid -- this bridge (SoundMixer's
   // background thread feeding this queue, drained here) was non-obvious
   // enough to get wrong twice while building it; a real audio capture
   // (parecord) is still the authoritative test, not this log line.
   void DrainToLibretro()
   {
      std::vector<RetroWaveHDR*> ready;
      {
         std::lock_guard<std::mutex> lock(mutex_);
         ready.swap(ready_);
      }
      drain_calls_++;
      buffers_drained_ += (unsigned)ready.size();
      if (log_cb != nullptr && (drain_calls_ % 250) == 0)
         log_cb(RETRO_LOG_DEBUG, "RetroSound: %u drain calls, %u buffers total.\n", drain_calls_, buffers_drained_);
      for (RetroWaveHDR* buf : ready)
      {
         const size_t frames = buf->buffer_length_ / (2 * sizeof(int16_t));
         MixFdcSound(buf->samples, frames);
         audio_batch_cb(buf->samples, frames);
         std::lock_guard<std::mutex> lock(mutex_);
         buf->status_ = IWaveHDR::UNUSED;
      }
   }

private:
   RetroWaveHDR buffers_[AUDIO_NUM_BUFFERS];
   std::vector<RetroWaveHDR*> ready_;
   std::mutex mutex_;
   unsigned sample_rate_ = 44100;
   unsigned drain_calls_ = 0;
   unsigned buffers_drained_ = 0;
};
static RetroSound retro_sound_;

// FDC::LoadDisk() only reports success/failure through this notifier (see
// EmulatorEngine::LoadDisk's switch on the return code, which -- in the
// upstream Qt app -- feeds a message box; here it just logs so a failed
// load isn't silently indistinguishable from a working one in the core log).
static void HandleAutorunForLoadedItem(int load_ok, int drive_number);
static void ApplyDiskWriteProtect();
static void ApplyPlayCity();

class RetroFdcNotify : public IFdcNotify
{
public:
   virtual void ItemLoaded(const char* disk_path, int load_ok, int drive_number)
   {
      if (load_ok == 0)
         PlayFdcSfx(insert_wav, retro_sound_.GetSampleRate());
      if (log_cb != nullptr)
      {
         const char* what = (load_ok == 0) ? "OK" : (load_ok == -1) ? "file not found" : "unknown/unsupported format";
         log_cb(RETRO_LOG_INFO, "FDC: drive %d load '%s': %s (%d).\n", drive_number, disk_path, what, load_ok);
      }

      // Autorun, same trigger point and same engine call SugarboxV2's own
      // Emulation::ItemLoaded() uses. Deferred to a helper defined further
      // down, where emulator_/autorun_enabled_ are in scope.
      ApplyDiskWriteProtect();
      HandleAutorunForLoadedItem(load_ok, drive_number);
   }
   virtual void DiskEject() { PlayFdcSfx(eject_wav, retro_sound_.GetSampleRate()); }
   // Never actually called by the current CPCCoreEmu FDC (grepped -- only
   // DiskEject/ItemLoaded/TrackChanged fire), kept implemented in case a
   // future CPCCore version wires it up.
   virtual void DiskRunning(bool on) { if (on) PlayFdcSfx(drive_mo_wav, retro_sound_.GetSampleRate()); }
   virtual void TrackChanged(int nb_tracks)
   {
      // Same <20/>=20-track short/long split as SugarboxV2's Emulation::
      // TrackChanged (Emulation.cpp:568-574).
      PlayFdcSfx(nb_tracks < 20 ? seek_short_wav : seek_long_wav, retro_sound_.GetSampleRate());
   }
};

// Definition of emulator
static EmulatorEngine* emulator_ = nullptr;
static Motherboard * motherboard_ = nullptr;
static ConfigurationManager conf_manager_;
static RetroDisplay display_;
static RetroDirectories directories_;
static MachineSettings machine_settings_;
static RetroFdcNotify fdc_notify_;
static bool autorun_enabled_ = true;
// Where a modified disk goes. "sidecar" keeps the user's image pristine and
// writes changes beside it in the save directory; "overwrite" replaces the
// original in place; "disabled" discards. Surveying the field, emulators split
// four ways here -- silent in-place (Caprice Forever, JavaCPC, CPCemu), prompt
// (Arnold, WinAPE), explicit action only (Caprice32, SugarboxV2) and discard
// entirely (ACE-DL's shipped default, MAME) -- so there is no single "correct"
// behaviour to copy. Sidecar is the option that cannot lose data either way,
// and it follows Caprice Forever, whose source redirects writes for formats it
// will not overwrite ("// IPF and RAW should not be overwritten").
enum DiskWriteMode { DISK_WRITE_SIDECAR = 0, DISK_WRITE_DISABLED, DISK_WRITE_OVERWRITE };
static DiskWriteMode disk_write_mode_ = DISK_WRITE_SIDECAR;

// Emulated write-protect tab, i.e. what the CPC itself sees. Distinct from the
// above, which only decides whether changes reach the host filesystem. "auto"
// mirrors the image file's own permissions, which is what Hatari, 1984, CPCemu
// and Caprice Forever all do.
enum DiskProtectMode { DISK_PROTECT_AUTO = 0, DISK_PROTECT_ON, DISK_PROTECT_OFF };
static DiskProtectMode disk_protect_mode_ = DISK_PROTECT_AUTO;
static bool drive_b_enabled_ = false;
static bool playcity_enabled_ = false;
static std::string last_applied_model_;
// -1 = "auto" (use the per-model default in ApplyMachineType); otherwise a
// CRTC::TypeCRTC value forced by the user.
static int crtc_type_option_ = -1;

// Picks the launch command for freshly-loaded media. drive_number == -1
// means this was a tape, not a disk: the cassette firmware's RUN" with no
// filename loads the next tape file, which is the correct command there.
// For disks the command comes from the engine's own catalogue-driven
// detection (FDC::GetAutorun -> DiskGen::GetAutorun), not guessed here.
static void HandleAutorunForLoadedItem(int load_ok, int drive_number)
{
   if (!autorun_enabled_ || load_ok != 0)
      return;
   if (drive_number > 0)
      return; // B: is a second disc, not the one we boot from
   if (drive_number < 0)
   {
      // Tape. |TAPE first: every model now carries AMSDOS in ROM slot 7, so a
      // bare RUN" is routed to the disc, which answers "Bad command" when
      // there is no disc in the drive. Loading a tape broke the moment AMSDOS
      // was wired up, because before that RUN" fell through to the cassette.
      ArmAutorun("|TAPE\rRUN\"\r");
   }
   else if (emulator_ != nullptr)
   {
      char auto_file[16] = { 0 };
      char command[64];
      switch (emulator_->GetFDC()->GetAutorun((unsigned)drive_number, auto_file, sizeof(auto_file)))
      {
      case IDisk::AUTO_CPM:
         ArmAutorun("|CPM\r");
         break;
      case IDisk::AUTO_FILE:
         snprintf(command, sizeof(command), "RUN\"%s\r", auto_file);
         ArmAutorun(command);
         break;
      default:
         // AUTO_UNKNOWN: the engine's rule table didn't recognise this disc.
         // CAT at least puts its real catalogue on screen so the user can
         // type the right RUN" themselves, instead of leaving them at a bare
         // Ready prompt (or, worse, AMSDOS's "Bad command" from a RUN" with
         // no filename).
         ArmAutorun("CAT\r");
         break;
      }
   }
   if (log_cb != nullptr && autorun_sequence_[0] != '\0')
      log_cb(RETRO_LOG_INFO, "Autorun: typing \"%s\".\n", autorun_sequence_);
}

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
   // Base (UK) layout until check_variables() reads the real option --
   // update_input() must never poll an empty table.
   ApplyKeyboardLayout("uk");

   emulator_ = new EmulatorEngine();
   emulator_->SetDirectories(&directories_);
   emulator_->SetConfigurationManager(&conf_manager_);
   emulator_->Init(&display_, nullptr);
   emulator_->SetNotifier(&fdc_notify_);

   struct retro_variable var = { "amstradcpc_model", nullptr };
   environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var);
   ApplyMachineType(var.value ? var.value : "6128");

   motherboard_ = emulator_->GetMotherboard();
   // EmulatorEngine::InitSound() (not Motherboard::GetPSG()->InitSound(),
   // which only sets a barely-used secondary field on the PSG -- see the
   // RetroSound comment above) is what actually wires SoundMixer::Init(),
   // the real audio path.
   emulator_->InitSound(&retro_sound_);
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
   info->geometry.base_width = display_.CropWidth();
   info->geometry.base_height = display_.CropHeight();

   info->geometry.max_width = FULL_WIDTH;
   info->geometry.max_height = FULL_HEIGHT;
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
      // Host keyboard layout. Only affects which CPC key a physical host
      // key presses -- see kKeyMapOverridesFR.
      { "sugarbox_keyboard_layout", "Host keyboard layout; uk|fr" },
      // "green" is the authentic GT65 monochrome monitor the CPC shipped
      // with alongside the CTM644 colour one; amber is a convenience.
      { "sugarbox_monitor", "Monitor; color|green|amber" },
      // "auto" uses the type the real machine shipped with (464/664 = 0,
      // 6128 = 1, Plus/GX4000 = 4); override when a demo needs another.
      { "sugarbox_crtc", "CRTC type; auto|0|1|2|3|4" },
      // Off by default: this overwrites the image file in place. Only EDSK
      // is saved -- see MaybeWriteBackDisk.
      { "sugarbox_disk_write", "Save disk changes (EDSK only); sidecar|disabled|overwrite" },
      { "sugarbox_disk_write_protect", "Disk write protection; auto|on|off" },
      // A real 6128 has one internal drive; B: is the second drive some
      // multi-disc software expects. Fed from the playlist's second entry,
      // the same convention the Amiga cores use for their extra drives.
      { "sugarbox_drive_b", "Second disk drive (B:) from playlist; disabled|enabled" },
      { "sugarbox_border", "Screen border; normal|full" },
      // Second AY pair + Z80 CTC on ports 0xF880-0xF8FF. Off by default: it
      // changes which RunFullSpeed() instantiation the engine dispatches to.
      { "sugarbox_playcity", "PlayCity expansion; disabled|enabled" },
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
   // joystick port on the base machine). Lightgun ("Magnum Light Phaser"/
   // GUNSTICK, real period-accurate CPC peripheral -- CRTC.cpp models the
   // raster-beam hit detection, see EmulatorEngine::GunSet in update_input
   // below) offered as an alternate device for the same port.
   static const struct retro_controller_description controllers[] = {
      { "Amstrad CPC Joystick", RETRO_DEVICE_JOYPAD },
      { "Amstrad CPC Lightgun", RETRO_DEVICE_LIGHTGUN },
   };

   static const struct retro_controller_info ports[] = {
      { controllers, 2 },
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
static int mouse_rel_x;
static int mouse_rel_y;

void retro_reset(void)
{
   // Was a no-op that only cleared two leftover sample-core cursor
   // variables, so the frontend's Reset did nothing to the emulated
   // machine at all. EmulatorEngine::Reset()/ResetPlus() are the real
   // ones; the Plus/GX4000 ASIC needs its own reset path.
   x_coord = 0;
   y_coord = 0;
   if (emulator_ == nullptr)
      return;
   if (last_applied_model_ == "plus6128" || last_applied_model_ == "gx4000")
      emulator_->ResetPlus();
   else
      emulator_->Reset();
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

   // Lightgun (gap #10) -- real, period-accurate CPC hardware ("Magnum
   // Light Phaser"/GUNSTICK): CRTC.cpp compares gun_x_/gun_y_ against the
   // live raster beam position (monitor_->x_, monitor_->y_*2) each tick,
   // exactly the standard lightgun emulation technique. gun_x_/gun_y_ are
   // in the SAME raw coordinate space RetroDisplay::GetVideoBuffer()/VSync()
   // already use (a 1024-wide internal buffer, row-doubled -- VSync crops
   // the WIDTHxHEIGHT frame RetroArch actually displays starting at
   // (OFFSET_X, OFFSET_Y) within it), so converting the frontend's
   // normalized on-screen gun position back to that space is just adding
   // the same offsets. SCREEN_X/Y are absolute positions in [-0x8000,
   // 0x7FFF] over the displayed frame; IS_OFFSCREEN reports a shot pointed
   // outside it (RELOAD gesture in most frontends).
   //
   // NOTE: unlike every other feature this session, this has NOT been
   // verified against real lightgun-aware CPC software (none was
   // available to test with) -- the coordinate math follows directly from
   // reading CRTC.cpp's own comparison and RetroDisplay's existing crop
   // offsets, but whether a shot lands correctly in an actual game is
   // unconfirmed.
   if (emulator_ != nullptr)
   {
      const bool gun_offscreen = input_state_cb(0, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN);
      if (gun_offscreen)
      {
         emulator_->GunSet(0, 0, 0);
      }
      else
      {
         const int16_t gun_x = input_state_cb(0, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X);
         const int16_t gun_y = input_state_cb(0, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y);
         const bool gun_trigger = input_state_cb(0, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER);
         const int displayed_x = ((int)gun_x + 0x8000) * WIDTH / 0x10000;
         const int displayed_y = ((int)gun_y + 0x8000) * HEIGHT / 0x10000;
         emulator_->GunSet(displayed_x + OFFSET_X, displayed_y + OFFSET_Y, gun_trigger ? 1 : 0);
      }
   }

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

   for (size_t i = 0; i < active_keymap_size_; ++i)
   {
      if (input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, active_keymap_[i].retrok))
         matrix[active_keymap_[i].line] &= ~(1 << active_keymap_[i].bit);
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
   {
      // ForceKeyboardState() writes the LIVE buffer (keyboard_lines_) the
      // PSG reads -- but Monitor.cpp's ValidateKeyboardMap(), called once
      // per VSync, unconditionally overwrites that live buffer FROM the
      // CACHED one (keyboard_lines_cached_), which nothing else here ever
      // touches (it only ever gets written by CharAction/SendScanCode,
      // both dead paths for this core -- see the kAutorunKeys comment
      // above). Since the CPC's own keyboard-scan interrupt is tied to
      // the same VSync, a live-buffer write races that reset: whether a
      // given frame's forced key survives long enough to be sampled
      // depends on exact cycle alignment between this call and VSync,
      // which is why autorun intermittently dropped characters (verified
      // by screenshotting real BASIC output: RUN" typed as "un", losing R
      // and the shift+2 quote combo, while U/N/Enter landed). Writing
      // into the cached buffer instead means ValidateKeyboardMap() carries
      // this frame's state into the live buffer at the next VSync and it
      // stays there until we write the next frame's state -- no race.
      memcpy(emulator_->GetKeyboardHandler()->GetKeyboardState(), matrix, 10);
   }

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

static void ApplyMachineType(const char* model)
{
   if (model == nullptr || last_applied_model_ == model)
      return;
   last_applied_model_ = model;

   MachineSettings::HardwareType hw;
   const char* lower_rom;
   const char* upper_rom;
   MachineSettings::RamCfg ram;
   CRTC::TypeCRTC default_crtc = CRTC::UM6845R;
   bool tape_plugged = true;
   bool fdc_plugged = true;
   const char* cartridge_file = nullptr; // non-null => Plus/GX4000-style cartridge boot

   if (!strcmp(model, "664"))
   {
      hw = MachineSettings::OLD_664;
      default_crtc = CRTC::HD6845S;
      lower_rom = "os664.rom";
      upper_rom = "basic664.rom";
      ram = MachineSettings::M64_K;
   }
   else if (!strcmp(model, "464"))
   {
      hw = MachineSettings::OLD_464;
      default_crtc = CRTC::HD6845S;
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
      default_crtc = CRTC::AMS40226;
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
      default_crtc = CRTC::AMS40226;
      lower_rom = "os6128.rom";
      upper_rom = "basic6128.rom";
      ram = MachineSettings::M128_K;
      cartridge_file = "system.cpr";
   }
   else // "6128", and the fallback for anything unrecognised
   {
      hw = MachineSettings::OLD_6128;
      default_crtc = CRTC::UM6845R;
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
   // Expansion ROM slot 7 = AMSDOS, the disk operating system ROM. Without
   // it the emulated machine has no disk firmware at all: RUN"/CAT fall
   // through to the built-in CASSETTE handler, which is exactly why every
   // disk test in this project showed "Press PLAY then any key:" instead of
   // loading -- the FDC accepted and parsed the image (the "FDC: ... OK (0)"
   // notifier fires, including for IPF/CAPS), but BASIC had no way to read
   // it. amsdos.rom was bundled in this repo from the start yet never wired
   // into a ROM slot. Slot 7 is where real 664/6128 hardware has it (and
   // where the DDI-1 add-on puts it on a 464), and MachineSettings only
   // populates upper_rom_[] from an INI we deliberately stub out, so
   // nothing else was ever going to set it.
   machine_settings_.SetUpperRom(7, "amsdos.rom");
   // CRTC type. Real CPCs shipped with several different CRTC chips whose
   // behavioural differences are software-visible (a lot of demos, and some
   // games, only run correctly on the type they were written for). This was
   // previously hardcoded to AMS40226 for every model, which is right only
   // for the Plus/GX4000: per SugarboxV2's own machine configs
   // (Sugarbox/CONF/CPC*.cfg, Type_CRTC=) a 464 and 664 are type 0
   // (HD6845S/UM6845), a 6128 is type 1 (UM6845R), and a 6128 Plus is
   // type 4. crtc_type_option_ lets the user override that per-model
   // default when a particular demo needs a different one.
   machine_settings_.SetCRTCType(crtc_type_option_ >= 0
      ? static_cast<CRTC::TypeCRTC>(crtc_type_option_)
      : default_crtc);
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
      // ChangeSettings() ran UpdateExternalDevices(), which cleared the
      // expansion list.
      ApplyPlayCity();
   }
}

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

   var.key = "sugarbox_keyboard_layout";
   var.value = nullptr;
   ApplyKeyboardLayout((environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) ? var.value : "uk");

   var.key = "sugarbox_crtc";
   var.value = nullptr;
   {
      const int previous_crtc = crtc_type_option_;
      crtc_type_option_ = -1;
      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value && strcmp(var.value, "auto") != 0)
         crtc_type_option_ = atoi(var.value);
      if (previous_crtc != crtc_type_option_)
      {
         // Force ApplyMachineType() to redo the settings even though the
         // model string itself has not changed.
         const std::string model = last_applied_model_;
         last_applied_model_.clear();
         ApplyMachineType(model.c_str());
      }
   }

   var.key = "sugarbox_disk_write";
   var.value = nullptr;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      disk_write_mode_ = (strcmp(var.value, "overwrite") == 0) ? DISK_WRITE_OVERWRITE
                       : (strcmp(var.value, "disabled") == 0) ? DISK_WRITE_DISABLED
                       : DISK_WRITE_SIDECAR;
   }

   var.key = "sugarbox_drive_b";
   var.value = nullptr;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      drive_b_enabled_ = (strcmp(var.value, "enabled") == 0);

   var.key = "sugarbox_disk_write_protect";
   var.value = nullptr;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      disk_protect_mode_ = (strcmp(var.value, "on") == 0) ? DISK_PROTECT_ON
                         : (strcmp(var.value, "off") == 0) ? DISK_PROTECT_OFF
                         : DISK_PROTECT_AUTO;
      ApplyDiskWriteProtect();
   }

   var.key = "sugarbox_playcity";
   var.value = nullptr;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      playcity_enabled_ = (strcmp(var.value, "enabled") == 0);
      ApplyPlayCity();
   }

   var.key = "sugarbox_border";
   var.value = nullptr;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      display_.SetFullBorder(strcmp(var.value, "full") == 0);

   var.key = "sugarbox_monitor";
   var.value = nullptr;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      display_.SetMonitorType(strcmp(var.value, "green") == 0 ? RetroDisplay::MONITOR_GREEN
                            : strcmp(var.value, "amber") == 0 ? RetroDisplay::MONITOR_AMBER
                            : RetroDisplay::MONITOR_COLOR);
   }

   float last = last_aspect;
   float last_rate = last_sample_rate;
   struct retro_system_av_info info;
   retro_get_system_av_info(&info);
   retro_sound_.SetSampleRate((unsigned)last_sample_rate);

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

   // Always drained from the main thread here, regardless of whether
   // SET_AUDIO_CALLBACK negotiation below succeeds -- see the RetroSound
   // comment for why this doesn't rely on audio_batch_cb() being
   // thread-safe.
   retro_sound_.DrainToLibretro();

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

// ---------------------------------------------------------------------------
// Writing a modified disk back to its file.
//
// What the engine can actually save, established by reading each format's
// SaveDisk():
//   EDSK, HFE, HFEv3, SCP, IPF -> real implementations
//   DSK (plain "MV - CPC"), RAW, CTRAW -> return NOT_IMPLEMENTED, write nothing
// Note this is the opposite way round from the obvious guess: the plain
// sector format is the one that cannot be written, while the flux formats
// can. Most .dsk files in the wild are actually EDSK ("EXTENDED CPC DSK
// File") regardless of their extension, which is why saving works for them.
//
// Policy here: save EDSK only. The flux formats are preservation dumps and
// rewriting one to persist a high score is not a trade worth making, even
// though the engine would do it. Plain DSK/RAW/CTRAW cannot be saved at all
// and are reported rather than silently dropped.
//
// The rename dance works around a real CPCCoreEmu bug: IDisk::SmartOpen()
// compares the target's extension with strcmp() on Linux (stricmp on
// Windows), so a file called "game.dsk" does not match the format's ".DSK"
// and the writer appends instead of overwriting -- producing "game.dsk.DSK"
// and leaving the original untouched. Saving and then renaming over the
// original keeps the user's filename and is atomic on the same filesystem.
// Multi-copy ("weak") sector detection, parsed straight from the EDSK file.
//
// EDSK records a protected sector by storing several recorded copies of it
// back to back, so its actual data length in the sector-info table exceeds
// the length its size code N implies. FormatTypeEDSK::SaveDisk() writes a
// single pass over sides/tracks/sectors with no revolution loop, while the
// loader tracks nb_recorded_revolutions/GetNbRevolutions() -- so saving a
// protected disc silently flattens exactly the data the protection depends
// on. Every emulator surveyed degrades protected EDSKs in some way (Arnold
// overwrites one random copy, JavaCPC corrupts past 29 sectors/track,
// ACE-DL's own strings warn "EDSK is NOT a proper dump format"); refusing is
// the honest option until sidecar writes exist.
//
// Layout: 0x00 signature, 0x30 track count, 0x31 side count, 0x34 per-track
// size table (in 256-byte units, 0 = unformatted). Each track: "Track-Info",
// +0x14 size code N, +0x15 sector count, +0x18 sector list of 8 bytes
// (C,H,R,N,ST1,ST2,actual-length-LE16).
static bool EdskHasWeakSectors(const std::string& path)
{
   FILE* f = fopen(path.c_str(), "rb");
   if (f == nullptr)
      return false;
   fseek(f, 0, SEEK_END);
   const long size = ftell(f);
   rewind(f);
   if (size <= 0x100)
   {
      fclose(f);
      return false;
   }
   std::vector<unsigned char> d((size_t)size);
   const size_t got = fread(d.data(), 1, d.size(), f);
   fclose(f);
   if (got != d.size() || memcmp(d.data(), "EXTENDED", 8) != 0)
      return false;

   const unsigned tracks = d[0x30];
   const unsigned sides = d[0x31];
   const size_t count = (size_t)tracks * (size_t)sides;
   if (0x34 + count > d.size())
      return false;

   size_t off = 0x100;
   for (size_t t = 0; t < count; ++t)
   {
      const size_t track_size = (size_t)d[0x34 + t] * 256;
      if (track_size == 0)
         continue; // unformatted
      if (off + 0x18 > d.size())
         break;
      if (memcmp(&d[off], "Track-Info", 10) == 0)
      {
         const unsigned nb_sectors = d[off + 0x15];
         for (unsigned sct = 0; sct < nb_sectors; ++sct)
         {
            const size_t si = off + 0x18 + (size_t)sct * 8;
            if (si + 8 > d.size())
               break;
            const unsigned n = d[si + 3];
            const unsigned actual = (unsigned)d[si + 6] | ((unsigned)d[si + 7] << 8);
            if (n < 8 && actual > (128u << n))
            {
               // More recorded data than the size code allows = extra copies.
               return true;
            }
         }
      }
      off += track_size;
   }
   return false;
}

// Push the emulated write-protect tab down to the FDC. "auto" mirrors the
// image file's own permissions so a read-only file behaves like a
// write-protected disc -- what Hatari, 1984, CPCemu and Caprice Forever all
// do; CPCEC reaches the same result by opening "rb+" and falling back to "rb".
static void ApplyDiskWriteProtect()
{
   if (emulator_ == nullptr)
      return;
   bool protect = (disk_protect_mode_ == DISK_PROTECT_ON);
   if (disk_protect_mode_ == DISK_PROTECT_AUTO)
   {
      protect = false;
      if (current_disk_index_ < disk_images_.size() && !disk_images_[current_disk_index_].empty())
         protect = (access(disk_images_[current_disk_index_].c_str(), W_OK) != 0);
   }
   emulator_->GetFDC()->SetWriteProtection(protect, 0);
}

// Sidecar destination: <save dir>/<image name>.dsk, so the user's own image is
// never touched and ROM directories (often read-only) are not written into.
static std::string GetSidecarPath(const std::string& source)
{
   const char* dir = nullptr;
   if (!environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) || dir == nullptr || *dir == '\0')
      dir = "/tmp";
   const size_t slash = source.find_last_of("/\\");
   std::string name = (slash == std::string::npos) ? source : source.substr(slash + 1);
   const size_t dot = name.find_last_of('.');
   if (dot != std::string::npos)
      name.erase(dot);
   return std::string(dir) + "/" + name + ".dsk";
}

// RetroArch hands a .m3u straight to the core rather than expanding it (the
// core declares need_fullpath and lists m3u itself), so the playlist has to be
// parsed here -- this file advertised m3u support and silently loaded nothing
// at all before. Entries may be relative to the playlist, blank lines and
// #comments are skipped, and #EXTINF-style directives are ignored.
static std::vector<std::string> ParseM3u(const std::string& m3u_path)
{
   std::vector<std::string> entries;
   FILE* f = fopen(m3u_path.c_str(), "rb");
   if (f == nullptr)
      return entries;

   const size_t slash = m3u_path.find_last_of("/\\");
   const std::string base = (slash == std::string::npos) ? std::string() : m3u_path.substr(0, slash + 1);

   char line[1024];
   while (fgets(line, sizeof(line), f) != nullptr)
   {
      std::string entry(line);
      // strip CR/LF and surrounding blanks
      while (!entry.empty() && (entry.back() == '\n' || entry.back() == '\r' ||
                                entry.back() == ' '  || entry.back() == '\t'))
         entry.pop_back();
      size_t first = entry.find_first_not_of(" \t");
      if (first == std::string::npos)
         continue;
      entry = entry.substr(first);
      if (entry[0] == '#')
         continue;
      const bool absolute = (entry[0] == '/') || (entry.size() > 1 && entry[1] == ':');
      entries.push_back(absolute ? entry : base + entry);
   }
   fclose(f);
   return entries;
}

static bool IsM3uFile(const char* path) { return HasExtension(path, "m3u"); }

// PlayCity: a second AY pair plus a Z80 CTC on ports 0xF880-0xF8FF.
// CPCCoreEmu implements the whole board (PlayCity.cpp, Z84C30.cpp for the
// CTC, YMZ294.cpp for the sound chips, already mixed through SoundMixer) but
// never plugs it in: the registration is commented out in Motherboard.cpp and
// Machine.cpp because it called MachineSettings accessors that do not exist.
//
// CSig::PlugExpansionModule() is declared but has no implementation anywhere
// in the engine, so this uses exp_list_/nb_expansion_ directly -- exactly what
// the commented-out line in Motherboard.cpp does. The list is rebuilt from
// scratch rather than appended to, because EmulatorEngine::UpdateExternalDevices()
// resets nb_expansion_ to 0 on every ChangeSettings(), so this has to be
// re-applied after each model change and must not accumulate duplicates.
static void ApplyPlayCity()
{
   if (emulator_ == nullptr || emulator_->GetSig() == nullptr || motherboard_ == nullptr)
      return;
   CSig* sig = emulator_->GetSig();
   sig->nb_expansion_ = 0;
   if (playcity_enabled_)
      sig->exp_list_[sig->nb_expansion_++] = motherboard_->GetPlayCity();
}

static bool DiskFileIsEdsk(const std::string& path)
{
   FILE* f = fopen(path.c_str(), "rb");
   if (f == nullptr)
      return false;
   char magic[24] = { 0 };
   const size_t got = fread(magic, 1, sizeof(magic) - 1, f);
   fclose(f);
   return got >= 8 && strncmp(magic, "EXTENDED", 8) == 0;
}

static void MaybeWriteBackDisk(unsigned index)
{
   if (disk_write_mode_ == DISK_WRITE_DISABLED || emulator_ == nullptr)
      return;
   if (index >= disk_images_.size() || disk_images_[index].empty())
      return;
   if (!emulator_->GetFDC()->IsDiskModified(0))
      return;

   const std::string& path = disk_images_[index];
   const bool sidecar = (disk_write_mode_ == DISK_WRITE_SIDECAR);

   if (!DiskFileIsEdsk(path))
   {
      // Plain DSK ("MV - CPC"), RAW and CTRAW have no writer in the engine at
      // all (their SaveDisk() returns NOT_IMPLEMENTED), and the flux formats
      // that do -- HFE, SCP, IPF -- are preservation dumps we will not rewrite.
      // Either way the engine would save in the source format, so a sidecar
      // cannot help here: it would just put an unwritable or preservation
      // format somewhere else.
      if (log_cb != nullptr)
         log_cb(RETRO_LOG_WARN,
            "Disk '%s' was modified, but only EDSK images can be saved; "
            "changes discarded.\n", path.c_str());
      return;
   }

   if (EdskHasWeakSectors(path) && !sidecar)
   {
      if (log_cb != nullptr)
         log_cb(RETRO_LOG_WARN,
            "Disk '%s' was modified, but it contains multi-copy (weak) sectors and "
            "the EDSK writer stores only one copy -- overwriting would destroy the "
            "protection. Changes discarded; set Save disk changes to \"sidecar\" "
            "to keep them beside the original instead.\n", path.c_str());
      return;
   }

   // SmartOpen() appends ".DSK" unless the path already ends in exactly that
   // (its extension compare is case-sensitive on Linux), so this is where the
   // engine actually puts the file.
   const std::string written = (path.size() >= 4 && path.compare(path.size() - 4, 4, ".DSK") == 0)
      ? path : path + ".DSK";
   const std::string target = sidecar ? GetSidecarPath(path) : path;

   emulator_->SaveDisk(0);

   if (written != target && rename(written.c_str(), target.c_str()) != 0)
   {
      if (log_cb != nullptr)
         log_cb(RETRO_LOG_ERROR, "Disk '%s': saved to '%s' but could not move it to '%s'.\n",
            path.c_str(), written.c_str(), target.c_str());
      return;
   }
   if (log_cb != nullptr)
   {
      if (sidecar && EdskHasWeakSectors(path))
         log_cb(RETRO_LOG_WARN, "Disk '%s': changes saved to '%s'. The original had "
            "multi-copy (weak) sectors which the copy does not preserve.\n",
            path.c_str(), target.c_str());
      else
         log_cb(RETRO_LOG_INFO, "Disk '%s': changes saved to '%s'.\n", path.c_str(), target.c_str());
   }
}

static bool dc_set_eject_state(bool ejected)
{
   if (emulator_ == nullptr)
      return false;
   if (ejected)
   {
      MaybeWriteBackDisk(current_disk_index_);
      emulator_->Eject(0);
   }
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

// EXT-only additions (retro_disk_control_ext_callback, negotiated below via
// RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION -- cap32 does the
// same negotiation). Lets the frontend restore the last-active disk of an
// M3U across a session/save-state and show real filenames instead of just
// "Disk 1"/"Disk 2" in its multi-disk UI.
static bool dc_set_initial_image(unsigned index, const char* path)
{
   (void)path; // trust disk_images_ (already populated from the M3U/content
               // path), just record which entry was active.
   current_disk_index_ = index;
   return true;
}

static bool dc_get_image_path(unsigned index, char* path, size_t len)
{
   if (index >= disk_images_.size() || disk_images_[index].empty())
      return false;
   strncpy(path, disk_images_[index].c_str(), len);
   path[len - 1] = '\0';
   return true;
}

static bool dc_get_image_label(unsigned index, char* label, size_t len)
{
   if (index >= disk_images_.size() || disk_images_[index].empty())
      return false;
   const std::string& full = disk_images_[index];
   const size_t slash = full.find_last_of("/\\");
   const std::string basename = (slash == std::string::npos) ? full : full.substr(slash + 1);
   strncpy(label, basename.c_str(), len);
   label[len - 1] = '\0';
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

static struct retro_disk_control_ext_callback disk_control_ext_cb = {
   dc_set_eject_state,
   dc_get_eject_state,
   dc_get_image_index,
   dc_set_image_index,
   dc_get_num_images,
   dc_replace_image_index,
   dc_add_image_index,
   dc_set_initial_image,
   dc_get_image_path,
   dc_get_image_label,
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


   // Negotiate the newer EXT disk-control interface (adds set_initial_image/
   // get_image_path/get_image_label) if the frontend supports it, matching
   // cap32's own negotiation pattern; fall back to the base interface
   // (same underlying callbacks minus those three) otherwise.
   unsigned disk_control_version = 0;
   if (environ_cb(RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION, &disk_control_version) &&
       disk_control_version >= 1)
      environ_cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE, &disk_control_ext_cb);
   else
      environ_cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE, &disk_control_cb);

   // Publish the base RAM bank as a memory map as well as through
   // retro_get_memory_data(). RetroArch's cheat search, memory viewer and
   // READ_CORE_MEMORY all go through the MAP, not the plain accessor --
   // without this they report "no memory map defined" even when
   // retro_get_memory_data() is implemented.
   if (emulator_ != nullptr && emulator_->GetMem() != nullptr)
   {
      static struct retro_memory_descriptor mem_desc[1];
      memset(mem_desc, 0, sizeof(mem_desc));
      mem_desc[0].flags = RETRO_MEMDESC_SYSTEM_RAM;
      mem_desc[0].ptr = emulator_->GetMem()->GetRamBuffer();
      mem_desc[0].start = 0x0000;
      mem_desc[0].len = 64 * 1024;
      mem_desc[0].addrspace = "RAM";
      static struct retro_memory_map mem_map;
      mem_map.descriptors = mem_desc;
      mem_map.num_descriptors = 1;
      environ_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &mem_map);
   }
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
         // Autorun is armed from RetroFdcNotify::ItemLoaded() instead of
         // here -- that is where the engine reports the load actually
         // succeeded and, for disks, what command to type.
         emulator_->LoadTape(info->path);
      }
      else
      {
         // Disk: EmulatorEngine::LoadDisk() auto-detects the real format
         // from content and inserts it into a drive without needing a machine
         // reset -- inserting a floppy doesn't reboot a real CPC either.
         // A .m3u is a playlist, not an image: expand it so every entry shows
         // up in the disk-control list, and put the first one in A:.
         if (IsM3uFile(info->path))
         {
            disk_images_ = ParseM3u(info->path);
            if (disk_images_.empty())
            {
               if (log_cb != nullptr)
                  log_cb(RETRO_LOG_ERROR, "Playlist '%s' contains no usable entries.\n", info->path);
               return false;
            }
            if (log_cb != nullptr)
               log_cb(RETRO_LOG_INFO, "Playlist '%s': %u disc(s).\n",
                  info->path, (unsigned)disk_images_.size());
         }
         else
         {
            disk_images_.push_back(info->path);
         }

         // B: takes the playlist's second entry, matching the convention the
         // Amiga cores use for their additional drives. It is loaded FIRST and
         // non-deferred: EmulatorEngine::LoadDisk()'s third argument is the
         // FDC's "delayed" flag, and the FDC keeps only ONE pending deferred
         // load (delayed_load_drive_/delayed_load_filepath_), so issuing two
         // deferred loads back to back silently discards the first. Loading B
         // immediately and leaving A's deferred load to be set last keeps A's
         // existing, tested timing intact.
         if (drive_b_enabled_ && disk_images_.size() > 1)
            emulator_->LoadDisk(disk_images_[1].c_str(), 1, false);

         emulator_->LoadDisk(disk_images_[0].c_str(), 0);
      }
   }

   return true;
}

void retro_unload_game(void)
{
   // Last chance to persist: without this anything a game saved is lost.
   MaybeWriteBackDisk(current_disk_index_);
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
// The scratch file used to bounce a snapshot through EmulatorEngine's
// file-based API. This used to live in the system directory, which is
// routinely read-only on a real install (and is the wrong place for a
// temp file regardless) -- save states would simply fail there. Prefer the
// frontend's save directory, fall back to the system directory, then /tmp.
static std::string GetScratchSnapshotPath()
{
   const char* dir = nullptr;
   if (!environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) || dir == nullptr || *dir == '\0')
   {
      dir = directories_.GetBaseDirectory();
      if (dir == nullptr || *dir == '\0')
         dir = "/tmp";
   }
   return std::string(dir) + "/sugarbox_savestate.tmp.sna";
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

// Base 64K of RAM, via Memory::GetRamBuffer() (a public accessor -- an
// earlier reading of this class wrongly concluded the RAM was reachable
// only through private members). Exposing it lets RetroArch's cheat
// system and RetroAchievements see the machine's memory; both incumbent
// CPC cores are weak here. The CPC's RAM is bank-switched and a 6128 has
// 128K, so this is deliberately just the base bank -- the contiguous
// region tools expect -- not the expansion banks.
void *retro_get_memory_data(unsigned id)
{
   if (id != RETRO_MEMORY_SYSTEM_RAM || emulator_ == nullptr)
      return NULL;
   return emulator_->GetMem()->GetRamBuffer();
}

size_t retro_get_memory_size(unsigned id)
{
   if (id != RETRO_MEMORY_SYSTEM_RAM || emulator_ == nullptr)
      return 0;
   return 64 * 1024;
}

void retro_cheat_reset(void)
{}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   (void)index;
   (void)enabled;
   (void)code;
}
