# Source layout

The source tree is organized by responsibility. Start from `app/` when
following application flow, and from `canvas/CanvasWidget.h` when following
painting behavior.

| Directory | Responsibility |
| --- | --- |
| `app/` | Application shell, main window, startup page, settings, file commands, shortcuts, and native-window integration |
| `canvas/` | Canvas widget, tabs/panes, input routing, rendering entry points, layer editing, selection, and undo coordination |
| `document/` | In-memory document model, project serialization, recent files, and undo recording |
| `rendering/` | Reusable OpenGL composition, shader cache, view transforms, and GPU layer allocation |
| `io/` | External formats such as PSD and ABR |
| `licensing/` | Pro-build licensing and public-key material |
| `actions/` | One-shot editing workflows and their controller/host interfaces |
| `tools/core/` | Tool contracts, shared context, configuration, registry, and GPU dispatch helpers |
| `tools/canvas/` | Persistent pointer tools used directly on the canvas |
| `tools/actions/` | Tool implementations owned by one-shot actions |
| `docks/` | Main-window dock panels; `docks/layers/` contains private layer-panel widgets |
| `dialogs/` | Modal dialogs and floating editor panels |
| `components/` | Reusable UI controls with no application-shell ownership |
| `shortcuts/` | Command metadata and shortcut persistence |

## Main integration points

- `main.cpp` configures Qt/OpenGL and creates `MainWindow`.
- `app/MainWindow.cpp` is the application-shell hub. Its implementation is
  split into `MainWindowWorkspace`, `MainWindowFiles`, `MainWindowSettings`,
  `MainWindowEvents`, `MainWindowShortcuts`, `MainWindowChrome`, and
  `MainWindowNative`.
- `canvas/CanvasWidget.cpp` is the canvas integration hub. Rendering, input,
  layers, selection, compositing, geometry, actions, and undo live in their
  correspondingly named implementation files.
- `document/CanvasDocument` owns editable state. UI code should use its public
  operations and change notifications instead of duplicating document state.
- `actions/CanvasActionHost` is the narrow bridge from editing actions back to
  the canvas. New action implementations should depend on this interface,
  rather than including `CanvasWidget.h`.

## Dependency direction

Prefer dependencies in this direction:

`app/UI -> canvas/actions/tools -> document/rendering`

Format adapters in `io/` may bridge the document and canvas APIs when GPU
pixel transfer is required. Lower-level document and rendering code should not
depend on `app/`, docks, or dialogs.
