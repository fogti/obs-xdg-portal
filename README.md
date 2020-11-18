# obs-xdg-portal

OBS Studio plugin that captures windows and monitors using portals. It relies
on PipeWire for exchanging buffers between the compositor.

### Building

**Dependencies:**

 - OBS Studio with Wayland / DMA-BUF support ([here](dma-buf-pull-request))
 - PipeWire >= 0.3

```
$ meson . _build --prefix /usr
$ sudo ninja -C _build install
```

[dma-buf-pull-request]: https://github.com/obsproject/obs-studio/pull/3338
