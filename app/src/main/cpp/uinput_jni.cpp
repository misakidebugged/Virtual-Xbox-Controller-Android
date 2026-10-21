// Virtual Xbox 360 gamepad via Linux uinput.
// Must run with a UID that has SELinux permission to open /dev/uinput
// (typically shell via Shizuku, or root). Plain app UID will be denied.

#include <jni.h>
#include <android/log.h>
#include <linux/uinput.h>
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <atomic>
#include <thread>
#include <vector>
#include <string>
#include <algorithm>
#include <poll.h>

#define LOG_TAG "uinput_jni"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Gamepad profiles — VID/PID/name selected at create time
struct gamepad_profile {
    int id;
    uint16_t vid;
    uint16_t pid;
    const char* name;
    bool mouse_mode;
};

static const gamepad_profile PROFILES[] = {
    { 0, 0x045E, 0x028E, "Microsoft X-Box 360 pad",                                           false }, // XBOX_360
    { 1, 0x045E, 0x02EA, "Microsoft Xbox One Controller",                                     false }, // XBOX_ONE
    { 2, 0x054C, 0x05C4, "Sony Interactive Entertainment Wireless Controller",                false }, // DS4
    { 3, 0x054C, 0x0CE6, "Sony Interactive Entertainment DualSense Wireless Controller",      false }, // DualSense
    { 4, 0x046D, 0xC077, "Steam Controller Desktop",                                          true  }, // MOUSE
};

// Keyboard keys exposed in mouse mode. Order MUST match MouseKeyBit in MouseTarget.kt.
static const int MOUSE_KEYS[] = {
    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,        // 0..3
    KEY_ENTER, KEY_BACK, KEY_TAB, KEY_SPACE,      // 4..7
    KEY_HOME, KEY_ESC,                            // 8..9
    KEY_VOLUMEUP, KEY_VOLUMEDOWN,                 // 10..11
    KEY_PLAYPAUSE, KEY_MENU,                      // 12..13
    KEY_BACKSPACE,                                // 14
    KEY_SELECT,                                   // 15 → AKEYCODE_DPAD_CENTER, required to "click" on Leanback IME keys
};
static constexpr int MOUSE_KEY_COUNT = sizeof(MOUSE_KEYS) / sizeof(MOUSE_KEYS[0]);
// Bits 16,17,18 = BTN_LEFT, BTN_RIGHT, BTN_MIDDLE (handled separately).

static const gamepad_profile* find_profile(int id) {
    for (const auto& p : PROFILES) if (p.id == id) return &p;
    return &PROFILES[0];  // fallback Xbox 360
}

// Axis ranges matching real Xbox 360 reports
#define STICK_MIN   -32768
#define STICK_MAX    32767
#define TRIG_MIN     0
#define TRIG_MAX     255
#define HAT_MIN     -1
#define HAT_MAX      1

// Up to three uinput devices live in parallel:
//   - g_fd_gamepad: emulated controller (Xbox/PS profiles), opened R/W for FF rumble.
//   - g_fd_mouse:   pure mouse (EV_REL + 3 mouse buttons). Always present alongside
//                   the gamepad as a "sidecar" so the right trackpad can drive a
//                   real cursor while games still see a gamepad. In Desktop mode
//                   this IS the primary device.
//   - g_fd_kbd:     pure keyboard (EV_KEY). Sidecar in gamepad mode (enables
//                   keyboard-key mappings on back paddles etc.); primary in Desktop.
// A single device declaring EV_REL+EV_KEY together gets classified SOURCE_MOUSE
// by Android, which makes IMEs ignore its key events — splitting into separate
// fds mirrors how a real composite USB keyboard+mouse combo looks.
static int g_fd_gamepad = -1;
static int g_fd_mouse   = -1;
static int g_fd_kbd     = -1;

// Cached "last frame" state for delta encoding in sendMouseFrame.
// Reset on every destroy_devices() so a new device starts from a clean slate.
static int g_last_mouse_buttons = 0;
static int g_last_kbd_keys      = 0;

static int set_bit_or_log(int fd, unsigned long req, int bit, const char* what) {
    if (ioctl(fd, req, bit) < 0) {
        LOGE("ioctl %s bit=%d failed: %s", what, bit, strerror(errno));
        return -1;
    }
    return 0;
}

static int setup_abs(int fd, int code, int min, int max, int fuzz, int flat) {
    if (ioctl(fd, UI_SET_ABSBIT, code) < 0) return -1;
    struct uinput_abs_setup s;
    memset(&s, 0, sizeof(s));
    s.code = code;
    s.absinfo.minimum = min;
    s.absinfo.maximum = max;
    s.absinfo.fuzz    = fuzz;
    s.absinfo.flat    = flat;
    if (ioctl(fd, UI_ABS_SETUP, &s) < 0) {
        LOGE("UI_ABS_SETUP code=%d failed: %s", code, strerror(errno));
        return -1;
    }
    return 0;
}

static int write_event(int fd, uint16_t type, uint16_t code, int32_t value) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type  = type;
    ev.code  = code;
    ev.value = value;
    if (write(fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) {
        LOGE("write event t=%d c=%d v=%d failed: %s", type, code, value, strerror(errno));
        return -1;
    }
    return 0;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_steamcontroller_android_uinput_UInputNative_canOpen(JNIEnv*, jclass) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        LOGI("canOpen: /dev/uinput open denied: %s", strerror(errno));
        return JNI_FALSE;
    }
    close(fd);
    return JNI_TRUE;
}

// Slot for one stored FF effect — we only track FF_RUMBLE for now.
struct ff_slot {
    int id;
    uint16_t strong;  // left motor magnitude
    uint16_t weak;    // right motor magnitude
};
static constexpr int MAX_FF_EFFECTS = 4;
static ff_slot g_ff_effects[MAX_FF_EFFECTS] = {};

// Latest play command pulled by Kotlin via pollFFEvent().
// strong/weak are 0..65535. -1 magnitudes mean "no pending event".
static int32_t g_pending_strong = -1;
static int32_t g_pending_weak   = -1;

// Helper: finalise a uinput device with the given identity strings and create it.
static int finalize_device(int fd, uint16_t vid, uint16_t pid, const char* name, uint32_t ff_effects_max) {
    struct uinput_setup us;
    memset(&us, 0, sizeof(us));
    us.id.bustype = BUS_USB;
    us.id.vendor  = vid;
    us.id.product = pid;
    us.id.version = 0x0114;
    us.ff_effects_max = ff_effects_max;
    strncpy(us.name, name, UINPUT_MAX_NAME_SIZE - 1);
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0) {
        LOGE("UI_DEV_SETUP failed: %s", strerror(errno));
        return -1;
    }
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        LOGE("UI_DEV_CREATE failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

// Set up a pure mouse device (EV_REL + 3 mouse buttons). Returns fd or -1.
static int create_mouse_fd(uint16_t vid, uint16_t pid) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { LOGE("open /dev/uinput (mouse) failed: %s", strerror(errno)); return -1; }
    if (set_bit_or_log(fd, UI_SET_EVBIT,  EV_KEY,    "mouse EV_KEY")    < 0) goto fail;
    if (set_bit_or_log(fd, UI_SET_EVBIT,  EV_REL,    "mouse EV_REL")    < 0) goto fail;
    if (set_bit_or_log(fd, UI_SET_EVBIT,  EV_SYN,    "mouse EV_SYN")    < 0) goto fail;
    if (set_bit_or_log(fd, UI_SET_RELBIT, REL_X,     "REL_X")           < 0) goto fail;
    if (set_bit_or_log(fd, UI_SET_RELBIT, REL_Y,     "REL_Y")           < 0) goto fail;
    if (set_bit_or_log(fd, UI_SET_RELBIT, REL_WHEEL, "REL_WHEEL")       < 0) goto fail;
    if (set_bit_or_log(fd, UI_SET_KEYBIT, BTN_LEFT,   "BTN_LEFT")       < 0) goto fail;
    if (set_bit_or_log(fd, UI_SET_KEYBIT, BTN_RIGHT,  "BTN_RIGHT")      < 0) goto fail;
    if (set_bit_or_log(fd, UI_SET_KEYBIT, BTN_MIDDLE, "BTN_MIDDLE")     < 0) goto fail;
    if (finalize_device(fd, vid, pid, "Steam Controller Mouse", 0) < 0) goto fail;
    return fd;
fail:
    close(fd);
    return -1;
}

// Set up a pure keyboard device (EV_KEY only). Returns fd or -1.
//
// `full_alpha` declares the full A-Z/0-9 alphabet so Android's EventHub classifies
// this device as INPUT_DEVICE_CLASS_ALPHAKEY + DPAD (needed on Android TV so the
// Leanback IME routes DPAD navigation correctly — see comment below). ONLY pass
// true for the Desktop-mode keyboard. Passing true for the gamepad-mode sidecar
// makes Android believe a real hardware QWERTY keyboard is attached at all times,
// which suppresses the on-screen keyboard for every text field system-wide while
// the controller is connected — the gamepad already exposes its own ABS_HAT dpad,
// so the sidecar doesn't need this trick, only the small MOUSE_KEYS set used by
// back-paddle key mappings.
static int create_keyboard_fd(uint16_t vid, uint16_t pid, bool full_alpha) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { LOGE("open /dev/uinput (kbd) failed: %s", strerror(errno)); return -1; }
    if (set_bit_or_log(fd, UI_SET_EVBIT, EV_KEY, "kbd EV_KEY") < 0) goto fail;
    if (set_bit_or_log(fd, UI_SET_EVBIT, EV_SYN, "kbd EV_SYN") < 0) goto fail;
    for (int i = 0; i < MOUSE_KEY_COUNT; i++) {
        if (set_bit_or_log(fd, UI_SET_KEYBIT, MOUSE_KEYS[i], "MOUSE_KEY") < 0) goto fail;
    }
    if (full_alpha) {
        // Declare the full alphabet + digits so Android's EventHub classifies this
        // as INPUT_DEVICE_CLASS_ALPHAKEY (cheap test: KEY_Q present).
        //
        // Also declare KEY_SELECT (Linux 353) — Generic.kl maps it to DPAD_CENTER,
        // which is the missing 5th key needed for INPUT_DEVICE_CLASS_DPAD. Without
        // DPAD class the source is SOURCE_KEYBOARD only, and the Leanback IME on
        // Android TV source-filters DPAD navigation to SOURCE_DPAD — that's why
        // arrow presses worked outside the IME but not on the soft keyboard.
        const int alpha_keys[] = {
            KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J,
            KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T,
            KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
            KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
            KEY_LEFTSHIFT, KEY_RIGHTSHIFT, KEY_LEFTCTRL, KEY_LEFTALT, KEY_CAPSLOCK,
            KEY_COMMA, KEY_DOT, KEY_SLASH, KEY_SEMICOLON, KEY_APOSTROPHE,
            KEY_MINUS, KEY_EQUAL,
            KEY_SELECT,  // → AKEYCODE_DPAD_CENTER, completes DPAD classification
        };
        for (int k : alpha_keys) {
            if (set_bit_or_log(fd, UI_SET_KEYBIT, k, "alpha KEY") < 0) goto fail;
        }
    }
    // PID +1 keeps a stable, distinct identity vs the mouse half
    if (finalize_device(fd, vid, (uint16_t)(pid + 1), "Steam Controller Keyboard", 0) < 0) goto fail;
    return fd;
fail:
    close(fd);
    return -1;
}

static void destroy_devices() {
    bool destroyed = false;
    if (g_fd_gamepad >= 0) {
        ioctl(g_fd_gamepad, UI_DEV_DESTROY);
        close(g_fd_gamepad);
        g_fd_gamepad = -1;
        destroyed = true;
    }
    if (g_fd_mouse >= 0) {
        ioctl(g_fd_mouse, UI_DEV_DESTROY);
        close(g_fd_mouse);
        g_fd_mouse = -1;
        destroyed = true;
    }
    if (g_fd_kbd >= 0) {
        ioctl(g_fd_kbd, UI_DEV_DESTROY);
        close(g_fd_kbd);
        g_fd_kbd = -1;
        destroyed = true;
    }
    // Any cached delta-state belonged to the destroyed device — reset.
    g_last_mouse_buttons = 0;
    g_last_kbd_keys = 0;
    if (destroyed) {
        // Give the kernel + InputReader a moment to fully release the input
        // device records. Without this, rapid profile switching can hit transient
        // failures (UI_DEV_CREATE returns success but Android never sees the new
        // device, leaving the UI thinking it's healthy while no events flow).
        struct timespec ts = { 0, 80 * 1000 * 1000 };  // 80ms
        nanosleep(&ts, nullptr);
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_steamcontroller_android_uinput_UInputNative_createDevice(JNIEnv*, jclass, jint profileId) {
    const gamepad_profile* prof = find_profile(profileId);
    LOGI("createDevice: profile=%d (VID=0x%04X PID=0x%04X name=\"%s\")",
         prof->id, prof->vid, prof->pid, prof->name);

    destroy_devices();

    if (prof->mouse_mode) {
        // Desktop: just mouse + keyboard, no gamepad.
        int mouse_fd = create_mouse_fd(prof->vid, prof->pid);
        if (mouse_fd < 0) return JNI_FALSE;
        int kbd_fd = create_keyboard_fd(prof->vid, prof->pid, /*full_alpha=*/true);
        if (kbd_fd < 0) {
            ioctl(mouse_fd, UI_DEV_DESTROY);
            close(mouse_fd);
            return JNI_FALSE;
        }
        g_fd_mouse = mouse_fd;
        g_fd_kbd   = kbd_fd;
        LOGI("Desktop devices created — mouse fd=%d, kbd fd=%d", mouse_fd, kbd_fd);
        return JNI_TRUE;
    }

    // Gamepad path — gamepad device + sidecar mouse + sidecar keyboard.
    // The sidecar pair lets the right trackpad drive a cursor and lets back-paddle
    // mappings hit keyboard keys, while games still see a proper gamepad.
    int fd = open("/dev/uinput", O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        LOGE("open /dev/uinput failed: %s", strerror(errno));
        return JNI_FALSE;
    }

    if (set_bit_or_log(fd, UI_SET_EVBIT, EV_KEY, "EV_KEY") < 0) goto fail;
    if (set_bit_or_log(fd, UI_SET_EVBIT, EV_ABS, "EV_ABS") < 0) goto fail;
    if (set_bit_or_log(fd, UI_SET_EVBIT, EV_SYN, "EV_SYN") < 0) goto fail;
    if (set_bit_or_log(fd, UI_SET_EVBIT, EV_FF,  "EV_FF")  < 0) goto fail;

    if (set_bit_or_log(fd, UI_SET_FFBIT, FF_RUMBLE,   "FF_RUMBLE")   < 0) goto fail;
    if (set_bit_or_log(fd, UI_SET_FFBIT, FF_PERIODIC, "FF_PERIODIC") < 0) goto fail;

    {
        const int btns[] = {
            BTN_A, BTN_B, BTN_X, BTN_Y,
            BTN_TL, BTN_TR,
            BTN_SELECT, BTN_START, BTN_MODE,
            BTN_THUMBL, BTN_THUMBR
        };
        for (int b : btns) {
            if (set_bit_or_log(fd, UI_SET_KEYBIT, b, "KEY") < 0) goto fail;
        }
    }

    if (setup_abs(fd, ABS_X,        STICK_MIN, STICK_MAX, 16, 128) < 0) goto fail;
    if (setup_abs(fd, ABS_Y,        STICK_MIN, STICK_MAX, 16, 128) < 0) goto fail;
    if (setup_abs(fd, ABS_RX,       STICK_MIN, STICK_MAX, 16, 128) < 0) goto fail;
    if (setup_abs(fd, ABS_RY,       STICK_MIN, STICK_MAX, 16, 128) < 0) goto fail;
    if (setup_abs(fd, ABS_Z,        TRIG_MIN,  TRIG_MAX,   0,   0) < 0) goto fail;
    if (setup_abs(fd, ABS_RZ,       TRIG_MIN,  TRIG_MAX,   0,   0) < 0) goto fail;
    if (setup_abs(fd, ABS_HAT0X,    HAT_MIN,   HAT_MAX,    0,   0) < 0) goto fail;
    if (setup_abs(fd, ABS_HAT0Y,    HAT_MIN,   HAT_MAX,    0,   0) < 0) goto fail;

    if (finalize_device(fd, prof->vid, prof->pid, prof->name, MAX_FF_EFFECTS) < 0) goto fail;

    memset(g_ff_effects, 0, sizeof(g_ff_effects));
    g_pending_strong = -1;
    g_pending_weak   = -1;

    LOGI("Virtual gamepad created (profile=%d), fd=%d", prof->id, fd);
    g_fd_gamepad = fd;

    // Sidecar mouse + keyboard — non-fatal if either fails (gamepad still works).
    g_fd_mouse = create_mouse_fd(prof->vid, (uint16_t)(prof->pid + 0x100));
    if (g_fd_mouse < 0) {
        LOGE("Sidecar mouse creation failed — trackpad-as-cursor will be unavailable");
    }
    g_fd_kbd = create_keyboard_fd(prof->vid, (uint16_t)(prof->pid + 0x200), /*full_alpha=*/false);
    if (g_fd_kbd < 0) {
        LOGE("Sidecar keyboard creation failed — key mappings on back paddles will be unavailable");
    }
    LOGI("Gamepad devices ready — gamepad=%d, mouse=%d, kbd=%d", g_fd_gamepad, g_fd_mouse, g_fd_kbd);
    return JNI_TRUE;

fail:
    close(fd);
    return JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_steamcontroller_android_uinput_UInputNative_sendFrame(
        JNIEnv*, jclass,
        jint buttons,
        jint lx, jint ly, jint rx, jint ry,
        jint lt, jint rt,
        jint dpadX, jint dpadY) {
    if (g_fd_gamepad < 0) return;

    // Bit-to-keycode pairs — order must match XboxDescriptor.kt bit positions.
    // bit 0 = A, 1 = B, 2 = X, 3 = Y, 4 = LB, 5 = RB,
    // 6 = SELECT, 7 = START, 8 = MODE, 9 = THUMBL, 10 = THUMBR
    static const int bit_to_key[] = {
        BTN_A, BTN_B, BTN_X, BTN_Y,
        BTN_TL, BTN_TR,
        BTN_SELECT, BTN_START, BTN_MODE,
        BTN_THUMBL, BTN_THUMBR
    };
    const int n = sizeof(bit_to_key) / sizeof(bit_to_key[0]);
    for (int i = 0; i < n; i++) {
        int pressed = (buttons >> i) & 1;
        write_event(g_fd_gamepad, EV_KEY, bit_to_key[i], pressed);
    }

    write_event(g_fd_gamepad, EV_ABS, ABS_X,     lx);
    write_event(g_fd_gamepad, EV_ABS, ABS_Y,     ly);
    write_event(g_fd_gamepad, EV_ABS, ABS_RX,    rx);
    write_event(g_fd_gamepad, EV_ABS, ABS_RY,    ry);
    write_event(g_fd_gamepad, EV_ABS, ABS_Z,     lt);
    write_event(g_fd_gamepad, EV_ABS, ABS_RZ,    rt);
    write_event(g_fd_gamepad, EV_ABS, ABS_HAT0X, dpadX);
    write_event(g_fd_gamepad, EV_ABS, ABS_HAT0Y, dpadY);

    write_event(g_fd_gamepad, EV_SYN, SYN_REPORT, 0);
}

// Drain any pending events from the uinput fd. We care about:
//   - UI_FF_UPLOAD: a game uploads an effect. Read the ff_effect, store it by id.
//   - UI_FF_ERASE:  effect removed by the game. Free the slot.
//   - EV_FF code=effect_id value=N: play the effect N times (N=0 → stop).
// Returns: jintArray of size 2 [strongMagnitude, weakMagnitude] (each 0..65535),
//          or null if no rumble event is pending.
extern "C" JNIEXPORT jintArray JNICALL
Java_com_steamcontroller_android_uinput_UInputNative_pollFFEvent(JNIEnv* env, jclass) {
    if (g_fd_gamepad < 0) return nullptr;

    struct input_event ev;
    // Drain everything available — there may be multiple events in flight
    while (read(g_fd_gamepad, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        if (ev.type == EV_UINPUT && ev.code == UI_FF_UPLOAD) {
            struct uinput_ff_upload upload;
            memset(&upload, 0, sizeof(upload));
            upload.request_id = ev.value;
            if (ioctl(g_fd_gamepad, UI_BEGIN_FF_UPLOAD, &upload) >= 0) {
                if (upload.effect.type == FF_RUMBLE) {
                    // Find/use slot matching effect.id (or first free)
                    int slot = -1;
                    for (int i = 0; i < MAX_FF_EFFECTS; i++) {
                        if (g_ff_effects[i].id == upload.effect.id) { slot = i; break; }
                    }
                    if (slot < 0) {
                        for (int i = 0; i < MAX_FF_EFFECTS; i++) {
                            if (g_ff_effects[i].id == 0) { slot = i; break; }
                        }
                    }
                    if (slot >= 0) {
                        g_ff_effects[slot].id     = upload.effect.id;
                        g_ff_effects[slot].strong = upload.effect.u.rumble.strong_magnitude;
                        g_ff_effects[slot].weak   = upload.effect.u.rumble.weak_magnitude;
                    }
                }
                upload.retval = 0;
                ioctl(g_fd_gamepad, UI_END_FF_UPLOAD, &upload);
            }
        } else if (ev.type == EV_UINPUT && ev.code == UI_FF_ERASE) {
            struct uinput_ff_erase erase;
            memset(&erase, 0, sizeof(erase));
            erase.request_id = ev.value;
            if (ioctl(g_fd_gamepad, UI_BEGIN_FF_ERASE, &erase) >= 0) {
                for (int i = 0; i < MAX_FF_EFFECTS; i++) {
                    if (g_ff_effects[i].id == (int)erase.effect_id) {
                        g_ff_effects[i] = {};
                        break;
                    }
                }
                erase.retval = 0;
                ioctl(g_fd_gamepad, UI_END_FF_ERASE, &erase);
            }
        } else if (ev.type == EV_FF) {
            // Play or stop. ev.code = effect id, ev.value = play count (0 = stop)
            int effect_id = ev.code;
            if (ev.value == 0) {
                g_pending_strong = 0;
                g_pending_weak   = 0;
            } else {
                for (int i = 0; i < MAX_FF_EFFECTS; i++) {
                    if (g_ff_effects[i].id == effect_id) {
                        g_pending_strong = g_ff_effects[i].strong;
                        g_pending_weak   = g_ff_effects[i].weak;
                        break;
                    }
                }
            }
        }
    }

    if (g_pending_strong < 0) return nullptr;

    jintArray result = env->NewIntArray(2);
    jint vals[2] = { g_pending_strong, g_pending_weak };
    env->SetIntArrayRegion(result, 0, 2, vals);
    g_pending_strong = -1;
    g_pending_weak   = -1;
    return result;
}

// Mouse + keyboard frame. `relX`/`relY` are mouse motion deltas, `scrollY` is wheel ticks,
// `keys` is a bitmask: bits 0..MOUSE_KEY_COUNT-1 = MOUSE_KEYS entries (routed to the
// keyboard fd), bits 15/16/17 = BTN_LEFT/RIGHT/MIDDLE (routed to the mouse fd).
//
// Only writes EV_KEY events when the bit actually changes, and only writes SYN_REPORT
// on a fd that emitted at least one event this frame. This is required for IME focus:
// if the mouse fd reports every frame (300Hz), Android keeps the cursor "active" and
// routes DPAD events to it instead of the focused IME view.
extern "C" JNIEXPORT void JNICALL
Java_com_steamcontroller_android_uinput_UInputNative_sendMouseFrame(
        JNIEnv*, jclass, jint relX, jint relY, jint scrollY, jint keys) {
    // Bit 16/17/18 of `keys` = BTN_LEFT/RIGHT/MIDDLE; bits 0..MOUSE_KEY_COUNT-1 = keyboard keys.
    const int mouse_bits = keys & ((1 << 16) | (1 << 17) | (1 << 18));
    const int kbd_bits   = keys & ((1 << MOUSE_KEY_COUNT) - 1);

    // Mouse fd: motion + button edges
    if (g_fd_mouse >= 0) {
        bool any_event = false;
        const int btn_changed = mouse_bits ^ g_last_mouse_buttons;
        if (btn_changed & (1 << 16)) { write_event(g_fd_mouse, EV_KEY, BTN_LEFT,   (mouse_bits >> 16) & 1); any_event = true; }
        if (btn_changed & (1 << 17)) { write_event(g_fd_mouse, EV_KEY, BTN_RIGHT,  (mouse_bits >> 17) & 1); any_event = true; }
        if (btn_changed & (1 << 18)) { write_event(g_fd_mouse, EV_KEY, BTN_MIDDLE, (mouse_bits >> 18) & 1); any_event = true; }
        if (relX != 0)    { write_event(g_fd_mouse, EV_REL, REL_X,     relX);    any_event = true; }
        if (relY != 0)    { write_event(g_fd_mouse, EV_REL, REL_Y,     relY);    any_event = true; }
        if (scrollY != 0) { write_event(g_fd_mouse, EV_REL, REL_WHEEL, scrollY); any_event = true; }
        if (any_event) {
            write_event(g_fd_mouse, EV_SYN, SYN_REPORT, 0);
            g_last_mouse_buttons = mouse_bits;
        }
    }

    // Keyboard fd: only emit on bit change
    if (g_fd_kbd >= 0) {
        const int kbd_changed = kbd_bits ^ g_last_kbd_keys;
        if (kbd_changed != 0) {
            for (int i = 0; i < MOUSE_KEY_COUNT; i++) {
                if (kbd_changed & (1 << i)) {
                    write_event(g_fd_kbd, EV_KEY, MOUSE_KEYS[i], (kbd_bits >> i) & 1);
                }
            }
            write_event(g_fd_kbd, EV_SYN, SYN_REPORT, 0);
            g_last_kbd_keys = kbd_bits;
        }
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_steamcontroller_android_uinput_UInputNative_destroy(JNIEnv*, jclass) {
    destroy_devices();
    LOGI("Virtual devices destroyed");
}

// ─── Native Evdev Reader for Generic Bluetooth Controllers ───────────────────

static JavaVM* g_jvm = nullptr;
static jobject g_listener_ref = nullptr;
static std::atomic<bool> g_evdev_running{false};
static std::thread g_evdev_thread;
static std::string g_target_device_filter = "ANY";

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    g_jvm = vm;
    return JNI_VERSION_1_6;
}

static void stop_evdev_reader_internal() {
    g_evdev_running.store(false);
    if (g_evdev_thread.joinable()) {
        g_evdev_thread.join();
    }
    if (g_listener_ref && g_jvm) {
        JNIEnv* env = nullptr;
        if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK && env) {
            env->DeleteGlobalRef(g_listener_ref);
        }
        g_listener_ref = nullptr;
    }
}

static bool contains_ic(const std::string& haystack, const std::string& needle) {
    if (needle.empty() || needle == "ANY") return true;
    std::string h_clean = haystack;
    std::string n_clean = needle;
    h_clean.erase(std::remove(h_clean.begin(), h_clean.end(), ':'), h_clean.end());
    n_clean.erase(std::remove(n_clean.begin(), n_clean.end(), ':'), n_clean.end());

    auto it = std::search(
        h_clean.begin(), h_clean.end(),
        n_clean.begin(), n_clean.end(),
        [](char c1, char c2) {
            return std::tolower(static_cast<unsigned char>(c1)) ==
                   std::tolower(static_cast<unsigned char>(c2));
        }
    );
    if (it != h_clean.end()) return true;

    auto it_raw = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](char c1, char c2) {
            return std::tolower(static_cast<unsigned char>(c1)) ==
                   std::tolower(static_cast<unsigned char>(c2));
        }
    );
    return it_raw != haystack.end();
}

static int32_t scale_axis(int32_t val, int32_t srcMin, int32_t srcMax, int32_t dstMin, int32_t dstMax) {
    if (srcMax <= srcMin) return dstMin;
    float norm = (float)(val - srcMin) / (float)(srcMax - srcMin);
    norm = std::max(0.0f, std::min(1.0f, norm));
    return dstMin + (int32_t)(norm * (float)(dstMax - dstMin));
}

static void evdev_reader_thread_func() {
    JNIEnv* env = nullptr;
    if (!g_jvm || g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) {
        LOGE("evdev_reader: AttachCurrentThread failed");
        return;
    }

    jclass listenerClass = env->GetObjectClass(g_listener_ref);
    if (!listenerClass) {
        LOGE("evdev_reader: GetObjectClass failed");
        g_jvm->DetachCurrentThread();
        return;
    }
    jmethodID onFrameMethod = env->GetMethodID(listenerClass, "onInputFrame", "([B)V");
    if (!onFrameMethod) {
        LOGE("evdev_reader: GetMethodID onInputFrame failed");
        g_jvm->DetachCurrentThread();
        return;
    }

    struct EvdevFd {
        int fd;
        std::string path;
        std::string name;
        struct input_absinfo abs_x{};
        struct input_absinfo abs_y{};
        struct input_absinfo abs_rx{};
        struct input_absinfo abs_ry{};
        struct input_absinfo abs_z{};
        struct input_absinfo abs_rz{};
        struct input_absinfo abs_brake{};
        struct input_absinfo abs_gas{};
        bool has_rx{false};
        bool has_ry{false};
    };

    std::vector<EvdevFd> open_fds;

    uint32_t cur_buttons = 0;
    int16_t cur_lx = 0, cur_ly = 0, cur_rx = 0, cur_ry = 0;
    uint16_t cur_lt = 0, cur_rt = 0;
    uint8_t seq = 0;

    auto send_report = [&]() {
        uint8_t report[40] = {0};
        report[0] = 0x45; // REPORT_STATE
        report[1] = seq++;

        report[2] = (uint8_t)(cur_buttons & 0xFF);
        report[3] = (uint8_t)((cur_buttons >> 8) & 0xFF);
        report[4] = (uint8_t)((cur_buttons >> 16) & 0xFF);
        report[5] = (uint8_t)((cur_buttons >> 24) & 0xFF);

        report[6] = (uint8_t)(cur_lt & 0xFF);
        report[7] = (uint8_t)((cur_lt >> 8) & 0xFF);
        report[8] = (uint8_t)(cur_rt & 0xFF);
        report[9] = (uint8_t)((cur_rt >> 8) & 0xFF);

        report[10] = (uint8_t)(cur_lx & 0xFF);
        report[11] = (uint8_t)((cur_lx >> 8) & 0xFF);
        report[12] = (uint8_t)(cur_ly & 0xFF);
        report[13] = (uint8_t)((cur_ly >> 8) & 0xFF);
        report[14] = (uint8_t)(cur_rx & 0xFF);
        report[15] = (uint8_t)((cur_rx >> 8) & 0xFF);
        report[16] = (uint8_t)(cur_ry & 0xFF);
        report[17] = (uint8_t)((cur_ry >> 8) & 0xFF);

        jbyteArray jarr = env->NewByteArray(40);
        if (jarr) {
            env->SetByteArrayRegion(jarr, 0, 40, (const jbyte*)report);
            env->CallVoidMethod(g_listener_ref, onFrameMethod, jarr);
            env->DeleteLocalRef(jarr);
        }
    };

    auto rescan_devices = [&]() {
        for (auto it = open_fds.begin(); it != open_fds.end(); ) {
            if (it->fd < 0) {
                it = open_fds.erase(it);
            } else {
                ++it;
            }
        }

        DIR* dir = opendir("/dev/input");
        if (!dir) return;

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (strncmp(entry->d_name, "event", 5) != 0) continue;

            std::string path = std::string("/dev/input/") + entry->d_name;

            bool already_open = false;
            for (const auto& dev : open_fds) {
                if (dev.path == path) { already_open = true; break; }
            }
            if (already_open) continue;

            int fd = open(path.c_str(), O_RDWR | O_NONBLOCK);
            if (fd < 0) fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
            if (fd < 0) continue;

            char dev_name[256] = "Unknown";
            char dev_uniq[256] = "";
            ioctl(fd, EVIOCGNAME(sizeof(dev_name) - 1), dev_name);
            ioctl(fd, EVIOCGUNIQ(sizeof(dev_uniq) - 1), dev_uniq);

            std::string name_str(dev_name);
            std::string uniq_str(dev_uniq);

            if (name_str.find("Steam Controller") != std::string::npos ||
                name_str.find("Microsoft X-Box") != std::string::npos ||
                name_str.find("Sony Interactive") != std::string::npos ||
                name_str.find("uinput") != std::string::npos) {
                close(fd);
                continue;
            }

            uint8_t evtype_bitmask[EV_MAX / 8 + 1] = {0};
            if (ioctl(fd, EVIOCGBIT(0, sizeof(evtype_bitmask)), evtype_bitmask) < 0) {
                close(fd);
                continue;
            }
            bool has_key = (evtype_bitmask[EV_KEY / 8] & (1 << (EV_KEY % 8))) != 0;
            bool has_abs = (evtype_bitmask[EV_ABS / 8] & (1 << (EV_ABS % 8))) != 0;
            if (!has_key && !has_abs) {
                close(fd);
                continue;
            }

            if (g_target_device_filter != "ANY") {
                bool match_uniq = !uniq_str.empty() && contains_ic(uniq_str, g_target_device_filter);
                bool match_name = !name_str.empty() && contains_ic(name_str, g_target_device_filter);
                if (!match_uniq && !match_name) {
                    close(fd);
                    continue;
                }
            }

            if (ioctl(fd, EVIOCGRAB, 1) == 0) {
                LOGI("EVIOCGRAB success on %s (\"%s\") - input grabbed exclusively", path.c_str(), name_str.c_str());
            } else {
                LOGI("EVIOCGRAB non-fatal warning on %s (\"%s\"): %s", path.c_str(), name_str.c_str(), strerror(errno));
            }

            EvdevFd dev;
            dev.fd = fd;
            dev.path = path;
            dev.name = name_str;

            uint8_t abs_bitmask[ABS_MAX / 8 + 1] = {0};
            ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bitmask)), abs_bitmask);
            dev.has_rx = (abs_bitmask[ABS_RX / 8] & (1 << (ABS_RX % 8))) != 0;
            dev.has_ry = (abs_bitmask[ABS_RY / 8] & (1 << (ABS_RY % 8))) != 0;

            if (ioctl(fd, EVIOCGABS(ABS_X), &dev.abs_x) < 0) dev.abs_x = {0, -32768, 32767, 0, 0, 0};
            if (ioctl(fd, EVIOCGABS(ABS_Y), &dev.abs_y) < 0) dev.abs_y = {0, -32768, 32767, 0, 0, 0};
            if (ioctl(fd, EVIOCGABS(ABS_RX), &dev.abs_rx) < 0) dev.abs_rx = {0, -32768, 32767, 0, 0, 0};
            if (ioctl(fd, EVIOCGABS(ABS_RY), &dev.abs_ry) < 0) dev.abs_ry = {0, -32768, 32767, 0, 0, 0};
            if (ioctl(fd, EVIOCGABS(ABS_Z), &dev.abs_z) < 0) dev.abs_z = {0, 0, 255, 0, 0, 0};
            if (ioctl(fd, EVIOCGABS(ABS_RZ), &dev.abs_rz) < 0) dev.abs_rz = {0, 0, 255, 0, 0, 0};
            ioctl(fd, EVIOCGABS(ABS_BRAKE), &dev.abs_brake);
            ioctl(fd, EVIOCGABS(ABS_GAS), &dev.abs_gas);

            LOGI("evdev_reader opened: %s (\"%s\") fd=%d", path.c_str(), name_str.c_str(), fd);
            open_fds.push_back(dev);
            send_report();
        }
        closedir(dir);
    };

    rescan_devices();

    uint64_t last_rescan_ms = 0;
    uint64_t last_stream_ms = 0;

    while (g_evdev_running.load()) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now_ms = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

        if (open_fds.empty() || (now_ms - last_rescan_ms) > 2000) {
            rescan_devices();
            last_rescan_ms = now_ms;
        }

        if (open_fds.empty()) {
            struct timespec req = {0, 100 * 1000 * 1000};
            nanosleep(&req, nullptr);
            continue;
        }

        std::vector<struct pollfd> pfds;
        for (const auto& dev : open_fds) {
            struct pollfd pfd;
            pfd.fd = dev.fd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            pfds.push_back(pfd);
        }

        int poll_res = poll(pfds.data(), (nfds_t)pfds.size(), 10);
        bool state_changed = false;

        if (poll_res > 0) {
            for (size_t i = 0; i < pfds.size(); i++) {
                if (!(pfds[i].revents & POLLIN)) {
                    if (pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                        ioctl(open_fds[i].fd, EVIOCGRAB, 0);
                        close(open_fds[i].fd);
                        open_fds[i].fd = -1;
                    }
                    continue;
                }

                struct input_event evs[32];
                ssize_t rd = read(open_fds[i].fd, evs, sizeof(evs));
                if (rd < (ssize_t)sizeof(struct input_event)) {
                    if (rd < 0 && (errno == EAGAIN || errno == EINTR)) continue;
                    ioctl(open_fds[i].fd, EVIOCGRAB, 0);
                    close(open_fds[i].fd);
                    open_fds[i].fd = -1;
                    continue;
                }

                size_t count = rd / sizeof(struct input_event);
                auto& dev = open_fds[i];

                for (size_t k = 0; k < count; k++) {
                    const auto& ev = evs[k];

                    if (ev.type == EV_KEY) {
                        uint32_t mask = 0;
                        switch (ev.code) {
                            case BTN_SOUTH: case 288: mask = 0x00000001; break; // A / BTN_TRIGGER / BTN_SOUTH
                            case BTN_EAST:  case 289: mask = 0x00000002; break; // B / BTN_THUMB / BTN_EAST
                            case BTN_NORTH: case 290: case 306: mask = 0x00000004; break; // X / BTN_THUMB2 / BTN_C / BTN_NORTH
                            case BTN_WEST:  case 291: case 309: mask = 0x00000008; break; // Y / BTN_TOP / BTN_Z / BTN_WEST
                            case BTN_TL:    case 292: mask = 0x00080000; break; // LB / BTN_TOP2 / BTN_TL
                            case BTN_TR:    case 293: mask = 0x00000200; break; // RB / BTN_PINKIE / BTN_TR
                            case BTN_TL2:   case 294: mask = 0x08000000; break; // LT_FULL / BTN_BASE / BTN_TL2
                            case BTN_TR2:   case 295: mask = 0x00800000; break; // RT_FULL / BTN_BASE2 / BTN_TR2
                            case BTN_SELECT:case 296: case 158: mask = 0x00004000; break; // VIEW / BTN_BASE3 / BTN_SELECT / KEY_BACK
                            case BTN_START: case 297: case 28:  mask = 0x00000040; break; // MENU / BTN_BASE4 / BTN_START / KEY_ENTER
                            case BTN_MODE:  case 172: mask = 0x00010000; break; // STEAM / GUIDE / BTN_MODE / KEY_HOMEPAGE
                            case BTN_THUMBL:case 298: mask = 0x00008000; break; // LS / BTN_BASE5 / BTN_THUMBL
                            case BTN_THUMBR:case 299: mask = 0x00000020; break; // RS / BTN_BASE6 / BTN_THUMBR
                            case KEY_UP: case 544:    mask = 0x00002000; break; // DPAD_UP
                            case KEY_DOWN: case 545:  mask = 0x00000400; break; // DPAD_DOWN
                            case KEY_LEFT: case 546:  mask = 0x00001000; break; // DPAD_LEFT
                            case KEY_RIGHT: case 547: mask = 0x00000800; break; // DPAD_RIGHT
                            default: break;
                        }

                        if (mask != 0) {
                            if (ev.value) cur_buttons |= mask;
                            else cur_buttons &= ~mask;
                            state_changed = true;
                        }
                    } else if (ev.type == EV_ABS) {
                        switch (ev.code) {
                            case ABS_X:
                                cur_lx = (int16_t)scale_axis(ev.value, dev.abs_x.minimum, dev.abs_x.maximum, -32767, 32767);
                                state_changed = true;
                                break;
                            case ABS_Y: {
                                int32_t s = scale_axis(ev.value, dev.abs_y.minimum, dev.abs_y.maximum, -32767, 32767);
                                cur_ly = (int16_t)(-s);
                                state_changed = true;
                                break;
                            }
                            case ABS_RX:
                                cur_rx = (int16_t)scale_axis(ev.value, dev.abs_rx.minimum, dev.abs_rx.maximum, -32767, 32767);
                                state_changed = true;
                                break;
                            case ABS_RY: {
                                int32_t s = scale_axis(ev.value, dev.abs_ry.minimum, dev.abs_ry.maximum, -32767, 32767);
                                cur_ry = (int16_t)(-s);
                                state_changed = true;
                                break;
                            }
                            case ABS_Z:
                                if (!dev.has_rx) {
                                    cur_rx = (int16_t)scale_axis(ev.value, dev.abs_z.minimum, dev.abs_z.maximum, -32767, 32767);
                                } else {
                                    cur_lt = (uint16_t)scale_axis(ev.value, dev.abs_z.minimum, dev.abs_z.maximum, 0, 32767);
                                    if (cur_lt > 16384) cur_buttons |= 0x08000000; else cur_buttons &= ~0x08000000;
                                }
                                state_changed = true;
                                break;
                            case ABS_RZ:
                                if (!dev.has_ry) {
                                    int32_t s = scale_axis(ev.value, dev.abs_rz.minimum, dev.abs_rz.maximum, -32767, 32767);
                                    cur_ry = (int16_t)(-s);
                                } else {
                                    cur_rt = (uint16_t)scale_axis(ev.value, dev.abs_rz.minimum, dev.abs_rz.maximum, 0, 32767);
                                    if (cur_rt > 16384) cur_buttons |= 0x00800000; else cur_buttons &= ~0x00800000;
                                }
                                state_changed = true;
                                break;
                            case ABS_BRAKE:
                                cur_lt = (uint16_t)scale_axis(ev.value, dev.abs_brake.minimum, dev.abs_brake.maximum, 0, 32767);
                                if (cur_lt > 16384) cur_buttons |= 0x08000000; else cur_buttons &= ~0x08000000;
                                state_changed = true;
                                break;
                            case ABS_GAS:
                                cur_rt = (uint16_t)scale_axis(ev.value, dev.abs_gas.minimum, dev.abs_gas.maximum, 0, 32767);
                                if (cur_rt > 16384) cur_buttons |= 0x00800000; else cur_buttons &= ~0x00800000;
                                state_changed = true;
                                break;
                            case ABS_HAT0X:
                                if (ev.value < 0) {
                                    cur_buttons |= 0x00001000;  // DPAD_LEFT
                                    cur_buttons &= ~0x00000800; // DPAD_RIGHT
                                } else if (ev.value > 0) {
                                    cur_buttons |= 0x00000800;  // DPAD_RIGHT
                                    cur_buttons &= ~0x00001000; // DPAD_LEFT
                                } else {
                                    cur_buttons &= ~(0x00001000 | 0x00000800);
                                }
                                state_changed = true;
                                break;
                            case ABS_HAT0Y:
                                if (ev.value < 0) {
                                    cur_buttons |= 0x00002000;  // DPAD_UP
                                    cur_buttons &= ~0x00000400; // DPAD_DOWN
                                } else if (ev.value > 0) {
                                    cur_buttons |= 0x00000400;  // DPAD_DOWN
                                    cur_buttons &= ~0x00002000; // DPAD_UP
                                } else {
                                    cur_buttons &= ~(0x00002000 | 0x00000400);
                                }
                                state_changed = true;
                                break;
                            default: break;
                        } // switch ev.code
                    } // else if EV_ABS
                } // for k
            } // for i
        } // if poll_res > 0

        // Stream state every ~10ms (100Hz) when active or on state change
        if (state_changed || (now_ms - last_stream_ms) >= 10) {
            send_report();
            last_stream_ms = now_ms;
        }
    } // while g_evdev_running

    for (auto& dev : open_fds) {
        if (dev.fd >= 0) {
            ioctl(dev.fd, EVIOCGRAB, 0);
            close(dev.fd);
        }
    }
    open_fds.clear();

    g_jvm->DetachCurrentThread();
}

extern "C" JNIEXPORT void JNICALL
Java_com_steamcontroller_android_uinput_UInputNative_startEvdevReader(
        JNIEnv* env, jclass, jobject listener, jstring deviceFilter) {
    stop_evdev_reader_internal();

    if (!listener) return;
    g_listener_ref = env->NewGlobalRef(listener);
    if (deviceFilter) {
        const char* str = env->GetStringUTFChars(deviceFilter, nullptr);
        g_target_device_filter = str ? str : "ANY";
        if (str) env->ReleaseStringUTFChars(deviceFilter, str);
    } else {
        g_target_device_filter = "ANY";
    }

    g_evdev_running.store(true);
    g_evdev_thread = std::thread(evdev_reader_thread_func);
    LOGI("startEvdevReader started (filter=\"%s\")", g_target_device_filter.c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_steamcontroller_android_uinput_UInputNative_stopEvdevReader(JNIEnv*, jclass) {
    stop_evdev_reader_internal();
    LOGI("stopEvdevReader stopped");
}

