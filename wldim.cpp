#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QLabel>
#include <QSlider>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QWidget>

#include <wayland-client.h>
#include <wayland-client-protocol.h>

#include <pthread.h>
#include <atomic>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include "ls-header.h"

#define DIM_MAX 90

static const char *ICON_SVG = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" width="48" height="48" viewBox="0 0 48 48">
  <g stroke="#f5a623" stroke-width="3" stroke-linecap="round">
    <line x1="24" y1="3" x2="24" y2="9"/>
    <line x1="24" y1="39" x2="24" y2="45"/>
    <line x1="3" y1="24" x2="9" y2="24"/>
    <line x1="39" y1="24" x2="45" y2="24"/>
    <line x1="9.2" y1="9.2" x2="13.5" y2="13.5"/>
    <line x1="34.5" y1="34.5" x2="38.8" y2="38.8"/>
    <line x1="9.2" y1="38.8" x2="13.5" y2="34.5"/>
    <line x1="34.5" y1="13.5" x2="38.8" y2="9.2"/>
  </g>
  <circle cx="24" cy="24" r="10" fill="#f9c440"/>
  <path d="M24 14 a10 10 0 0 1 0 20 z" fill="#2b303b" opacity="0.85"/>
</svg>
)SVG";

struct dim_ctx;

struct output_ctx {
    struct dim_ctx *ctx;
    uint32_t name;
    struct wl_output *output;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer;
    struct wl_buffer *buffer;
    void *shm_data;
    int shm_fd;
    int32_t width, height;
    struct wl_list link;
};

struct pending_output {
    uint32_t name;
    struct wl_list link;
};

struct dim_ctx {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct wl_region *input_region;
    struct wl_list outputs;
    struct wl_list pending;
    std::atomic<int> dim;
    std::atomic<int> running;
    int efd;
};

static struct dim_ctx g_ctx;

static int
create_shm_file(off_t size)
{
    int fd = memfd_create("wldim-shm", MFD_CLOEXEC);
    if (fd < 0)
        return -1;
    if (ftruncate(fd, size) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void
fill_buffer(struct output_ctx *o, int dim)
{
    uint32_t a = (uint32_t)(dim * 255 / 100) << 24;
    uint32_t *pix = (uint32_t *)o->shm_data;
    size_t n = (size_t)o->width * o->height;
    for (size_t i = 0; i < n; i++)
        pix[i] = a;
}

static void
alloc_buffer(struct output_ctx *o, int32_t width, int32_t height)
{
    struct dim_ctx *ctx = &g_ctx;
    if (o->buffer) {
        wl_buffer_destroy(o->buffer);
        o->buffer = NULL;
    }
    if (o->shm_data) {
        munmap(o->shm_data, (size_t)o->width * o->height * 4);
        o->shm_data = NULL;
    }
    if (o->shm_fd >= 0) {
        close(o->shm_fd);
        o->shm_fd = -1;
    }

    o->width = width;
    o->height = height;
    if (width <= 0 || height <= 0)
        return;

    size_t size = (size_t)width * height * 4;
    o->shm_fd = create_shm_file(size);
    if (o->shm_fd < 0)
        return;
    o->shm_data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, o->shm_fd, 0);
    if (o->shm_data == MAP_FAILED) {
        o->shm_data = NULL;
        close(o->shm_fd);
        o->shm_fd = -1;
        return;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(ctx->shm, o->shm_fd, size);
    o->buffer = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4,
        WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
}

static void
repaint(struct output_ctx *o)
{
    if (!o->buffer)
        return;
    fill_buffer(o, g_ctx.dim.load());
    wl_surface_attach(o->surface, o->buffer, 0, 0);
    wl_surface_damage_buffer(o->surface, 0, 0, o->width, o->height);
    wl_surface_commit(o->surface);
}

static void
layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *layer,
                        uint32_t serial, uint32_t width, uint32_t height)
{
    struct output_ctx *o = (struct output_ctx *)data;
    zwlr_layer_surface_v1_ack_configure(layer, serial);
    if (o->width != (int32_t)width || o->height != (int32_t)height || !o->buffer)
        alloc_buffer(o, width, height);
    repaint(o);
}

static void
layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *layer)
{
    (void)data;
    (void)layer;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

static void
output_ctx_destroy(struct output_ctx *o)
{
    if (o->buffer)
        wl_buffer_destroy(o->buffer);
    if (o->shm_data)
        munmap(o->shm_data, (size_t)o->width * o->height * 4);
    if (o->shm_fd >= 0)
        close(o->shm_fd);
    if (o->layer)
        zwlr_layer_surface_v1_destroy(o->layer);
    if (o->surface)
        wl_surface_destroy(o->surface);
    if (o->output)
        wl_output_destroy(o->output);
    wl_list_remove(&o->link);
    free(o);
}

static void
output_ctx_create(struct dim_ctx *ctx, uint32_t name, struct wl_output *output)
{
    struct output_ctx *o = (struct output_ctx *)calloc(1, sizeof(*o));
    o->ctx = ctx;
    o->name = name;
    o->output = output;
    o->shm_fd = -1;
    wl_list_insert(&ctx->outputs, &o->link);

    o->surface = wl_compositor_create_surface(ctx->compositor);
    if (!ctx->input_region)
        ctx->input_region = wl_compositor_create_region(ctx->compositor);
    wl_surface_set_input_region(o->surface, ctx->input_region);
    o->layer = zwlr_layer_shell_v1_get_layer_surface(
        ctx->layer_shell, o->surface, output,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "wldim");
    zwlr_layer_surface_v1_set_anchor(o->layer,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_size(o->layer, 0, 4000);
    zwlr_layer_surface_v1_set_exclusive_zone(o->layer, 0);
    zwlr_layer_surface_v1_add_listener(o->layer, &layer_surface_listener, o);
    wl_surface_commit(o->surface);
}

static void
output_geometry(void *data, struct wl_output *output, int32_t x, int32_t y,
                int32_t pw, int32_t ph, int32_t sub, const char *make,
                const char *model, int32_t transform)
{
    (void)data; (void)output; (void)x; (void)y; (void)pw; (void)ph;
    (void)sub; (void)make; (void)model; (void)transform;
}

static void
output_mode(void *data, struct wl_output *output, uint32_t flags,
            int32_t width, int32_t height, int32_t rate)
{
    (void)data; (void)output; (void)flags; (void)width; (void)height; (void)rate;
}

static void
output_done(void *data, struct wl_output *output)
{
    (void)data; (void)output;
}

static void
output_scale(void *data, struct wl_output *output, int32_t factor)
{
    (void)data; (void)output; (void)factor;
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
};

static int
all_ready(struct dim_ctx *ctx)
{
    return ctx->compositor && ctx->shm && ctx->layer_shell;
}

static void
flush_pending(struct dim_ctx *ctx)
{
    if (!all_ready(ctx))
        return;
    struct pending_output *p, *tmp;
    wl_list_for_each_safe(p, tmp, &ctx->pending, link) {
        struct wl_output *output = (struct wl_output *)wl_registry_bind(ctx->registry, p->name,
            &wl_output_interface, 2);
        wl_output_add_listener(output, &output_listener, NULL);
        output_ctx_create(ctx, p->name, output);
        wl_list_remove(&p->link);
        free(p);
    }
}

static void
registry_global(void *data, struct wl_registry *registry, uint32_t name,
                const char *interface, uint32_t version)
{
    struct dim_ctx *ctx = (struct dim_ctx *)data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        ctx->compositor = (struct wl_compositor *)wl_registry_bind(registry, name,
            &wl_compositor_interface, version < 4 ? version : 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        ctx->shm = (struct wl_shm *)wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        ctx->layer_shell = (struct zwlr_layer_shell_v1 *)wl_registry_bind(registry, name,
            &zwlr_layer_shell_v1_interface, version < 3 ? version : 3);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        if (all_ready(ctx)) {
            struct wl_output *output = (struct wl_output *)wl_registry_bind(registry, name,
                &wl_output_interface, 2);
            wl_output_add_listener(output, &output_listener, NULL);
            output_ctx_create(ctx, name, output);
        } else {
            struct pending_output *p = (struct pending_output *)calloc(1, sizeof(*p));
            p->name = name;
            wl_list_insert(&ctx->pending, &p->link);
            return;
        }
    } else {
        return;
    }
    flush_pending(ctx);
}

static void
registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    struct dim_ctx *ctx = (struct dim_ctx *)data;
    struct output_ctx *o, *tmp;
    wl_list_for_each_safe(o, tmp, &ctx->outputs, link) {
        if (o->name == name)
            output_ctx_destroy(o);
    }
    (void)registry;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void
overlay_update_all(struct dim_ctx *ctx)
{
    struct output_ctx *o;
    wl_list_for_each(o, &ctx->outputs, link)
        repaint(o);
}

static void *
overlay_thread(void *p)
{
    struct dim_ctx *ctx = (struct dim_ctx *)p;

    ctx->display = wl_display_connect(NULL);
    if (!ctx->display) {
        fprintf(stderr, "wldim: cannot connect to wayland compositor\n");
        return NULL;
    }

    ctx->registry = wl_display_get_registry(ctx->display);
    wl_registry_add_listener(ctx->registry, &registry_listener, ctx);
    wl_display_roundtrip(ctx->display);

    if (!all_ready(ctx)) {
        fprintf(stderr, "wldim: compositor lacks layer-shell/shm support\n");
        wl_display_disconnect(ctx->display);
        return NULL;
    }
    flush_pending(ctx);
    wl_display_roundtrip(ctx->display);

    while (ctx->running.load()) {
        struct pollfd pfds[2];
        pfds[0].fd = wl_display_get_fd(ctx->display);
        pfds[0].events = POLLIN;
        pfds[1].fd = ctx->efd;
        pfds[1].events = POLLIN;

        while (wl_display_prepare_read(ctx->display) != 0)
            wl_display_dispatch_pending(ctx->display);
        wl_display_flush(ctx->display);

        if (poll(pfds, 2, -1) < 0) {
            wl_display_cancel_read(ctx->display);
            if (errno == EINTR)
                continue;
            break;
        }

        if (pfds[1].revents & POLLIN) {
            uint64_t v;
            ssize_t r = read(ctx->efd, &v, sizeof(v));
            (void)r;
            overlay_update_all(ctx);
        }

        if (wl_display_read_events(ctx->display) != 0)
            break;
        if (wl_display_dispatch_pending(ctx->display) < 0)
            break;
    }

    ctx->running.store(0);
    wl_display_flush(ctx->display);
    wl_display_disconnect(ctx->display);
    return NULL;
}

static QString
config_path(void)
{
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/wldim.conf";
}

static int
config_load(void)
{
    QFile f(config_path());
    if (!f.open(QIODevice::ReadOnly))
        return 50;
    int dim = f.readAll().trimmed().toInt();
    f.close();
    if (dim < 0 || dim > DIM_MAX)
        dim = 50;
    return dim;
}

static void
config_save(int dim)
{
    QFile f(config_path());
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QByteArray::number(dim) + "\n");
        f.close();
    }
}

static QString
autostart_path(void)
{
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
        + "/autostart/com.wurztha.screendim.desktop";
}

static bool
autostart_get(void)
{
    return QFile::exists(autostart_path());
}

static void
autostart_set(bool enabled)
{
    QString path = autostart_path();
    if (enabled) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(
                "[Desktop Entry]\n"
                "Type=Application\n"
                "Name=Screen Dim\n"
                "Comment=Dim the screen below hardware minimum brightness\n"
                "Exec=wldim\n"
                "Icon=com.wurztha.screendim\n"
                "Terminal=false\n"
                "Categories=Utility;Qt;\n");
            f.close();
        }
    } else {
        QFile::remove(path);
    }
}

static void
on_usr1(int sig)
{
    (void)sig;
    struct dim_ctx *ctx = &g_ctx;
    ctx->dim.store(config_load());
    uint64_t one = 1;
    ssize_t r = write(ctx->efd, &one, sizeof(one));
    (void)r;
}

int
main(int argc, char *argv[])
{
    struct dim_ctx *ctx = &g_ctx;
    wl_list_init(&ctx->outputs);
    wl_list_init(&ctx->pending);
    ctx->dim.store(config_load());
    ctx->running.store(1);
    ctx->efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

    signal(SIGUSR1, on_usr1);

    pthread_t thread;
    pthread_create(&thread, NULL, overlay_thread, ctx);

    QApplication app(argc, argv);
    QGuiApplication::setDesktopFileName("com.wurztha.screendim");
    QGuiApplication::setApplicationName("Screen Dim");

    QImage img;
    img.loadFromData(QByteArray(ICON_SVG), "SVG");
    QIcon icon(QPixmap::fromImage(img));

    QWidget window;
    window.setWindowTitle("Screen Dim");
    window.setWindowIcon(icon);
    window.setMinimumWidth(320);

    QVBoxLayout *outer = new QVBoxLayout(&window);
    QHBoxLayout *row = new QHBoxLayout;
    row->addWidget(new QLabel("Brightness"));
    QSlider *slider = new QSlider(Qt::Horizontal);
    slider->setRange(0, 100);
    slider->setValue(100 - ctx->dim.load() * 100 / DIM_MAX);
    row->addWidget(slider);
    outer->addLayout(row);

    QCheckBox *check = new QCheckBox("Autostart");
    check->setChecked(autostart_get());
    outer->addWidget(check);

    QObject::connect(slider, &QSlider::valueChanged, [ctx](int bright) {
        int dim = DIM_MAX * (100 - bright) / 100;
        ctx->dim.store(dim);
        uint64_t one = 1;
        ssize_t r = write(ctx->efd, &one, sizeof(one));
        (void)r;
        config_save(dim);
    });
    QObject::connect(check, &QCheckBox::toggled, [](bool on) {
        autostart_set(on);
    });

    window.show();
    int status = app.exec();

    ctx->running.store(0);
    uint64_t one = 1;
    ssize_t r = write(ctx->efd, &one, sizeof(one));
    (void)r;
    pthread_join(thread, NULL);
    return status;
}
