// Actual rendererless Wayland/wlroots lifecycle. No OpenVR or real desktop.
#define _GNU_SOURCE
#define WLR_USE_UNSTABLE
#include <assert.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wlr/types/wlr_shm.h>
#include "../cursor-cache.h"

static struct wl_compositor *client_compositor;
static struct wl_shm *client_shm;
static void global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    (void)data; (void)version;
    if (!strcmp(interface, "wl_compositor")) client_compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    if (!strcmp(interface, "wl_shm")) client_shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
}
static void removed(void *data, struct wl_registry *registry, uint32_t name) { (void)data; (void)registry; (void)name; }
static const struct wl_registry_listener registry_listener = {global, removed};
static void stage(struct wl_display *display, int report, int ack, char value) {
    assert(wl_display_roundtrip(display) >= 0);
    assert(write(report, &value, 1) == 1);
    char answer;
    assert(read(ack, &answer, 1) == 1 && answer == value);
}
static void client(int fd, int report, int ack) {
    alarm(10);
    struct wl_display *d = wl_display_connect_to_fd(fd);
    assert(d);
    struct wl_registry *registry = wl_display_get_registry(d);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    assert(wl_display_roundtrip(d) >= 0 && client_compositor && client_shm);
    int memory = memfd_create("offline-cursor", MFD_CLOEXEC);
    assert(memory >= 0 && ftruncate(memory, 600*4) == 0);
    uint32_t *pixels = mmap(NULL, 600*4, PROT_READ|PROT_WRITE, MAP_SHARED, memory, 0);
    assert(pixels != MAP_FAILED); pixels[0] = 0xff123456;
    struct wl_shm_pool *pool = wl_shm_create_pool(client_shm, memory, 600*4);
    struct wl_buffer *small = wl_shm_pool_create_buffer(pool, 0, 8, 1, 32, WL_SHM_FORMAT_ARGB8888);
    struct wl_buffer *large = wl_shm_pool_create_buffer(pool, 0, 600, 1, 2400, WL_SHM_FORMAT_ARGB8888);
    struct wl_surface *surface = wl_compositor_create_surface(client_compositor);
    wl_surface_attach(surface, small, 0, 0); wl_surface_commit(surface);
    stage(d, report, ack, 1); // image committed before any set_cursor/consumer
    wl_surface_commit(surface); stage(d, report, ack, 2); // callback-only commit preserves image
    wl_surface_attach(surface, NULL, 0, 0); wl_surface_commit(surface); stage(d, report, ack, 3);
    wl_surface_attach(surface, large, 0, 0); wl_surface_commit(surface); stage(d, report, ack, 4);
    wl_surface_attach(surface, small, 0, 0); wl_surface_commit(surface); stage(d, report, ack, 5);
    wl_surface_destroy(surface); stage(d, report, ack, 6);
    wl_buffer_destroy(small); wl_buffer_destroy(large); wl_shm_pool_destroy(pool);
    wl_registry_destroy(registry); wl_display_disconnect(d);
    munmap(pixels, 600*4); close(memory);
    _exit(0);
}
int main(void) {
    alarm(10);
    struct wl_display *display = wl_display_create();
    struct wlr_compositor *compositor = wlr_compositor_create(display, 6, NULL);
    uint32_t formats[] = {DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888};
    assert(wlr_shm_create(display, 1, formats, 2));
    struct ft_cursor_cache cache;
    ft_cursor_cache_init(&cache, compositor);
    int connection[2], report[2], ack[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0, connection) == 0);
    assert(pipe(report) == 0 && pipe(ack) == 0);
    pid_t pid = fork(); assert(pid >= 0);
    if (!pid) { close(connection[0]); close(report[0]); close(ack[1]); client(connection[1], report[1], ack[0]); }
    close(connection[1]); close(report[1]); close(ack[0]);
    fcntl(report[0], F_SETFL, O_NONBLOCK);
    assert(wl_client_create(display, connection[0]));
    struct wl_event_loop *loop = wl_display_get_event_loop(display);
    struct wlr_buffer *first = NULL;
    for (char expected = 1; expected <= 6;) {
        assert(wl_event_loop_dispatch(loop, 10) >= 0); wl_display_flush_clients(display);
        char observed;
        if (read(report[0], &observed, 1) != 1) continue;
        assert(observed == expected);
        if (expected == 6) assert(wl_list_empty(&cache.frames));
        else {
            assert(!wl_list_empty(&cache.frames));
            struct ft_cursor_frame *f = wl_container_of(cache.frames.next, f, link);
            assert(f->surface->current.buffer == NULL); // wlroots already released it
            struct wlr_buffer *buffer = ft_cursor_cached(&cache, f->surface);
            if (expected == 3 || expected == 4) assert(buffer == NULL);
            else {
                assert(buffer && buffer->width == 8 && buffer->height == 1);
                if (expected == 1) first = buffer;
                if (expected == 2) assert(buffer == first);
                void *data; uint32_t format; size_t stride;
                assert(wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride));
                assert(*(uint32_t *)data == 0xff123456 && format == DRM_FORMAT_ARGB8888 && stride == 32);
                wlr_buffer_end_data_ptr_access(buffer);
            }
        }
        assert(write(ack[1], &observed, 1) == 1); ++expected;
    }
    int status; assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(report[0]); close(ack[1]); wl_display_destroy_clients(display);
    ft_cursor_cache_finish(&cache); wl_display_destroy(display);
    puts("cursor before selection, buffer-only lifetime, null/oversize commits and destruction passed");
}
