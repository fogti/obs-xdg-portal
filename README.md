# obs-xdg-portal

OBS Studio plugin that captures windows and monitors using portals. It relies
on PipeWire for exchanging buffers between the compositor.

### Building

**Dependencies:**

 - OBS Studio with Wayland / DMA-BUF support ([here](https://github.com/obsproject/obs-studio/pull/3338))
 - PipeWire >= 0.3

```
$ meson . _build --prefix /usr
$ sudo ninja -C _build install
```
