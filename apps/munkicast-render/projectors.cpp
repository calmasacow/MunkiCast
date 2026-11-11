// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "projectors.hpp"

#include "linux-dmabuf-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-shell-client-protocol.h"
// wayland-scanner intentionally gives this request the same name as its protocol type.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#include "presentation-time-client-protocol.h"
#pragma GCC diagnostic pop
#include "xdg-decoration-unstable-v1-client-protocol.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <drm_fourcc.h>
#include <json/json.h>
#include <map>
#include <poll.h>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

using namespace munkicast::rhi;

namespace {
constexpr unsigned kWindows = 4;
constexpr std::uint64_t kBudget = 384ull * 1024 * 1024;
} // namespace

struct Projectors::Impl {
    struct Output {
        wl_output* handle = nullptr;
        std::string name;
        int scale = 1, width = 0, height = 0, transform = 0;
    };

    struct Slot {
        VkCtx* device = nullptr;
        VkExportedImage image{};
        zwp_linux_buffer_params_v1* params = nullptr;
        wl_buffer* buffer = nullptr;
        bool busy = false, foreign = false, failed = false;

        ~Slot() {
            if (params)
                zwp_linux_buffer_params_v1_destroy(params);
            if (buffer)
                wl_buffer_destroy(buffer);
            if (device)
                vk_destroy_exportable(*device, image);
        }
    };

    struct Window {
        Impl* manager = nullptr;
        unsigned id = 0;
        ldoc::Id canvas;
        std::string screen, error;
        bool preview = false, fullscreen = false, is_fullscreen = false, closed = false,
             configured = false, redraw = true;
        int width = 960, height = 540, scale = 1, native_width = 0, native_height = 0;

        int pixel_width() const {
            return is_fullscreen && native_width > 0 ? native_width : width * scale;
        }

        int pixel_height() const {
            return is_fullscreen && native_height > 0 ? native_height : height * scale;
        }

        wl_surface* surface = nullptr;
        xdg_surface* shell = nullptr;
        xdg_toplevel* top = nullptr;
        zxdg_toplevel_decoration_v1* decoration = nullptr;
        wp_viewport* viewport = nullptr;
        std::vector<struct wp_presentation_feedback*> feedback;
        std::vector<std::unique_ptr<Slot>> slots;
        Slot* acquired = nullptr;
        std::uint64_t submitted = 0, dropped = 0, presented = 0, discarded = 0;
        std::chrono::steady_clock::time_point opened = std::chrono::steady_clock::now();

        ~Window() { reset(); }

        void reset() {
            for (auto* f : feedback)
                wp_presentation_feedback_destroy(f);
            if (decoration)
                zxdg_toplevel_decoration_v1_destroy(decoration);
            if (viewport)
                wp_viewport_destroy(viewport);
            if (top)
                xdg_toplevel_destroy(top);
            if (shell)
                xdg_surface_destroy(shell);
            if (surface)
                wl_surface_destroy(surface);
            feedback.clear();
            decoration = nullptr;
            viewport = nullptr;
            top = nullptr;
            shell = nullptr;
            surface = nullptr;
            slots.clear();
            acquired = nullptr;
            // slots die last: destruction is safe even for attached buffers; the server
            // retains its DMA-BUF references until scanout/composition finishes.
        }
    };

    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    xdg_wm_base* wm = nullptr;
    zwp_linux_dmabuf_v1* dmabuf = nullptr;
    zwp_linux_dmabuf_feedback_v1* formats_feedback = nullptr;
    wp_viewporter* viewporter = nullptr;
    wp_presentation* presentation = nullptr;
    zxdg_decoration_manager_v1* decorations = nullptr;
    wl_callback* sync = nullptr;
    wl_seat* seat = nullptr;
    wl_keyboard* keyboard = nullptr;
    wl_pointer* pointer = nullptr;
    Window* focused = nullptr;
    bool initialized = false, formats_ready = false;
    std::uint64_t main_device = 0;
    std::map<std::uint32_t, std::unique_ptr<Output>> outputs;
    std::map<std::uint32_t, std::vector<std::uint64_t>> formats;

    struct Format {
        std::uint32_t format, padding;
        std::uint64_t modifier;
    };

    std::vector<Format> format_table;
    std::vector<std::unique_ptr<Window>> windows;
    std::string error;

    ~Impl() { disconnect(); }

    void disconnect() {
        windows.clear();
        if (keyboard)
            wl_keyboard_destroy(keyboard);
        if (pointer)
            wl_pointer_destroy(pointer);
        if (seat)
            wl_seat_destroy(seat);
        if (sync)
            wl_callback_destroy(sync);
        if (formats_feedback)
            zwp_linux_dmabuf_feedback_v1_destroy(formats_feedback);
        if (decorations)
            zxdg_decoration_manager_v1_destroy(decorations);
        if (presentation)
            wp_presentation_destroy(presentation);
        if (viewporter)
            wp_viewporter_destroy(viewporter);
        if (dmabuf)
            zwp_linux_dmabuf_v1_destroy(dmabuf);
        if (wm)
            xdg_wm_base_destroy(wm);
        if (compositor)
            wl_compositor_destroy(compositor);
        for (auto& [id, o] : outputs) {
            (void)id;
            wl_output_destroy(o->handle);
        }
        outputs.clear();
        if (registry)
            wl_registry_destroy(registry);
        if (display)
            wl_display_disconnect(display);
        display = nullptr;
        registry = nullptr;
        compositor = nullptr;
        wm = nullptr;
        dmabuf = nullptr;
        formats_feedback = nullptr;
        viewporter = nullptr;
        presentation = nullptr;
        decorations = nullptr;
        sync = nullptr;
        seat = nullptr;
        keyboard = nullptr;
        pointer = nullptr;
        focused = nullptr;
        initialized = formats_ready = false;
        formats.clear();
        format_table.clear();
        main_device = 0;
    }

    void input(wl_seat* s, std::uint32_t caps) {
        if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
            keyboard = wl_seat_get_keyboard(s);
            static const auto listener = [] {
                wl_keyboard_listener l{};
                l.keymap = [](void*, wl_keyboard*, std::uint32_t, int fd, std::uint32_t) {
                    close(fd);
                };
                l.enter = [](void* d, wl_keyboard*, std::uint32_t, wl_surface* surface, wl_array*) {
                    auto& self = *static_cast<Impl*>(d);
                    self.focused = nullptr;
                    for (auto& w : self.windows)
                        if (w->surface == surface)
                            self.focused = w.get();
                };
                l.leave = [](void* d, wl_keyboard*, std::uint32_t, wl_surface*) {
                    static_cast<Impl*>(d)->focused = nullptr;
                };
                l.key = [](void* d, wl_keyboard*, std::uint32_t, std::uint32_t, std::uint32_t key,
                           std::uint32_t state) {
                    auto& self = *static_cast<Impl*>(d);
                    if (key == 1 && state == WL_KEYBOARD_KEY_STATE_PRESSED && self.focused)
                        self.focused->closed = true; // Linux KEY_ESC, independent of keymap.
                };
                l.modifiers = [](void*, wl_keyboard*, std::uint32_t, std::uint32_t, std::uint32_t,
                                 std::uint32_t, std::uint32_t) {};
                l.repeat_info = [](void*, wl_keyboard*, std::int32_t, std::int32_t) {};
                return l;
            }();
            wl_keyboard_add_listener(keyboard, &listener, this);
        }
        if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
            pointer = wl_seat_get_pointer(s);
            static const auto listener = [] {
                wl_pointer_listener l{};
                l.enter = [](void*, wl_pointer* p, std::uint32_t serial, wl_surface*, wl_fixed_t,
                             wl_fixed_t) {
                    wl_pointer_set_cursor(p, serial, nullptr, 0, 0); // Clean HDMI, no cursor.
                };
                l.leave = [](void*, wl_pointer*, std::uint32_t, wl_surface*) {};
                l.motion = [](void*, wl_pointer*, std::uint32_t, wl_fixed_t, wl_fixed_t) {};
                l.button = [](void*, wl_pointer*, std::uint32_t, std::uint32_t, std::uint32_t,
                              std::uint32_t) {};
                l.axis = [](void*, wl_pointer*, std::uint32_t, std::uint32_t, wl_fixed_t) {};
                return l;
            }();
            wl_pointer_add_listener(pointer, &listener, this);
        }
    }

    void feedback() {
        formats_feedback = zwp_linux_dmabuf_v1_get_default_feedback(dmabuf);
        static const auto listener = [] {
            zwp_linux_dmabuf_feedback_v1_listener l{};
            l.done = [](void* d, auto*) { static_cast<Impl*>(d)->formats_ready = true; };
            l.format_table = [](void* d, auto*, int fd, std::uint32_t size) {
                auto& self = *static_cast<Impl*>(d);
                self.formats.clear();
                self.format_table.clear();
                if (size > 0 && size <= 16 * 1024 * 1024 && size % sizeof(Format) == 0) {
                    void* p = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
                    if (p != MAP_FAILED) {
                        self.format_table.resize(size / sizeof(Format));
                        std::memcpy(self.format_table.data(), p, size);
                        munmap(p, size);
                    }
                }
                close(fd);
            };
            l.main_device = [](void* d, auto*, wl_array* a) {
                auto& self = *static_cast<Impl*>(d);
                if (a->size == sizeof(dev_t)) {
                    dev_t device{};
                    std::memcpy(&device, a->data, sizeof(device));
                    self.main_device = static_cast<std::uint64_t>(device);
                }
            };
            l.tranche_done = [](void*, auto*) {};
            l.tranche_target_device = [](void*, auto*, wl_array*) {};
            l.tranche_formats = [](void* d, auto*, wl_array* a) {
                auto& self = *static_cast<Impl*>(d);
                const auto* bytes = static_cast<const char*>(a->data);
                for (std::size_t i = 0; i + 2 <= a->size; i += 2) {
                    std::uint16_t index{};
                    std::memcpy(&index, bytes + i, 2);
                    if (index >= self.format_table.size())
                        continue;
                    const auto& f = self.format_table[index];
                    auto& mods = self.formats[f.format];
                    if (std::ranges::find(mods, f.modifier) == mods.end())
                        mods.push_back(f.modifier);
                }
            };
            l.tranche_flags = [](void*, auto*, std::uint32_t) {};
            return l;
        }();
        zwp_linux_dmabuf_feedback_v1_add_listener(formats_feedback, &listener, this);
    }

    bool connect() {
        if (display)
            return true;
        display = wl_display_connect(nullptr);
        if (!display) {
            error = "Projectors require a Wayland session.";
            return false;
        }
        error.clear();
        registry = wl_display_get_registry(display);
        static const wl_registry_listener listener{
            [](void* d, wl_registry* r, std::uint32_t id, const char* interface, std::uint32_t v) {
                auto& self = *static_cast<Impl*>(d);
                const std::string_view name(interface);
                if (name == wl_compositor_interface.name)
                    self.compositor = static_cast<wl_compositor*>(
                        wl_registry_bind(r, id, &wl_compositor_interface, std::min(v, 4u)));
                else if (name == xdg_wm_base_interface.name) {
                    self.wm = static_cast<xdg_wm_base*>(
                        wl_registry_bind(r, id, &xdg_wm_base_interface, 1));
                    static const xdg_wm_base_listener ping{
                        [](void*, xdg_wm_base* base, std::uint32_t serial) {
                            xdg_wm_base_pong(base, serial);
                        }};
                    xdg_wm_base_add_listener(self.wm, &ping, &self);
                } else if (name == zwp_linux_dmabuf_v1_interface.name && v >= 3) {
                    self.dmabuf = static_cast<zwp_linux_dmabuf_v1*>(
                        wl_registry_bind(r, id, &zwp_linux_dmabuf_v1_interface, std::min(v, 4u)));
                    if (v >= 4)
                        self.feedback();
                    else {
                        static const zwp_linux_dmabuf_v1_listener mods{
                            [](void*, auto*, std::uint32_t) {},
                            [](void* data, auto*, std::uint32_t f, std::uint32_t hi,
                               std::uint32_t lo) {
                                static_cast<Impl*>(data)->formats[f].push_back(
                                    (std::uint64_t(hi) << 32) | lo);
                            }};
                        zwp_linux_dmabuf_v1_add_listener(self.dmabuf, &mods, &self);
                        self.formats_ready = true;
                    }
                } else if (name == wp_viewporter_interface.name)
                    self.viewporter = static_cast<wp_viewporter*>(
                        wl_registry_bind(r, id, &wp_viewporter_interface, 1));
                else if (name == wp_presentation_interface.name) {
                    self.presentation = static_cast<wp_presentation*>(
                        wl_registry_bind(r, id, &wp_presentation_interface, 1));
                    static const wp_presentation_listener clock{[](void*, auto*, std::uint32_t) {}};
                    wp_presentation_add_listener(self.presentation, &clock, &self);
                } else if (name == zxdg_decoration_manager_v1_interface.name)
                    self.decorations = static_cast<zxdg_decoration_manager_v1*>(
                        wl_registry_bind(r, id, &zxdg_decoration_manager_v1_interface, 1));
                else if (name == wl_seat_interface.name && !self.seat) {
                    self.seat = static_cast<wl_seat*>(
                        wl_registry_bind(r, id, &wl_seat_interface, std::min(v, 4u)));
                    static const wl_seat_listener caps{[](void* data, wl_seat* s, std::uint32_t c) {
                                                           static_cast<Impl*>(data)->input(s, c);
                                                       },
                                                       [](void*, wl_seat*, const char*) {}};
                    wl_seat_add_listener(self.seat, &caps, &self);
                } else if (name == wl_output_interface.name && v >= 4) {
                    auto o = std::make_unique<Output>();
                    o->handle =
                        static_cast<wl_output*>(wl_registry_bind(r, id, &wl_output_interface, 4));
                    static const auto output_listener = [] {
                        wl_output_listener l{};
                        l.geometry = [](void* data, auto*, int, int, int, int, int, const char*,
                                        const char*, int transform) {
                            static_cast<Output*>(data)->transform = transform;
                        };
                        l.mode = [](void* data, auto*, std::uint32_t flags, int width, int height,
                                    int) {
                            if (flags & WL_OUTPUT_MODE_CURRENT) {
                                auto& target = *static_cast<Output*>(data);
                                target.width = width;
                                target.height = height;
                            }
                        };
                        l.done = [](void*, auto*) {};
                        l.scale = [](void* data, auto*, int scale) {
                            static_cast<Output*>(data)->scale = std::clamp(scale, 1, 8);
                        };
                        l.name = [](void* data, auto*, const char* n) {
                            static_cast<Output*>(data)->name = n;
                        };
                        l.description = [](void*, auto*, const char*) {};
                        return l;
                    }();
                    wl_output_add_listener(o->handle, &output_listener, o.get());
                    self.outputs[id] = std::move(o);
                }
            },
            [](void* d, wl_registry*, std::uint32_t id) {
                auto& self = *static_cast<Impl*>(d);
                auto found = self.outputs.find(id);
                if (found == self.outputs.end())
                    return;
                for (auto& w : self.windows)
                    if (w->fullscreen && w->screen == found->second->name) {
                        w->error = "Projector monitor disconnected.";
                        w->closed = true;
                    }
                wl_output_destroy(found->second->handle);
                self.outputs.erase(found);
            }};
        wl_registry_add_listener(registry, &listener, this);
        sync = wl_display_sync(display);
        static const wl_callback_listener ready{[](void* d, wl_callback* c, std::uint32_t) {
            auto& self = *static_cast<Impl*>(d);
            wl_callback_destroy(c);
            self.sync = wl_display_sync(self.display);
            static const wl_callback_listener bound{
                [](void* data, wl_callback* callback, std::uint32_t) {
                    auto& manager = *static_cast<Impl*>(data);
                    manager.initialized = true;
                    manager.sync = nullptr;
                    wl_callback_destroy(callback);
                }};
            wl_callback_add_listener(self.sync, &bound, &self);
        }};
        wl_callback_add_listener(sync, &ready, this);
        wl_display_flush(display);
        return true;
    }

    void open(Window& w) {
        Output* output = nullptr;
        if (!w.screen.empty()) {
            for (auto& [id, o] : outputs) {
                (void)id;
                if (o->name == w.screen)
                    output = o.get();
            }
            if (!output) {
                w.error = "Projector monitor was not found: " + w.screen;
                w.closed = true;
                return;
            }
            w.scale = output->scale;
        }
        w.surface = wl_compositor_create_surface(compositor);
        w.shell = xdg_wm_base_get_xdg_surface(wm, w.surface);
        static const xdg_surface_listener configure{
            [](void* d, xdg_surface* surface, std::uint32_t serial) {
                auto& win = *static_cast<Window*>(d);
                xdg_surface_ack_configure(surface, serial);
                win.configured = true;
                win.redraw = true;
            }};
        xdg_surface_add_listener(w.shell, &configure, &w);
        static const auto surface_listener = [] {
            wl_surface_listener l{};
            l.enter = [](void* d, wl_surface*, wl_output* out) {
                auto& win = *static_cast<Window*>(d);
                for (auto& [id, o] : win.manager->outputs) {
                    (void)id;
                    if (o->handle != out)
                        continue;
                    if (win.scale != o->scale) {
                        win.scale = o->scale;
                        win.redraw = true;
                    }
                    if (win.fullscreen)
                        win.screen = o->name;
                }
            };
            l.leave = [](void*, wl_surface*, wl_output*) {};
            return l;
        }();
        wl_surface_add_listener(w.surface, &surface_listener, &w);
        w.top = xdg_surface_get_toplevel(w.shell);
        static const auto top_listener = [] {
            xdg_toplevel_listener l{};
            l.configure = [](void* d, xdg_toplevel*, int width, int height, wl_array* states) {
                auto& win = *static_cast<Window*>(d);
                win.is_fullscreen = false;
                const auto* bytes = static_cast<const char*>(states->data);
                for (std::size_t i = 0; i + sizeof(std::uint32_t) <= states->size;
                     i += sizeof(std::uint32_t)) {
                    std::uint32_t state{};
                    std::memcpy(&state, bytes + i, sizeof(state));
                    if (state == XDG_TOPLEVEL_STATE_FULLSCREEN)
                        win.is_fullscreen = true;
                }
                if (width > 8192 || height > 8192) {
                    win.error = "Projector logical size exceeds 8192 pixels.";
                    win.closed = true;
                    return;
                }
                if (width > 0)
                    win.width = width;
                if (height > 0)
                    win.height = height;
            };
            l.close = [](void* d, xdg_toplevel*) { static_cast<Window*>(d)->closed = true; };
            return l;
        }();
        xdg_toplevel_add_listener(w.top, &top_listener, &w);
        xdg_toplevel_set_app_id(w.top, "org.munkicast.Studio.Projector");
        const auto title = std::string(w.preview ? "Preview" : "Program") +
                           " Projector — MunkiCast (" + std::to_string(w.id) + ")";
        xdg_toplevel_set_title(w.top, title.c_str());
        if (w.fullscreen)
            xdg_toplevel_set_fullscreen(w.top, output ? output->handle : nullptr);
        if (decorations) {
            w.decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(decorations, w.top);
            static const zxdg_toplevel_decoration_v1_listener listener{
                [](void*, auto*, std::uint32_t) {}};
            zxdg_toplevel_decoration_v1_add_listener(w.decoration, &listener, &w);
            zxdg_toplevel_decoration_v1_set_mode(w.decoration,
                                                 ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        }
        w.viewport = wp_viewporter_get_viewport(viewporter, w.surface);
        wl_surface_commit(w.surface);
    }

    bool dispatch() {
        // Never roundtrip or wait for a frame callback on the output clock. prepare_read
        // interlocks with dispatch_pending; poll(0) makes compositor stalls harmless.
        if (!display)
            return false;
        while (wl_display_prepare_read(display) != 0)
            if (wl_display_dispatch_pending(display) < 0)
                return false;
        const int flushed = wl_display_flush(display);
        if (flushed < 0 && errno != EAGAIN) {
            wl_display_cancel_read(display);
            return false;
        }
        pollfd fd{wl_display_get_fd(display), POLLIN, 0};
        const int rc = poll(&fd, 1, 0);
        if (rc > 0 && (fd.revents & POLLIN)) {
            if (wl_display_read_events(display) < 0)
                return false;
        } else
            wl_display_cancel_read(display);
        if (rc > 0 && (fd.revents & (POLLERR | POLLHUP | POLLNVAL)))
            return false;
        return wl_display_dispatch_pending(display) >= 0;
    }

    std::uint64_t allocated() const {
        std::uint64_t bytes = 0;
        for (const auto& w : windows)
            for (const auto& s : w->slots)
                bytes += std::uint64_t(s->image.width) * s->image.height * 4;
        return bytes;
    }

    void allocate(Window& w, VkCtx& device) {
        const auto width = std::uint32_t(w.pixel_width());
        const auto height = std::uint32_t(w.pixel_height());
        if (width > 8192 || height > 8192 || width == 0 || height == 0) {
            w.error = "Projector size exceeds 8192 pixels.";
            w.closed = true;
            return;
        }
        // Retire resized buffers only after release. Never grow a ring to work around a slow
        // consumer: three release-tracked buffers bound memory and avoid a CPU frame queue.
        std::erase_if(w.slots, [&](const auto& s) {
            return !s->busy && !s->params && (s->image.width != width || s->image.height != height);
        });
        if (w.slots.size() >= 3)
            return;
        if (allocated() + std::uint64_t(width) * height * 4 > kBudget) {
            w.error = "Projector buffers exceed the 384 MiB display budget.";
            w.closed = true;
            return;
        }
        auto s = std::make_unique<Slot>();
        s->device = &device;
        std::string why;
        // Default feedback describes the compositor's rendering GPU. Unknown/different GPUs
        // need system memory: VRAM imports across devices can succeed then fault at scanout.
        const bool host = !main_device || !vk_matches_drm_device(device, main_device);
        for (auto format : {DRM_FORMAT_XBGR8888, DRM_FORMAT_XRGB8888}) {
            const auto found = formats.find(format);
            if (found == formats.end())
                continue;
            auto allowed = found->second;
            // Native tiled layouts can exclude system RAM even when linear sharing works.
            // Across GPUs use the portable linear modifier, only when advertised by Wayland.
            if (host)
                std::erase_if(allowed,
                              [](auto modifier) { return modifier != DRM_FORMAT_MOD_LINEAR; });
            if (vk_create_exportable(device, width, height, format, allowed, host, s->image, why))
                break;
        }
        if (!s->image.image) {
            w.error = "No compatible GPU projector buffer: " + why;
            w.closed = true;
            return;
        }
        s->params = zwp_linux_dmabuf_v1_create_params(dmabuf);
        for (unsigned p = 0; p < s->image.n_planes; ++p)
            zwp_linux_buffer_params_v1_add(
                s->params, s->image.dmabuf_fd, p, s->image.offsets[p], s->image.strides[p],
                std::uint32_t(s->image.modifier >> 32), std::uint32_t(s->image.modifier));
        static const zwp_linux_buffer_params_v1_listener listener{
            [](void* d, zwp_linux_buffer_params_v1* p, wl_buffer* b) {
                auto& slot = *static_cast<Slot*>(d);
                slot.buffer = b;
                slot.params = nullptr;
                zwp_linux_buffer_params_v1_destroy(p);
                static const wl_buffer_listener release{
                    [](void* data, wl_buffer*) { static_cast<Slot*>(data)->busy = false; }};
                wl_buffer_add_listener(b, &release, &slot);
            },
            [](void* d, zwp_linux_buffer_params_v1* p) {
                auto& slot = *static_cast<Slot*>(d);
                slot.failed = true;
                slot.params = nullptr;
                zwp_linux_buffer_params_v1_destroy(p);
            }};
        zwp_linux_buffer_params_v1_add_listener(s->params, &listener, s.get());
        zwp_linux_buffer_params_v1_create(s->params, int(width), int(height), s->image.drm_format,
                                          0);
        w.slots.push_back(std::move(s));
    }
};

Projectors::Projectors() : impl_(std::make_unique<Impl>()) {}

Projectors::~Projectors() = default;

std::string Projectors::control(const std::string& json, const ldoc::Project& project) {
    auto& s = *impl_;
    try {
        Json::CharReaderBuilder builder;
        builder["failIfExtra"] = true;
        const auto reader = std::unique_ptr<Json::CharReader>(builder.newCharReader());
        Json::Value request;
        std::string parse_error;
        if (!reader->parse(json.data(), json.data() + json.size(), &request, &parse_error) ||
            !request.isObject())
            throw std::runtime_error("Projector request must be a JSON object.");
        if (request.isMember("closeAll")) {
            if (!request["closeAll"].isBool())
                throw std::runtime_error("closeAll must be boolean.");
            if (request["closeAll"].asBool()) {
                s.disconnect();
                s.error.clear();
            }
        } else if (request.isMember("close")) {
            if (!request["close"].isUInt())
                throw std::runtime_error("close must be a projector ID.");
            const auto id = request["close"].asUInt();
            for (auto& w : s.windows)
                if (w->id == id)
                    w->closed = true;
        } else if (request.isMember("id")) {
            if (!request["id"].isUInt() || !request["canvas"].isString() ||
                !request["fullscreen"].isBool() || !request["screen"].isString())
                throw std::runtime_error("Invalid projector request types.");
            const auto id = request["id"].asUInt();
            const auto canvas = ldoc::Id::parse(request["canvas"].asString());
            const auto screen = request["screen"].asString();
            const bool fullscreen = request["fullscreen"].asBool();
            if (request.isMember("preview") && !request["preview"].isBool())
                throw std::runtime_error("preview must be boolean.");
            if (!canvas || !project.canvas(*canvas) || screen.size() > 256 || id == 0)
                throw std::runtime_error("Invalid projector canvas, screen or ID.");
            if (std::ranges::any_of(s.windows, [&](const auto& w) { return w->id == id; }))
                throw std::runtime_error("Projector ID already exists.");
            std::erase_if(s.windows, [](const auto& w) { return w->closed; });
            if (s.windows.size() >= kWindows)
                throw std::runtime_error("At most four projectors can be open.");
            if (s.connect()) {
                auto w = std::make_unique<Impl::Window>();
                w->manager = &s;
                w->id = id;
                w->canvas = *canvas;
                w->screen = screen;
                w->fullscreen = fullscreen;
                w->preview = request.get("preview", false).asBool();
                const auto* c = project.canvas(*canvas);
                w->height = std::max(1, int(960.0 * c->height / std::max(1, c->width)));
                s.windows.push_back(std::move(w));
                s.error.clear();
            }
        } else if (!request["status"].isBool() || !request["status"].asBool()) {
            throw std::runtime_error("Unknown projector operation.");
        }
    } catch (const std::exception& e) {
        s.error = e.what();
    }
    return state();
}

std::string Projectors::state() const {
    const auto& s = *impl_;
    Json::Value result(Json::objectValue), rows(Json::arrayValue);
    for (const auto& w : s.windows) {
        Json::Value row(Json::objectValue);
        row["id"] = w->id;
        row["canvas"] = w->canvas.to_string();
        row["preview"] = w->preview;
        row["fullscreen"] = w->is_fullscreen;
        row["requestedFullscreen"] = w->fullscreen;
        row["screen"] = w->screen;
        row["active"] = !w->closed;
        row["ready"] = w->submitted > 0 && !w->closed;
        row["width"] = w->pixel_width();
        row["height"] = w->pixel_height();
        row["submitted"] = Json::UInt64(w->submitted);
        row["dropped"] = Json::UInt64(w->dropped);
        row["presented"] = Json::UInt64(w->presented);
        row["discarded"] = Json::UInt64(w->discarded);
        row["error"] = w->error;
        rows.append(std::move(row));
    }
    result["presentationFeedback"] = s.presentation != nullptr;
    result["windows"] = std::move(rows);
    result["error"] = s.error;
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, result);
}

std::optional<std::chrono::nanoseconds> Projectors::pump(const ldoc::Project& project) {
    auto& s = *impl_;
    if (s.display && !s.dispatch()) {
        s.error = "Projector Wayland connection lost.";
        s.disconnect();
    }
    std::optional<std::chrono::nanoseconds> period;
    for (auto& w : s.windows) {
        if (!project.canvas(w->canvas)) {
            w->error = "Projector canvas was removed.";
            w->closed = true;
        }
        if (!w->closed && std::chrono::steady_clock::now() - w->opened > std::chrono::seconds(5) &&
            !w->submitted) {
            w->error = "Projector display did not become ready within five seconds.";
            w->closed = true;
        }
        if (w->closed) {
            if (s.focused == w.get())
                s.focused = nullptr;
            w->reset();
            continue;
        }
        if (s.initialized && s.formats_ready && !w->surface) {
            if (!s.compositor || !s.wm || !s.dmabuf || !s.viewporter) {
                w->error = "Wayland compositor needs xdg-shell, linux-dmabuf v3 and viewporter.";
                w->closed = true;
                continue;
            }
            s.open(*w);
        }
        if (w->fullscreen)
            for (const auto& [id, output] : s.outputs) {
                (void)id;
                if (output->name != w->screen)
                    continue;
                const bool rotated = (output->transform & 1) != 0;
                const int width = rotated ? output->height : output->width;
                const int height = rotated ? output->width : output->height;
                if (w->native_width != width || w->native_height != height)
                    w->redraw = true;
                // Fullscreen uses actual mode pixels, even under fractional desktop scaling.
                w->native_width = width;
                w->native_height = height;
            }
        const auto* c = project.canvas(w->canvas);
        const auto frame = c && c->fps.valid() ? std::chrono::nanoseconds(std::max<std::int64_t>(
                                                     1, 1000000000LL * c->fps.den / c->fps.num))
                                               : std::chrono::nanoseconds(16666667);
        if (!period || frame < *period)
            period = frame;
    }
    if (!period && s.display) {
        // No display clock remains to service Wayland pings. Disconnect while idle, retaining
        // final diagnostics for the UI; the next open negotiates a fresh connection.
        auto closed = std::move(s.windows);
        s.disconnect();
        s.windows = std::move(closed);
    }
    return period;
}

bool Projectors::wants(const ldoc::Id& canvas) const {
    return std::ranges::any_of(impl_->windows,
                               [&](const auto& w) { return !w->closed && w->canvas == canvas; });
}

bool Projectors::dirty() const {
    return std::ranges::any_of(impl_->windows,
                               [](const auto& w) { return !w->closed && w->redraw; });
}

std::vector<PresentationTarget> Projectors::acquire(const ldoc::Id& canvas, VkCtx* device) {
    auto& s = *impl_;
    std::vector<PresentationTarget> targets;
    for (auto& w : s.windows) {
        if (w->closed || w->canvas != canvas)
            continue;
        if (!device) {
            w->error = "Projectors require the Vulkan renderer; CPU fallback is not suitable for "
                       "HDMI output.";
            w->closed = true;
            continue;
        }
        if (!w->configured) {
            ++w->dropped;
            continue;
        }
        if (std::ranges::any_of(w->slots, [](const auto& slot) { return slot->failed; })) {
            w->error = "Wayland rejected the projector DMA-BUF.";
            w->closed = true;
            continue;
        }
        s.allocate(*w, *device);
        const auto found = std::ranges::find_if(w->slots, [&](const auto& slot) {
            return slot->buffer && !slot->busy && slot->image.width == unsigned(w->pixel_width()) &&
                   slot->image.height == unsigned(w->pixel_height());
        });
        if (w->closed || found == w->slots.end()) {
            ++w->dropped;
            continue;
        }
        w->acquired = found->get();
        targets.push_back({&w->acquired->image, w->acquired->foreign});
    }
    return targets;
}

void Projectors::present(const ldoc::Id& canvas, bool success) {
    auto& s = *impl_;
    for (auto& w : s.windows) {
        if (w->canvas != canvas || !w->acquired)
            continue;
        auto& slot = *w->acquired;
        w->acquired = nullptr;
        if (!success) {
            w->error = "GPU projector composition failed.";
            w->closed = true;
            continue;
        }
        slot.foreign = true;
        slot.busy = true;
        wl_surface_attach(w->surface, slot.buffer, 0, 0);
        wp_viewport_set_destination(w->viewport, w->width, w->height);
        wl_surface_damage(w->surface, 0, 0, INT32_MAX, INT32_MAX);
        auto* opaque = wl_compositor_create_region(s.compositor);
        wl_region_add(opaque, 0, 0, w->width, w->height);
        wl_surface_set_opaque_region(w->surface, opaque);
        wl_region_destroy(opaque);
        // Video follows the canvas clock, not frame callbacks (which can halve the rate
        // when display and canvas clocks drift). Release-tracked buffers provide backpressure;
        // a new commit replaces pending content instead of queuing CPU frames for later.
        if (s.presentation && w->feedback.size() < 4) {
            auto* f = wp_presentation_feedback(s.presentation, w->surface);
            w->feedback.push_back(f);
            static const wp_presentation_feedback_listener listener{
                [](void*, auto*, wl_output*) {},
                [](void* d, auto* feedback, std::uint32_t, std::uint32_t, std::uint32_t,
                   std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) {
                    auto& win = *static_cast<Impl::Window*>(d);
                    ++win.presented;
                    std::erase(win.feedback, feedback);
                    wp_presentation_feedback_destroy(feedback);
                },
                [](void* d, auto* feedback) {
                    auto& win = *static_cast<Impl::Window*>(d);
                    ++win.discarded;
                    std::erase(win.feedback, feedback);
                    wp_presentation_feedback_destroy(feedback);
                }};
            wp_presentation_feedback_add_listener(f, &listener, w.get());
        }
        wl_surface_commit(w->surface);
        ++w->submitted;
        w->redraw = false;
    }
    if (s.display)
        wl_display_flush(s.display);
}
