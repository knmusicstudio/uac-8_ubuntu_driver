#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/wait.h>
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

#define JACK_IN_CHANNELS   18
#define JACK_OUT_CHANNELS  20

#define PERIOD_COUNT       3

static volatile int running = 0;
static volatile int active_transfers = 0;
static jack_client_t *jack_client = NULL;
static jack_port_t *input_ports[JACK_IN_CHANNELS];   // Capture ports
static jack_port_t *output_ports[JACK_OUT_CHANNELS]; // Playback ports
static libusb_device_handle *dev_handle = NULL;
static libusb_context *ctx = NULL;
static volatile uint32_t current_sample_rate = 96000;
static volatile jack_nframes_t current_buffer_size = 512;
static int target_alt_setting = 2;

static int debug_mode = 1;
static pid_t spawned_jackd_pid = 0;

#define RING_BUFFER_SIZE (PACKET_SIZE * 32768)
#define RING_BUFFER_MASK (RING_BUFFER_SIZE - 1)

// Output ring buffer (JACK Playback -> USB OUT)
static unsigned char ring_buffer_out[RING_BUFFER_SIZE];
static volatile int ring_head_out = 0;
static volatile int ring_tail_out = 0;

// Input ring buffer (USB IN -> JACK Capture)
static unsigned char ring_buffer_in[RING_BUFFER_SIZE];
static volatile int ring_head_in = 0;
static volatile int ring_tail_in = 0;

static pthread_t audio_thread;
static pthread_t usb_event_thread;

// GUI widget references
static GtkWidget *btn_start;
static GtkWidget *btn_stop;
static GtkWidget *lbl_status;

// Structure for thread-safe UI updates
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

static inline int get_ring_avail_out(void) {
    __sync_synchronize();
    int h = ring_head_out;
    int t = ring_tail_out;
    return (h - t) & RING_BUFFER_MASK;
}

static inline int get_ring_avail_in(void) {
    __sync_synchronize();
    int h = ring_head_in;
    int t = ring_tail_in;
    return (h - t) & RING_BUFFER_MASK;
}

// JACK XRUN detection callback
static int xrun_callback(void *arg) {
    (void)arg;
    if (debug_mode) {
        fprintf(stderr, "\033[1;31m[DEBUG XRUN]\033[0m JACK XRUN occurred!\n");
    }
    return 0;
}

static int buffer_size_callback(jack_nframes_t nframes, void *arg) {
    (void)arg;
    current_buffer_size = nframes;
    if (debug_mode) {
        printf("[INFO] Buffer size changed: %u frames\n", nframes);
    }
    return 0;
}

static int sample_rate_callback(jack_nframes_t nframes, void *arg) {
    (void)arg;
    current_sample_rate = nframes;
    if (debug_mode) {
        printf("[INFO] Sample rate changed: %u Hz\n", nframes);
    }
    return 0;
}

static void jack_shutdown_callback(void *arg) {
    (void)arg;
    if (debug_mode) {
        printf("[WARN] JACK server shutdown detected\n");
    }
    jack_client = NULL;
    running = 0;
    post_ui_update("JACK stopped (Waiting for reconnection)", TRUE, FALSE);
}

static void LIBUSB_CALL cb_out(struct libusb_transfer *transfer) {
    if (!running || transfer->status == LIBUSB_TRANSFER_CANCELLED) {
        __sync_fetch_and_sub(&active_transfers, 1);
        return;
    }

    if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE ||
        transfer->status == LIBUSB_TRANSFER_ERROR) {
        if (running) {
            running = 0;
            post_ui_update("Error: USB device disconnected", TRUE, FALSE);
        }
        __sync_fetch_and_sub(&active_transfers, 1);
        return;
    }

    if (transfer->status == LIBUSB_TRANSFER_STALL) {
        if (debug_mode) {
            fprintf(stderr, "\033[1;33m[DEBUG USB]\033[0m OUT endpoint STALL\n");
        }
        libusb_clear_halt(dev_handle, EP_AUDIO_OUT);
    } else if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
        int avail = get_ring_avail_out();
        if (avail >= PACKET_SIZE) {
            int tail = ring_tail_out;
            int first_part = RING_BUFFER_SIZE - tail;
            if (first_part >= PACKET_SIZE) {
                memcpy(transfer->buffer, &ring_buffer_out[tail], PACKET_SIZE);
            } else {
                memcpy(transfer->buffer, &ring_buffer_out[tail], first_part);
                memcpy(transfer->buffer + first_part, ring_buffer_out, PACKET_SIZE - first_part);
            }
            __sync_synchronize();
            ring_tail_out = (tail + PACKET_SIZE) & RING_BUFFER_MASK;
        } else {
            // Buffer underrun on USB OUT
            if (debug_mode) {
                fprintf(stderr, "\033[1;35m[DEBUG OUT]\033[0m Underrun (USB OUT buffer starved: %d bytes left)\n", avail);
            }
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

    if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE ||
        transfer->status == LIBUSB_TRANSFER_ERROR) {
        if (running) {
            running = 0;
            post_ui_update("Error: USB device disconnected", TRUE, FALSE);
        }
        __sync_fetch_and_sub(&active_transfers, 1);
        return;
    }

    if (transfer->status == LIBUSB_TRANSFER_STALL) {
        if (debug_mode) {
            fprintf(stderr, "\033[1;33m[DEBUG USB]\033[0m IN endpoint STALL\n");
        }
        libusb_clear_halt(dev_handle, EP_AUDIO_IN);
    } else if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
        int avail = get_ring_avail_in();
        int free_bytes = (RING_BUFFER_SIZE - 1) - avail;

        if (free_bytes >= PACKET_SIZE) {
            int head = ring_head_in;
            int first_part = RING_BUFFER_SIZE - head;
            if (first_part >= PACKET_SIZE) {
                memcpy(&ring_buffer_in[head], transfer->buffer, PACKET_SIZE);
            } else {
                memcpy(&ring_buffer_in[head], transfer->buffer, first_part);
                memcpy(ring_buffer_in, transfer->buffer + first_part, PACKET_SIZE - first_part);
            }
            __sync_synchronize();
            ring_head_in = (head + PACKET_SIZE) & RING_BUFFER_MASK;
        } else {
            // Buffer overrun on USB IN
            if (debug_mode) {
                fprintf(stderr, "\033[1;35m[DEBUG IN]\033[0m Overrun (USB IN ring buffer overflow)\n");
            }
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

// Dedicated real-time thread for handling libusb events
static void *usb_event_worker_thread(void *arg) {
    (void)arg;
    struct sched_param param;
    param.sched_priority = 80;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);

    while (running) {
        struct timeval tv = {0, 2000};
        libusb_handle_events_timeout_completed(ctx, &tv, NULL);
    }
    return NULL;
}

int process_callback(jack_nframes_t nframes, void *arg) {
    (void)arg;
    if (!running) return 0;

    // 1. Acquire playback port buffers
    jack_default_audio_sample_t *play_bufs[JACK_OUT_CHANNELS];
    for (int i = 0; i < JACK_OUT_CHANNELS; i++) {
        play_bufs[i] = (jack_default_audio_sample_t *)jack_port_get_buffer(output_ports[i], nframes);
    }

    // 2. Acquire capture port buffers
    jack_default_audio_sample_t *rec_bufs[JACK_IN_CHANNELS];
    for (int i = 0; i < JACK_IN_CHANNELS; i++) {
        rec_bufs[i] = (jack_default_audio_sample_t *)jack_port_get_buffer(input_ports[i], nframes);
    }

    // --- Input Processing (UAC-8 USB -> JACK Capture) ---
    int in_avail = get_ring_avail_in();
    int frame_bytes = NUM_PHYS_CHANNELS * 4; // 1 frame = 128 bytes
    int target_cushion = (int)nframes * frame_bytes * PERIOD_COUNT;

    int tail_in = ring_tail_in;
    const float inv_scale = 1.0f / 2147483647.0f;
    static float prev_sample[NUM_PHYS_CHANNELS] = {0};

    // Clock drift compensation: skip a frame if buffer is overflowing
    if (in_avail > target_cushion + (frame_bytes * 32)) {
        tail_in = (tail_in + frame_bytes) & RING_BUFFER_MASK;
        in_avail -= frame_bytes;
    }

    for (jack_nframes_t s = 0; s < nframes; s++) {
        if (in_avail >= frame_bytes) {
            for (int ch = 0; ch < NUM_PHYS_CHANNELS; ch++) {
                int32_t val32 = *(int32_t *)&ring_buffer_in[tail_in];
                tail_in = (tail_in + 4) & RING_BUFFER_MASK;

                float sample_val = (float)val32 * inv_scale;
                prev_sample[ch] = sample_val;

                if (ch < JACK_IN_CHANNELS && rec_bufs[ch]) {
                    rec_bufs[ch][s] = sample_val;
                }
            }
            in_avail -= frame_bytes;
        } else {
            // Buffer starvation concealment: decay previous sample
            for (int ch = 0; ch < JACK_IN_CHANNELS; ch++) {
                if (rec_bufs[ch]) {
                    rec_bufs[ch][s] = prev_sample[ch] * 0.98f;
                }
            }
        }
    }

    __sync_synchronize();
    ring_tail_in = tail_in;

    // --- Output Processing (JACK Playback -> UAC-8 USB) ---
    int out_avail = get_ring_avail_out();
    int free_out_bytes = (RING_BUFFER_SIZE - 1) - out_avail;
    int required_out_bytes = nframes * NUM_PHYS_CHANNELS * 4;

    if (free_out_bytes >= required_out_bytes) {
        int head_out = ring_head_out;

        for (jack_nframes_t s = 0; s < nframes; s++) {
            int32_t samples_32ch[NUM_PHYS_CHANNELS];
            memset(samples_32ch, 0, sizeof(samples_32ch));

            for (int ch = 0; ch < JACK_OUT_CHANNELS; ch++) {
                float sample = play_bufs[ch] ? play_bufs[ch][s] : 0.0f;
                if (sample > 1.0f) sample = 1.0f;
                if (sample < -1.0f) sample = -1.0f;
                samples_32ch[ch] = (int32_t)(sample * 2147483647.0f);
            }

            // Mirror Main L/R to hardware outputs & headphone ports
            samples_32ch[2]  = samples_32ch[0]; // Out 3
            samples_32ch[10] = samples_32ch[0]; // 192k Headphone L
            samples_32ch[18] = samples_32ch[0]; // 48k/96k Headphone L

            samples_32ch[3]  = samples_32ch[1]; // Out 4
            samples_32ch[11] = samples_32ch[1]; // 192k Headphone R
            samples_32ch[19] = samples_32ch[1]; // 48k/96k Headphone R

            int first_part = RING_BUFFER_SIZE - head_out;
            int copy_bytes = sizeof(samples_32ch);
            if (first_part >= copy_bytes) {
                memcpy(&ring_buffer_out[head_out], samples_32ch, copy_bytes);
            } else {
                memcpy(&ring_buffer_out[head_out], samples_32ch, first_part);
                memcpy(ring_buffer_out, ((uint8_t *)samples_32ch) + first_part, copy_bytes - first_part);
            }
            head_out = (head_out + copy_bytes) & RING_BUFFER_MASK;
        }

        __sync_synchronize();
        ring_head_out = head_out;
    }

    return 0;
}

// Fallback: spawn jackd automatically if not running
static int start_jackd_auto(void) {
    if (debug_mode) {
        printf("[INFO] JACK server not found. Spawning jackd (96kHz / 512 / Period 3)...\n");
    }
    pid_t pid = fork();
    if (pid == 0) {
        int dev_null = open("/dev/null", O_WRONLY);
        if (dev_null >= 0) {
            dup2(dev_null, STDOUT_FILENO);
            dup2(dev_null, STDERR_FILENO);
            close(dev_null);
        }
        execlp("jackd", "jackd", "-d", "dummy", "-r", "96000", "-p", "512", "-n", "3", NULL);
        _exit(1);
    } else if (pid > 0) {
        spawned_jackd_pid = pid;
        sleep(1);
        return 0;
    }
    return -1;
}

// Audio initialization and transfer worker
static void *audio_worker_thread(void *arg) {
    (void)arg;
    int r;
    jack_status_t status;

    // 1. Connect to JACK server
    jack_client = jack_client_open("uac8_jack", JackNoStartServer, &status);
    if (!jack_client) {
        start_jackd_auto();
        jack_client = jack_client_open("uac8_jack", JackNullOption, &status);
        if (!jack_client) {
            post_ui_update("Error: Failed to connect to JACK", TRUE, FALSE);
            running = 0;
            pthread_detach(pthread_self());
            return NULL;
        }
    }

    current_sample_rate = jack_get_sample_rate(jack_client);
    current_buffer_size = jack_get_buffer_size(jack_client);

    if (current_sample_rate >= 176400) {
        target_alt_setting = 1;
    } else if (current_sample_rate >= 88200) {
        target_alt_setting = 2;
    } else {
        target_alt_setting = 3;
    }

    if (debug_mode) {
        printf("[INFO] Connected to JACK: %u Hz / Buffer: %d frames (Alt Setting: %d)\n",
               current_sample_rate, current_buffer_size, target_alt_setting);
    }

    char status_msg[128];
    snprintf(status_msg, sizeof(status_msg), "Running: %u Hz / %d frames (Alt %d)", 
             current_sample_rate, current_buffer_size, target_alt_setting);
    post_ui_update(status_msg, FALSE, TRUE);

    jack_on_shutdown(jack_client, jack_shutdown_callback, NULL);
    jack_set_buffer_size_callback(jack_client, buffer_size_callback, NULL);
    jack_set_sample_rate_callback(jack_client, sample_rate_callback, NULL);
    jack_set_xrun_callback(jack_client, xrun_callback, NULL);
    jack_set_process_callback(jack_client, process_callback, NULL);

    for (int i = 0; i < JACK_IN_CHANNELS; i++) {
        char port_name[32];
        snprintf(port_name, sizeof(port_name), "capture_%d", i + 1);
        input_ports[i] = jack_port_register(jack_client, port_name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    }

    for (int i = 0; i < JACK_OUT_CHANNELS; i++) {
        char port_name[32];
        snprintf(port_name, sizeof(port_name), "playback_%d", i + 1);
        output_ports[i] = jack_port_register(jack_client, port_name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    }

    // 2. Initialize USB via libusb
    r = libusb_init(&ctx);
    if (r < 0) {
        post_ui_update("Error: libusb_init failed", TRUE, FALSE);
        jack_client_close(jack_client);
        jack_client = NULL;
        running = 0;
        pthread_detach(pthread_self());
        return NULL;
    }

    dev_handle = libusb_open_device_with_vid_pid(ctx, VENDOR_ID, PRODUCT_ID);
    if (!dev_handle) {
        post_ui_update("Error: ZOOM UAC-8 not found", TRUE, FALSE);
        libusb_exit(ctx);
        ctx = NULL;
        jack_client_close(jack_client);
        jack_client = NULL;
        running = 0;
        pthread_detach(pthread_self());
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
    rate_data[0] = (current_sample_rate >> 0) & 0xFF;
    rate_data[1] = (current_sample_rate >> 8) & 0xFF;
    rate_data[2] = (current_sample_rate >> 16) & 0xFF;
    rate_data[3] = (current_sample_rate >> 24) & 0xFF;
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

    // 3. Setup bulk transfers
    struct libusb_transfer *out_transfers[NUM_TRANSFERS];
    struct libusb_transfer *in_transfers[NUM_TRANSFERS];
    unsigned char *out_buffers[NUM_TRANSFERS];
    unsigned char *in_buffers[NUM_TRANSFERS];

    active_transfers = NUM_TRANSFERS * 2;

    for (int i = 0; i < NUM_TRANSFERS; i++) {
        in_buffers[i] = calloc(1, PACKET_SIZE);
        in_transfers[i] = libusb_alloc_transfer(0);
        libusb_fill_bulk_transfer(in_transfers[i], dev_handle, EP_AUDIO_IN, in_buffers[i], PACKET_SIZE, cb_in, NULL, 0);
        libusb_submit_transfer(in_transfers[i]);
    }

    for (int i = 0; i < NUM_TRANSFERS; i++) {
        out_buffers[i] = calloc(1, PACKET_SIZE);
        out_transfers[i] = libusb_alloc_transfer(0);
        libusb_fill_bulk_transfer(out_transfers[i], dev_handle, EP_AUDIO_OUT, out_buffers[i], PACKET_SIZE, cb_out, NULL, 0);
        libusb_submit_transfer(out_transfers[i]);
    }

    pthread_create(&usb_event_thread, NULL, usb_event_worker_thread, NULL);

    int target_prefill = current_buffer_size * NUM_PHYS_CHANNELS * 4 * PERIOD_COUNT;
    int wait_cycles = 0;
    while (get_ring_avail_in() < target_prefill && running && wait_cycles++ < 500) {
        usleep(1000);
    }

    ring_head_out = target_prefill;
    ring_tail_out = 0;

    jack_activate(jack_client);

    while (running) {
        usleep(100000);
    }

    // Teardown & cleanup
    if (jack_client) {
        jack_deactivate(jack_client);
        jack_client_close(jack_client);
        jack_client = NULL;
    }

    pthread_join(usb_event_thread, NULL);

    if (spawned_jackd_pid > 0) {
        kill(spawned_jackd_pid, SIGTERM);
        waitpid(spawned_jackd_pid, NULL, 0);
        spawned_jackd_pid = 0;
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

static void *stop_worker_thread(void *arg) {
    (void)arg;
    pthread_join(audio_thread, NULL);
    post_ui_update("Stopped (Click Start to run)", TRUE, FALSE);
    return NULL;
}

static void on_start_clicked(GtkWidget *widget, gpointer data) {
    (void)widget;
    (void)data;
    if (running) return;

    gtk_label_set_text(GTK_LABEL(lbl_status), "Connecting...");
    gtk_widget_set_sensitive(btn_start, FALSE);
    gtk_widget_set_sensitive(btn_stop, TRUE);

    ring_head_out = 0;
    ring_tail_out = 0;
    memset(ring_buffer_out, 0, sizeof(ring_buffer_out));

    ring_head_in = 0;
    ring_tail_in = 0;
    memset(ring_buffer_in, 0, sizeof(ring_buffer_in));

    running = 1;
    pthread_create(&audio_thread, NULL, audio_worker_thread, NULL);
}

static void on_stop_clicked(GtkWidget *widget, gpointer data) {
    (void)widget;
    (void)data;
    if (!running) return;

    running = 0;
    post_ui_update("Stopping...", FALSE, FALSE);

    pthread_t stop_thread;
    pthread_create(&stop_thread, NULL, stop_worker_thread, NULL);
    pthread_detach(stop_thread);
}

static void on_window_destroy(GtkWidget *widget, gpointer data) {
    (void)widget;
    (void)data;
    if (running) {
        running = 0;
        pthread_join(audio_thread, NULL);
    }
    if (spawned_jackd_pid > 0) {
        kill(spawned_jackd_pid, SIGTERM);
        waitpid(spawned_jackd_pid, NULL, 0);
        spawned_jackd_pid = 0;
    }
    gtk_main_quit();
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "ZOOM UAC-8 Linux Bridge");
    
    gtk_window_set_default_size(GTK_WINDOW(window), 450, 180);
    gtk_container_set_border_width(GTK_CONTAINER(window), 20);
    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_size_request(hbox, -1, 45);

    btn_start = gtk_button_new_with_label("Start");
    btn_stop = gtk_button_new_with_label("Stop");
    gtk_widget_set_sensitive(btn_stop, FALSE);

    g_signal_connect(btn_start, "clicked", G_CALLBACK(on_start_clicked), NULL);
    g_signal_connect(btn_stop, "clicked", G_CALLBACK(on_stop_clicked), NULL);

    gtk_box_pack_start(GTK_BOX(hbox), btn_start, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), btn_stop, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    lbl_status = gtk_label_new("Stopped (Click Start to run)");
    gtk_label_set_line_wrap(GTK_LABEL(lbl_status), TRUE);
    gtk_label_set_justify(GTK_LABEL(lbl_status), GTK_JUSTIFY_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), lbl_status, TRUE, TRUE, 0);

    gtk_widget_show_all(window);
    gtk_main();

    return 0;
}
