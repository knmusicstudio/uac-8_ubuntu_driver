#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <math.h>
#include <pthread.h>
#include <gtk/gtk.h>
#include <jack/jack.h>
#include <libusb-1.0/libusb.h>

#define VENDOR_ID  0x1686
#define PRODUCT_ID 0xF02B

#define EP_AUDIO_OUT 0x01
#define EP_AUDIO_IN  0x82

#define NUM_PHYS_CHANNELS  32
#define PACKET_SIZE        1024
#define NUM_TRANSFERS      128

#define JACK_CHANNELS      18

static volatile int running = 0;
static volatile int active_transfers = 0;
static jack_client_t *jack_client = NULL;
static jack_port_t *output_ports[JACK_CHANNELS];
static libusb_device_handle *dev_handle = NULL;
static libusb_context *ctx = NULL;
static uint32_t current_sample_rate = 48000;
static int target_alt_setting = 3;

#define RING_BUFFER_SIZE (PACKET_SIZE * 16384)
static unsigned char ring_buffer[RING_BUFFER_SIZE];
static volatile int ring_head = 0;
static volatile int ring_tail = 0;

static pthread_t audio_thread;

// GUI ウィジェット参照
static GtkWidget *btn_start;
static GtkWidget *btn_stop;
static GtkWidget *lbl_status;

// メインスレッドで安全にUIを更新するための構造体
typedef struct {
    char *message;
    gboolean start_sensitive;
    gboolean stop_sensitive;
} UIUpdateData;

static gboolean update_ui_idle(gpointer user_data) {
    UIUpdateData *data = (UIUpdateData *)user_data;
    if (data) {
        if (data->message) {
            gtk_label_set_text(GTK_LABEL(lbl_status), data->message);
            free(data->message);
        }
        gtk_widget_set_sensitive(btn_start, data->start_sensitive);
        gtk_widget_set_sensitive(btn_stop, data->stop_sensitive);
        free(data);
    }
    return G_SOURCE_REMOVE;
}

static void post_ui_update(const char *msg, gboolean start_sens, gboolean stop_sens) {
    UIUpdateData *data = malloc(sizeof(UIUpdateData));
    if (data) {
        data->message = msg ? strdup(msg) : NULL;
        data->start_sensitive = start_sens;
        data->stop_sensitive = stop_sens;
        g_idle_add(update_ui_idle, data);
    }
}

static inline int get_ring_avail(void) {
    __sync_synchronize();
    int h = ring_head;
    int t = ring_tail;
    return (h - t + RING_BUFFER_SIZE) % RING_BUFFER_SIZE;
}

static void LIBUSB_CALL cb_out(struct libusb_transfer *transfer) {
    if (!running || transfer->status == LIBUSB_TRANSFER_CANCELLED) {
        __sync_fetch_and_sub(&active_transfers, 1);
        return;
    }

    if (transfer->status == LIBUSB_TRANSFER_STALL) {
        libusb_clear_halt(dev_handle, EP_AUDIO_OUT);
    } else if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
        int avail = get_ring_avail();
        if (avail >= PACKET_SIZE) {
            int tail = ring_tail;
            for (int i = 0; i < PACKET_SIZE; i++) {
                transfer->buffer[i] = ring_buffer[(tail + i) % RING_BUFFER_SIZE];
            }
            __sync_synchronize();
            ring_tail = (tail + PACKET_SIZE) % RING_BUFFER_SIZE;
        } else {
            memset(transfer->buffer, 0, PACKET_SIZE);
        }
    }

    if (running) {
        if (libusb_submit_transfer(transfer) != 0) {
            __sync_fetch_and_sub(&active_transfers, 1);
        }
    } else {
        __sync_fetch_and_sub(&active_transfers, 1);
    }
}

static void LIBUSB_CALL cb_in(struct libusb_transfer *transfer) {
    if (!running || transfer->status == LIBUSB_TRANSFER_CANCELLED) {
        __sync_fetch_and_sub(&active_transfers, 1);
        return;
    }

    if (transfer->status == LIBUSB_TRANSFER_STALL) {
        libusb_clear_halt(dev_handle, EP_AUDIO_IN);
    }

    if (running) {
        if (libusb_submit_transfer(transfer) != 0) {
            __sync_fetch_and_sub(&active_transfers, 1);
        }
    } else {
        __sync_fetch_and_sub(&active_transfers, 1);
    }
}

int process_callback(jack_nframes_t nframes, void *arg) {
    (void)arg;

    if (!running) return 0;

    jack_default_audio_sample_t *in_bufs[JACK_CHANNELS];
    for (int i = 0; i < JACK_CHANNELS; i++) {
        in_bufs[i] = (jack_default_audio_sample_t *)jack_port_get_buffer(output_ports[i], nframes);
    }

    int head = ring_head;

    for (jack_nframes_t s = 0; s < nframes; s++) {
        int32_t samples_32ch[NUM_PHYS_CHANNELS];
        memset(samples_32ch, 0, sizeof(samples_32ch));

        for (int ch = 0; ch < JACK_CHANNELS; ch++) {
            float sample = in_bufs[ch] ? in_bufs[ch][s] : 0.0f;

            if (sample > 1.0f) sample = 1.0f;
            if (sample < -1.0f) sample = -1.0f;
            int32_t val32 = (int32_t)(sample * 2147483647.0f);

            if (ch == 0) {
                samples_32ch[0]  = val32; // Main L (Ch 1)
                samples_32ch[2]  = val32; // Out 3
                samples_32ch[10] = val32; // 192k HP L
                samples_32ch[18] = val32; // 48k/96k HP L
            } else if (ch == 1) {
                samples_32ch[1]  = val32; // Main R (Ch 2)
                samples_32ch[3]  = val32; // Out 4
                samples_32ch[11] = val32; // 192k HP R
                samples_32ch[19] = val32; // 48k/96k HP R
            } else {
                samples_32ch[ch] = val32;
            }
        }

        for (int ch = 0; ch < NUM_PHYS_CHANNELS; ch++) {
            int32_t v = samples_32ch[ch];
            ring_buffer[head]                           = (uint8_t)(v & 0xFF);
            ring_buffer[(head + 1) % RING_BUFFER_SIZE] = (uint8_t)((v >> 8) & 0xFF);
            ring_buffer[(head + 2) % RING_BUFFER_SIZE] = (uint8_t)((v >> 16) & 0xFF);
            ring_buffer[(head + 3) % RING_BUFFER_SIZE] = (uint8_t)((v >> 24) & 0xFF);
            head = (head + 4) % RING_BUFFER_SIZE;
        }
    }

    __sync_synchronize();
    ring_head = head;
    return 0;
}

// オーディオスレッド本体
static void *audio_worker_thread(void *arg) {
    (void)arg;
    int r;
    jack_status_t status;

    // 1. JACK 接続
    jack_client = jack_client_open("uac8_jack", JackNullOption, &status);
    if (!jack_client) {
        post_ui_update("エラー: JACK (QjackCtl) が起動してへんで！", TRUE, FALSE);
        running = 0;
        return NULL;
    }

    uint32_t sample_rate = jack_get_sample_rate(jack_client);
    jack_nframes_t jack_buf_size = jack_get_buffer_size(jack_client);
    current_sample_rate = sample_rate;

    if (sample_rate >= 176400) {
        target_alt_setting = 1;
    } else if (sample_rate >= 88200) {
        target_alt_setting = 2;
    } else {
        target_alt_setting = 3;
    }

    // ステータスをUIに反映
    char status_msg[128];
    snprintf(status_msg, sizeof(status_msg), "稼働中: %u Hz / バッファ %d frames (Alt %d)", sample_rate, jack_buf_size, target_alt_setting);
    post_ui_update(status_msg, FALSE, TRUE);

    jack_set_process_callback(jack_client, process_callback, NULL);

    for (int i = 0; i < JACK_CHANNELS; i++) {
        char port_name[32];
        snprintf(port_name, sizeof(port_name), "playback_%d", i + 1);
        output_ports[i] = jack_port_register(jack_client, port_name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    }

    // 2. USB 初期化
    r = libusb_init(&ctx);
    if (r < 0) {
        post_ui_update("エラー: libusb の初期化に失敗", TRUE, FALSE);
        jack_client_close(jack_client);
        jack_client = NULL;
        running = 0;
        return NULL;
    }

    dev_handle = libusb_open_device_with_vid_pid(ctx, VENDOR_ID, PRODUCT_ID);
    if (!dev_handle) {
        post_ui_update("エラー: UAC-8 が見つからへん", TRUE, FALSE);
        libusb_exit(ctx);
        ctx = NULL;
        jack_client_close(jack_client);
        jack_client = NULL;
        running = 0;
        return NULL;
    }

    for (int iface = 0; iface < 4; iface++) {
        if (libusb_kernel_driver_active(dev_handle, iface) == 1) {
            libusb_detach_kernel_driver(dev_handle, iface);
        }
        libusb_claim_interface(dev_handle, iface);
    }

    libusb_set_interface_alt_setting(dev_handle, 1, 0);
    libusb_set_interface_alt_setting(dev_handle, 2, 0);
    usleep(20000);

    unsigned char clk_sel = 1;
    libusb_control_transfer(dev_handle, 0x21, 0x01, 0x0100, 0x2900, &clk_sel, 1, 1000);

    unsigned char rate_data[4];
    rate_data[0] = (sample_rate >> 0) & 0xFF;
    rate_data[1] = (sample_rate >> 8) & 0xFF;
    rate_data[2] = (sample_rate >> 16) & 0xFF;
    rate_data[3] = (sample_rate >> 24) & 0xFF;
    libusb_control_transfer(dev_handle, 0x21, 0x01, 0x0100, 0x2500, rate_data, 4, 1000);
    libusb_control_transfer(dev_handle, 0x21, 0x01, 0x0100, 0x2800, rate_data, 4, 1000);

    unsigned char unmute_val[2] = {0x00, 0x00};
    libusb_control_transfer(dev_handle, 0x21, 0x01, 0x0100, 0x2900, unmute_val, 2, 1000);
    libusb_control_transfer(dev_handle, 0x21, 0x01, 0x0200, 0x2900, unmute_val, 2, 1000);

    unsigned char vol_val[2] = {0x00, 0x00};
    libusb_control_transfer(dev_handle, 0x21, 0x01, 0x0201, 0x2900, vol_val, 2, 1000);
    libusb_control_transfer(dev_handle, 0x21, 0x01, 0x0202, 0x2900, vol_val, 2, 1000);

    libusb_control_transfer(dev_handle, 0x01, 0x0B, target_alt_setting, 0x0001, NULL, 0, 1000);
    libusb_control_transfer(dev_handle, 0x01, 0x0B, target_alt_setting, 0x0002, NULL, 0, 1000);
    libusb_control_transfer(dev_handle, 0x02, 0x01, 0x0000, 0x0001, NULL, 0, 1000);
    libusb_control_transfer(dev_handle, 0x02, 0x01, 0x0000, 0x0082, NULL, 0, 1000);
    libusb_control_transfer(dev_handle, 0x01, 0x0B, target_alt_setting, 0x0001, NULL, 0, 1000);
    libusb_control_transfer(dev_handle, 0x01, 0x0B, target_alt_setting, 0x0002, NULL, 0, 1000);

    libusb_set_interface_alt_setting(dev_handle, 1, target_alt_setting);
    libusb_set_interface_alt_setting(dev_handle, 2, target_alt_setting);

    // 3. データ転送開始
    struct libusb_transfer *out_transfers[NUM_TRANSFERS];
    struct libusb_transfer *in_transfers[NUM_TRANSFERS];
    unsigned char *out_buffers[NUM_TRANSFERS];
    unsigned char *in_buffers[NUM_TRANSFERS];

    active_transfers = NUM_TRANSFERS * 2;

    for (int i = 0; i < NUM_TRANSFERS; i++) {
        in_buffers[i] = calloc(1, PACKET_SIZE);
        in_transfers[i] = libusb_alloc_transfer(0);
        libusb_fill_bulk_transfer(in_transfers[i], dev_handle, EP_AUDIO_IN, in_buffers[i], PACKET_SIZE, cb_in, NULL, 1000);
        libusb_submit_transfer(in_transfers[i]);
    }

    jack_activate(jack_client);

    while (get_ring_avail() < (PACKET_SIZE * 64) && running) {
        usleep(1000);
    }

    for (int i = 0; i < NUM_TRANSFERS; i++) {
        out_buffers[i] = calloc(1, PACKET_SIZE);
        out_transfers[i] = libusb_alloc_transfer(0);
        libusb_fill_bulk_transfer(out_transfers[i], dev_handle, EP_AUDIO_OUT, out_buffers[i], PACKET_SIZE, cb_out, NULL, 1000);
        libusb_submit_transfer(out_transfers[i]);
    }

    while (running) {
        struct timeval tv = {0, 500};
        libusb_handle_events_timeout_completed(ctx, &tv, NULL);
    }

    // 停止処理
    if (jack_client) {
        jack_deactivate(jack_client);
        jack_client_close(jack_client);
        jack_client = NULL;
    }

    for (int i = 0; i < NUM_TRANSFERS; i++) {
        if (out_transfers[i]) libusb_cancel_transfer(out_transfers[i]);
        if (in_transfers[i])  libusb_cancel_transfer(in_transfers[i]);
    }

    int safety_timeout = 200;
    while (active_transfers > 0 && safety_timeout-- > 0) {
        struct timeval tv = {0, 10000};
        libusb_handle_events_timeout_completed(ctx, &tv, NULL);
    }

    for (int i = 0; i < NUM_TRANSFERS; i++) {
        if (out_transfers[i]) libusb_free_transfer(out_transfers[i]);
        if (in_transfers[i])  libusb_free_transfer(in_transfers[i]);
        free(out_buffers[i]);
        free(in_buffers[i]);
    }

    for (int iface = 0; iface < 4; iface++) {
        libusb_release_interface(dev_handle, iface);
    }

    if (dev_handle) {
        libusb_close(dev_handle);
        dev_handle = NULL;
    }
    if (ctx) {
        libusb_exit(ctx);
        ctx = NULL;
    }

    return NULL;
}

// 起動ボタンのコールバック
static void on_start_clicked(GtkWidget *widget, gpointer data) {
    (void)widget;
    (void)data;
    if (running) return;

    gtk_label_set_text(GTK_LABEL(lbl_status), "接続中...");
    gtk_widget_set_sensitive(btn_start, FALSE);
    gtk_widget_set_sensitive(btn_stop, TRUE);

    ring_head = 0;
    ring_tail = 0;
    memset(ring_buffer, 0, sizeof(ring_buffer));

    running = 1;
    pthread_create(&audio_thread, NULL, audio_worker_thread, NULL);
}

// 停止ボタンのコールバック
static void on_stop_clicked(GtkWidget *widget, gpointer data) {
    (void)widget;
    (void)data;
    if (!running) return;

    running = 0;
    pthread_join(audio_thread, NULL);

    gtk_label_set_text(GTK_LABEL(lbl_status), "停止中 (QjackCtl 起動後に「起動」を押してな)");
    gtk_widget_set_sensitive(btn_start, TRUE);
    gtk_widget_set_sensitive(btn_stop, FALSE);
}

static void on_window_destroy(GtkWidget *widget, gpointer data) {
    (void)widget;
    (void)data;
    if (running) {
        running = 0;
        pthread_join(audio_thread, NULL);
    }
    gtk_main_quit();
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "ZOOM UAC-8 ブリッジ");
    
    // ウィンドウサイズをゆったり（幅 450px, 高さ 180px）に変更
    gtk_window_set_default_size(GTK_WINDOW(window), 450, 180);
    gtk_container_set_border_width(GTK_CONTAINER(window), 20);
    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    // 1. ボタン配置（ボタン自体の高さも確保）
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_size_request(hbox, -1, 45);

    btn_start = gtk_button_new_with_label("起動");
    btn_stop = gtk_button_new_with_label("終了");
    gtk_widget_set_sensitive(btn_stop, FALSE);

    g_signal_connect(btn_start, "clicked", G_CALLBACK(on_start_clicked), NULL);
    g_signal_connect(btn_stop, "clicked", G_CALLBACK(on_stop_clicked), NULL);

    gtk_box_pack_start(GTK_BOX(hbox), btn_start, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), btn_stop, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    // 2. ステータス表示
    lbl_status = gtk_label_new("停止中 (QjackCtl 起動後に「起動」を押してな)");
    gtk_label_set_line_wrap(GTK_LABEL(lbl_status), TRUE);
    gtk_label_set_justify(GTK_LABEL(lbl_status), GTK_JUSTIFY_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), lbl_status, TRUE, TRUE, 0);

    gtk_widget_show_all(window);
    gtk_main();

    return 0;
}
