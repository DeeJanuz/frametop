#pragma once
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>

// A rendererless compositor has no persistent surface texture. wlroots releases
// current.buffer after each commit callback. KDE commits a cursor before issuing
// set_cursor, so retain small buffers from the first commit, not from set_cursor.
// This also keeps animation/callback-only commits from erasing the last image.
struct ft_cursor_cache;
struct ft_cursor_frame {
    struct ft_cursor_cache *cache;
    struct wlr_surface *surface;
    struct wlr_buffer *buffer;
    struct wl_listener commit, destroy;
    struct wl_list link;
};
struct ft_cursor_cache {
    struct wl_listener new_surface;
    struct wl_list frames;
};
static inline struct wlr_buffer *ft_cursor_cached(struct ft_cursor_cache *cache, struct wlr_surface *surface) {
    struct ft_cursor_frame *f;
    wl_list_for_each(f, &cache->frames, link) if (f->surface == surface) return f->buffer;
    return NULL;
}
static inline void ft_cursor_frame_committed(struct wl_listener *listener, void *data) {
    (void)data;
    struct ft_cursor_frame *f = wl_container_of(listener, f, commit);
    if (!(f->surface->current.committed & WLR_SURFACE_STATE_BUFFER)) return;
    struct wlr_buffer *buffer = f->surface->current.buffer;
    // Desktop DMA-BUFs go straight to SteamVR; do not retain them here.
    struct wlr_buffer *next = buffer && buffer->width > 0 && buffer->height > 0 &&
        buffer->width <= 512 && buffer->height <= 512 ? wlr_buffer_lock(buffer) : NULL;
    if (f->buffer) wlr_buffer_unlock(f->buffer);
    f->buffer = next;
}
static inline void ft_cursor_frame_destroyed(struct wl_listener *listener, void *data) {
    (void)data;
    struct ft_cursor_frame *f = wl_container_of(listener, f, destroy);
    wl_list_remove(&f->commit.link); wl_list_remove(&f->destroy.link); wl_list_remove(&f->link);
    if (f->buffer) wlr_buffer_unlock(f->buffer);
    free(f);
}
static inline void ft_cursor_surface_created(struct wl_listener *listener, void *data) {
    struct ft_cursor_cache *cache = wl_container_of(listener, cache, new_surface);
    struct wlr_surface *surface = data;
    struct ft_cursor_frame *f = calloc(1, sizeof *f);
    if (!f) { wl_client_post_no_memory(wl_resource_get_client(surface->resource)); return; }
    f->cache = cache; f->surface = surface;
    f->commit.notify = ft_cursor_frame_committed;
    f->destroy.notify = ft_cursor_frame_destroyed;
    wl_signal_add(&surface->events.commit, &f->commit);
    wl_signal_add(&surface->events.destroy, &f->destroy);
    wl_list_insert(&cache->frames, &f->link);
}
static inline void ft_cursor_cache_init(struct ft_cursor_cache *cache, struct wlr_compositor *compositor) {
    wl_list_init(&cache->frames);
    cache->new_surface.notify = ft_cursor_surface_created;
    wl_signal_add(&compositor->events.new_surface, &cache->new_surface);
}
static inline void ft_cursor_cache_finish(struct ft_cursor_cache *cache) {
    wl_list_remove(&cache->new_surface.link);
    struct ft_cursor_frame *f, *tmp;
    wl_list_for_each_safe(f, tmp, &cache->frames, link) ft_cursor_frame_destroyed(&f->destroy, NULL);
}
