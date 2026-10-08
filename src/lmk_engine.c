

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/prctl.h>
#include <sys/swap.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sched.h>
#include <poll.h>
#include <math.h>

/* ================================================================
 *  VERSION
 * ================================================================ */
#define LMK_VERSION   "1.26"
#define LMK_AUTHOR    "P34"

/* ================================================================
 *  MEMORY PRESSURE THRESHOLDS
 * ================================================================ */
#define FREE_HIGH_PCT    25   /* stop killing above this % avail   */
#define FREE_LOW_PCT     15   /* start killing below this %        */
#define FREE_CRIT_PCT     8   /* aggressive kill tier              */
#define PIN_ALLOW_FLOOR_PCT 4

/* ================================================================
 *  ZRAM SETTINGS
 * ================================================================ */
#define ZRAM_SIZE_PCT    50

#define ZRAM_WARN_PCT    90   /* early-warning: proactive page drop    */
#define ZRAM_TRIM_PCT    93   /* pressure kill tier                    */
#define ZRAM_CRIT_PCT    98   /* try full cycle                        */

/* Minimum rate at which zram_pressure_kill() is called (seconds). */
#define ZRAM_KILL_INTVL_S    5

/* Minimum avail buffer (kB) above orig_data_size to attempt a cycle. */
#define ZRAM_CYCLE_MARGIN_KB  (200 * 1024)   /* 200 MB safety buffer   */

/* Maximum ZRAM pressure kills per invocation of zram_pressure_kill(). */
#define ZRAM_KILL_BATCH       6

/* ================================================================
 *  ZRAM STUCK / DEEP CLEAN
 * ================================================================ */
#define ZRAM_STUCK_PCT            96

#define ZRAM_STUCK_S              30
#define ZRAM_DEEPCLEAN_CD_S      180   /* min seconds between deep cleans */
#define ZRAM_COMPACT_WAIT_MS    1500   /* ms to wait after zram compact   */

#define ZRAM_CYCLE_RETRIES           8
#define ZRAM_CYCLE_RETRY_BACKOFF_US  150000

#define DEEPCLEAN_KILL_MAX         24   /* max procs killed per deep clean */
#define DEEPCLEAN_KILL_TARGET_PCT  25   /* stop once this % of ZRAM freed  */

/* Emergency kill: when ZRAM ≥ STUCK and all blocked by cooldown, a
 * second pass runs with this reduced cooldown floor (seconds). */
#define ZRAM_EMERG_CD_S           10
#define ZRAM_EMERG_KILL_MAX        2

#define BOUNCE_SUPPRESS_RC        20        /* rc ≥ this → suppress ZRAM kill */
#define BOUNCE_SUPPRESS_SWAP_KB   (150*1024) /* override: >150 MB swap = kill  */
#define BOUNCE_WINDOW_S           60        /* time window for bounce count    */
#define BOUNCE_WINDOW_KILLS       3         /* >3 kills in window → suppress   */
#define BOUNCE_DECAY_S            (24*60*60) /* 24h since last bounce → reset rc */

#define LOWFREQ_MIN_OBS_S         (24*60*60) /* don't judge until tracked 24h+  */
#define LOWFREQ_FG_RATE_THRESH    0.5        /* fg-credited ticks per hour tracked;
                                               * below this → background candidate */

/* ================================================================
 *  DEEP-CLEAN FUTILITY DETECTION
 * ================================================================ */
#define DEEPCLEAN_FUTILE_PCT       2    /* ≤2% recovered = futile            */
#define DEEPCLEAN_FUTILE_STRIKES   1
#define DEEPCLEAN_FUTILE_PAUSE_S  300

/* ================================================================
 *  ADAPTIVE CLEAN (tiered, trend-aware)
 * ================================================================ */
#define ADAPTIVE_INTVL_LOW_S      45   /* interval when ZRAM mildly elevated  */
#define ADAPTIVE_INTVL_MED_S      25   /* interval when ZRAM clearly rising   */
#define ADAPTIVE_INTVL_HIGH_S     12   /* interval under heavy pressure       */
#define ADAPTIVE_KILLS_LOW         3   /* max kills at low tier               */
#define ADAPTIVE_KILLS_MED         7   /* max kills at medium tier            */
#define ADAPTIVE_KILLS_HIGH       12   /* max kills at high tier              */
#define ADAPTIVE_TARGET_LOW_PCT   5    /* free 5% of ZRAM disksize            */
#define ADAPTIVE_TARGET_MED_PCT   10   /* free 10%                            */
#define ADAPTIVE_TARGET_HIGH_PCT  25   /* free 25%                            */
#define ADAPTIVE_TREND_WINDOW_S   60   /* seconds to measure ZRAM rise        */
#define ADAPTIVE_RISE_THRESH_PCT   4   /* pct rise over window = "rising"     */
#define ADAPTIVE_INTVL_MAINT_S   120   /* maintenance kill interval (normal)  */
#define ADAPTIVE_KILLS_MAINT       1   /* only 1 stale kill at maintenance    */

/* ================================================================
 *  RANK CACHE  (smarter ZRAM kill ordering across restarts)
 *  Saves avg_swap_kb + computed rank for all running apps every
 *  RANK_CACHE_SAVE_S seconds.  Loaded at startup to pre-populate
 *  avg_swap_kb in AppScore entries so cmp_killable_zram immediately
 *  targets the highest-swap apps even after a daemon restart.
 * ================================================================ */
#define RANK_CACHE_FILE  "/data/local/tmp/lmk_rank.cache"
#define RANK_CACHE_SAVE_S  10

/* ================================================================
 *  IDLE MODE  (in adaptive_clean)
 *  When the phone has had no foreground-app change for IDLE_DETECT_S
 *  seconds (screen likely off / phone sitting idle), run a gentle
 *  COLD+STALE sweep up to IDLE_KILLS_MAX kills every IDLE_INTVL_S.
 *  Only fires when RAM is not already under pressure (pressure tiers
 *  take precedence).
 * ================================================================ */
#define IDLE_DETECT_S     300   /* seconds of no fg change = idle      */
#define IDLE_INTVL_S      120
#define IDLE_KILLS_MAX    2
#define IDLE_MIN_ZRAM_FREE_PCT 10
/* Age-based kill gates within IDLE tier */
#define IDLE_KILL_HOT_AGE_S    (14*60)
#define IDLE_PROTECT_PCT        50

/* Intense idle clean: device screen-off ≥30 min — best opportunity to
 * sweep harder and run compaction since nothing is user-visible. */
#define IDLE_DEEP_S            1800  /* 30 min continuous screen-off       */
#define IDLE_DEEP_KILLS_MAX    8     /* generous kill budget                */
#define IDLE_DEEP_INTVL_S      300   /* run at most every 5 min             */
#define IDLE_DEEP_TARGET_PCT   70
#define IDLE_DEEP_HOT_AGE_S    60    /* relaxed age gate — already idle 30m */
#define IDLE_DEEP_PROTECT_PCT   20

#define LOG_RATELIMIT_S           30   /* noisy-state logs: once per 30 s    */

#define RANK_EXEMPT   0
#define RANK_TIER1    1   /* was FOREGROUND+HOT */
#define RANK_TIER2    2   /* was WARM+COLD */
#define RANK_TIER3    3   /* was STALE */
#define RANK_TIER4    4   /* was BACKGROUND */

#define RANK_FREQ_MED_SESSIONS  8
#define RANK_FREQ_HIGH_SESSIONS 20
#define RANK_HOT_S       60          /*  <60 s → TIER1                 */
#define RANK_WARM_S      (10*60)     /*  <10 min → old WARM boundary, still used
                                       *  standalone by the TIER2 MAINT-clause
                                       *  split (see adaptive_clean) */
#define RANK_COLD_S      (60*60)     /*  <1 hr → TIER2; else TIER3     */

#define RANK1_HOLD_S     3

#define TIER1_MAX_SLOTS   4      /* tier1 tier capacity */

#define TIER1_PIN_MAX     4      /* of TIER1_MAX_SLOTS, how many get a real pin */

#define TIER1_PIN_HOLD_S  RETAIN_FG_AGE_S

#define TIER1_DWELL_S     300    /* min time in tier1 before demotion to tier2 */
#define TIER2_MAX_SLOTS   6
#define TIER2_PIN_MAX     3
#define TIER2_DWELL_S     420    /* min time in tier2 before demotion to tier3
                                  * (widened 240->420 same request as
                                  * TIER1_DWELL_S 180->300 above) */
#define BG_IDLE_DEBOUNCE_S 30
#define UNTRUSTED_PROMO_DEBOUNCE_S 30
#define CAPACITY_PROMO_DEBOUNCE_S 30

#define BLIP_HEADLESS_MIN_S   600   /* 10 cumulative min of non-genuine ADJ_FOREGROUND */

#define FLAP_WINDOW_MIN_S    (30*60)  /* don't judge a rate until the window
                                        * has been open at least this long —
                                        * avoids false alarms on a burst of
                                        * legitimate early re-opens */
#define FLAP_RATE_WARN_THRESH  15.0   /* flaps/hour that triggers a warn */
#define FLAP_WINDOW_RESET_S  (4*60*60) /* rolling window reset period, also
                                         * re-arms the one-shot warn */

/* ================================================================
 *  FILE SWAP
 * ================================================================ */
#define SWAP_FILE "/data/lmk_swap"

/* ================================================================
 *  OOM ADJ BANDS
 * ================================================================ */
#define ADJ_FOREGROUND    0
#define ADJ_VISIBLE_MAX   200
#define ADJ_SERVICE_MAX   899
#define ADJ_CACHED_MAX    999
#define ADJ_UNKNOWN       1001

/* ================================================================
 *  FOREGROUND PROTECTION WINDOW (seconds)
 * ================================================================ */
#define FG_PROTECT_BASE   (30*60)
#define FG_PROTECT_MAX    (6*60*60)
/* Reduced windows under memory pressure */
#define FG_PROTECT_LOW_PCT  40  /* LOW state: % of computed window   */
#define FG_PROTECT_CRIT_S   20  /* CRITICAL state: flat floor (s)    */
#define MIN_BG_KILL_AGE_S    8

/* ================================================================
 *  APP RETENTION (oom pinning)
 * ================================================================ */

#define RETAIN_FG_AGE_S  900    /* only pin if used within 15 min    */
#define PIN_SETTLE_S     10
#define LEARN_RESET_GEN     7
#define LEARN_LOG_FILE     "/data/local/lmk_learn.log" /* legacy path, kept
                                  * only so score_load()'s gen-mismatch
                                  * archive step has something to rename —
                                  * the daemon no longer writes to this file
                                  * (AI_Swap learning model removed). */
#define RETAIN_MAX_N       6
#define RETAIN_PROTECT_GRACE_S 8
/* T3 critical-RAM override sparing — keep a couple of tier1-but-not-
 * true-fg apps (is_tier1_background(), was "rank-2 HOT") retained even
 * when overriding AI_Swap retention, so the user still has light
 * multitasking (e.g. switching back to the last app or two) instead of
 * every background app getting wiped in one critical sweep. */
#define T3_SPARE_HOT_N      2

/* ================================================================
 *  PROACTIVE LAUNCH TRIM
 * ================================================================ */
#define LAUNCH_TRIM_AVAIL_PCT     FREE_HIGH_PCT  /* only trim if avail below this   */
#define LAUNCH_TRIM_HEAVY_SWAP_KB (150 * 1024)   /* learned avg_swap_kb → "heavy"   */
#define LAUNCH_TRIM_TARGET_PCT    (FREE_HIGH_PCT - 3)
#define LAUNCH_TRIM_HEAVY_TARGET_PCT (FREE_HIGH_PCT + 7)
#define LAUNCH_TRIM_WARM_SKIP_S   90  /* skip RAM trim if relaunched within this   */

/* ================================================================
 *  SESSION TRACKING
 * ================================================================ */
#define SESSION_GAP_S     60   /* seconds of absence = new session  */

#define SESSION_DURATION_CAP_S     600
#define SESSION_DURATION_BONUS_MAX 150
#define MIN_GENUINE_SESSION_S      20

/* ================================================================
 *  RESTART RESILIENCE
 * ================================================================ */

#define RESTART_WINDOW_S    240
#define RESTART_CD_BASE_S   30
#define RESTART_CD_SCALE_S  20
#define RESTART_CD_MAX_S   180
#define BOUNCE_KILL_CD_MAX_S (30*60)

/* ================================================================
 *  KILL HISTORY / COOLDOWN
 * ================================================================ */
#define KILL_HIST_SIZE   32

/* ================================================================
 *  PROCESS PROTECTION LISTS (generic AOSP)
 * ================================================================ */
static const char * const NEVER_KILL[] = {
    "zygote","zygote64","system_server","surfaceflinger",
    "android.hardware","audioserver","cameraserver","installd","vold","netd",
    "logd","servicemanager","hwservicemanager","vndservicemanager",
    "lmk_engine","init","ueventd",
    "com.android.systemui",
    "android.process.media",
    "com.android.providers.media",
    "com.android.contacts",
    "com.android.calendar",
    "com.android.deskclock",
    NULL
};

static const char * const LAUNCHERS[] = {
    "com.android.launcher","com.android.launcher2","com.android.launcher3",
    "com.google.android.apps.nexuslauncher",
    "com.miui.home","com.huawei.android.launcher",
    "com.sec.android.app.launcher","com.teslacoilsw.launcher",
    "com.microsoft.launcher",
    NULL
};

/* Input method editors (keyboards) — must never be killed; losing the
 * active IME causes input field freezes / fallback-keyboard flicker. */

static const char * const SERVICE_EXEMPT[] = {
    "sandboxed_process",               /* webview/chrome sandboxed renderers */
    ":rcs",                            /* carrier RCS messaging service      */
    "googlequicksearchbox:interactor", /* GMS quicksearch interactor         */
    "com.truecaller",                  /* caller-ID/call-screening service;  */

                                        /* respawn cycles, rc 75->85, each    */
                                        /* kill freed 0-30MB RSS. OS re-wakes */
                                        /* it for call events regardless —   */
                                        /* pure battery/relaunch cost, no    */
                                        /* memory benefit.                   */
    NULL
};

static const char * const IME_PKGS[] = {
    "inputmethod.latin",               /* AOSP LatinIME / OpenBoard */
    "com.google.android.inputmethod",  /* Gboard */
    "com.touchtype.swiftkey",          /* SwiftKey */
    "com.samsung.android.honeyboard",
    "com.baidu.input",
    "com.sohu.inputmethod",
    "com.iflytek.inputmethod",
    "com.komoxo.babelkeyboard",
    NULL
};

static const char * const MEDIA_PLAYERS[] = {
    "spotify","music","youtube","player","audio","podcast","radio",
    "tidal","deezer","soundcloud","pandora","amazon","apple",
    "vlc","audioplayer","media","song","fm",
    NULL
};

/* ================================================================
 *  GLOBALS
 * ================================================================ */
static volatile bool g_running      = true;
static bool          g_zram_active  = false;
static char          g_zram_dev[128] = {0};
static char          g_zram_sys[128] = {0};
static long          g_total_ram_kb  = 0;
static time_t        g_last_kill_t   = 0;
static FILE         *g_log           = NULL;

static char          g_true_fg_name[256] = {0};
static char          g_pending_fg_name[256] = {0};
static int           g_pending_fg_streak = 0;
#define FG_DEBOUNCE_TICKS 2 /* candidate must persist this many ticks before accepted */

#define FG_MIN_HOLD_S 3
#define FG_HOLD_WOBBLE_TOLERANCE_S 2

static bool          g_zram_needs_resize = false;

/* Widget providers (dynamic, refreshed every 5 min) */
static char   g_wpkg[64][128];
static int    g_wpkg_count = 0;
static time_t g_wpkg_ts    = 0;

/* ================================================================
 *  KILL HISTORY (per-app cooldown + restart detection)
 * ================================================================ */
typedef struct {
    char   name[256];
    time_t killed_at;
    bool   bounced;
    bool   scheduled;
} KillRecord;
static KillRecord g_kill_hist[KILL_HIST_SIZE];
static int        g_kill_hist_idx = 0;

/* ================================================================
 *  FUTILITY TRACKING
 * ================================================================ */
static int    g_zram_pct_at_start = 0;
static time_t g_zram_kill_start_t = 0;
static time_t g_zram_pause_until  = 0;
#define FUTILITY_WINDOW_S   90
#define FUTILITY_PAUSE_S   120
#define FUTILITY_MIN_DROP    2

/* Rate-limit for zram_pressure_kill() */
static time_t g_zram_kill_last_t  = 0;

/* Rate-limit for oom_pin_retained() to suppress per-tick log spam */
static time_t g_retain_log_t      = 0;

/* Adaptive clean trend tracking */
static int    g_adaptive_prev_zram_pct = 0;
static time_t g_adaptive_trend_t       = 0;

/* Idle mode tracking — updated when true-foreground app changes */
static time_t g_last_fg_change_t = 0;
static time_t g_screen_off_since = 0;  /* 0 = screen currently on */

static int g_zram_used_pct = 0;

/* Rank cache save timestamp */
static time_t g_rank_cache_last_save = 0;

/* Boot widget-settle: kill paths are suppressed until this timestamp to give
 * AppWidgetService time to write appwidgets.xml after sys.boot_completed. */
static time_t g_widget_settle_until = 0;
#define WIDGET_SETTLE_S  45   /* seconds to wait after boot_completed */

/* Stuck-ZRAM tracking for deep clean */
static time_t g_zram_stuck_since  = 0;   /* when ZRAM first hit STUCK_PCT */
static time_t g_last_deepclean    = 0;   /* timestamp of last deep clean  */

#define RESET_GATE_S       (12*3600)  /* min time since boot/last reset  */
static time_t g_last_full_reset_t = 0;   /* 0 = not done yet this boot   */
static bool   g_reset_in_progress = false; /* tells check_bounced() this
                                             * kill was scheduled, not an
                                             * organic instability signal */

#define START_TIME_FILE  "/data/local/tmp/lmk_engine.start"

static time_t g_start_time           = 0;  /* daemon start epoch           */

/* Log rate-limit timestamps (prevent 4+ lines/s when ZRAM saturated) */
static time_t g_log_stuck_t          = 0;  /* last ZRAMstuck cd-active log */
static time_t g_log_crit_t           = 0;  /* last ZRAMcrit log            */
static time_t g_log_zramcd_t         = 0;  /* last ZRAMcd cooldown log     */

/* Deep-clean futility */
static int    g_deepclean_fail_cnt   = 0;  /* consecutive ≤2% cleans       */
static time_t g_deepclean_pause_until = 0; /* pause end timestamp          */

#define SCORE_FILE          "/data/local/lmk_scores.dat"

#define SCORE_JOURNAL_FILE          "/data/local/lmk_scores.journal"
#define SCORE_JOURNAL_CONSOLIDATE_N 400  /* consolidate after this many
                                           * journal-appended entries    */

#define CLASSIFY_IDLE_S        240  /* 4 min screen-off before classify starts */
#define SYSCHECK_QUEUE_MAX     128
#define SYSCHECK_DRAIN_PER_TICK  2   /* max dumpsys calls per idle tick   */
#define SYSCHECK_MAX_ATTEMPTS    4   /* give up + use fallback after this many
                                       * failed classify attempts, so a
                                       * genuinely un-lookupable name (a
                                       * process alias with no matching
                                       * installed package at any dot-level)
                                       * can't loop forever */
#define CATEGORY_RECONFIRM_S  (3L*24*3600)
#define SCORE_MAX_APPS      260
#define SCORE_MAX           1000   /* final score_compute() clamp only    */

#define FG_COUNT_MAX        100000
#define FG_COUNT_SCALE      100
#define SCORE_SAVE_INTVL    15
#define SCORE_AVG_ALPHA_PCT  25   /* EMA weight for new sample (25%)  */

#define GROWTH_WINDOW_S        600    /* sample window: 10 min             */
#define GROWTH_MIN_BASE_KB     51200  /* ignore
