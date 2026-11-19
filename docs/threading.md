# Threading

LibWM runs on two threads inside the application process. Getting work onto the
right one is the main rule of the codebase.

## The two threads

| Thread | Runs | Owns |
|---|---|---|
| **Main** (the application's) | the application's `Core::EventLoop`, LibGUI, and Wayland dispatch (`WaylandClient::dispatch`, from a `Core::Notifier` on the display fd) | all Wayland objects; GUI objects such as `FilePicker` |
| **LibWM server** (`"LibWM Server"`) | one `Core::EventLoop` serving every in-process portal | `WindowServerConnection` (its windows, menus, `MenuController`) and the other portal connections, including their IPC sockets |

The Wayland connection is made on the main thread before the server thread
starts (`start_server_thread` in `src/LibWM.cpp`), so dispatch always happens
there.

## Rules

**1. Portal connections are touched only on the server thread.** LibIPC has no
send lock, and the connections' own state (windows, menus, double-click
tracking) is unsynchronised. Wayland events arrive on the main thread, so
`WindowServerConnection` wraps every callback it hands to `WaylandClient` in
`on_server_thread()`, which runs it inline if already on the server thread and
otherwise posts it there (`MainThreadInvoker::post_to_server`). The posted task
captures the connection as a raw pointer: the connection lives for the whole
process (LibWM leaks its slot), and taking a reference on the main thread would
race the server thread's own reference counting (rule 5). Event order is
preserved, since it is one queue. The clipboard connection's selection-changed
callback is posted the same way. Paths that change connection state `VERIFY` they are on the server
thread.

**1a. Nothing calls from Wayland code into the server thread's objects.** The
menus are the case to remember: the menubar is drawn by `MenuController`, which
belongs to the server thread, so the server thread renders it into an image and
hands that over (`WaylandClient::set_window_inset`). Presentation, which also
runs from frame callbacks on the main thread, only copies the image.

**2. GUI work goes to the main thread, asynchronously.** A portal request that
needs a GUI (the file picker) posts it with `MainThreadInvoker::post_to_main` and
posts the reply back with `post_to_server`. The server thread never blocks
waiting for the main thread: the GUI it would wait on talks to the WindowServer
the server thread owns.

**3. `WaylandClient` state is guarded by one recursive mutex.** `m_mutex` covers
the client and its area objects (`WaylandOutputs`, `WaylandSeat`,
`WaylandDataDevice`, `SerenityShell`). Each window additionally has
`WindowSurface::mutex`, always taken **after** `m_mutex`. Both are recursive, so
a public method may call another.

**4. Event handlers run with `m_mutex` held.** `dispatch()` holds it for the
whole of `wl_display_dispatch`, so a handler that blocks holds up the server
thread's calls into `WaylandClient` and the main thread's event loop. Anything
that waits on another client (reading a drag offer) is bounded by a timeout;
clipboard reads release the lock and read outside it.

**5. Nothing refcounted is shared between the threads.** AK reference counts
(`RefCounted`, `String`, `ByteString`, `Gfx::Bitmap`) are not atomic: if both
threads ref and unref the same object, updates are lost and the object is freed
while still in use. So nothing that crosses threads is shared:

* posted tasks capture long-lived objects as raw pointers, never `RefPtr`s;
* strings are deep-copied (`ByteString { value.view() }`);
* bitmaps and MIME-data maps go through `copy_for_other_thread()` (`WaylandClient.h`):
  cursor and icon bitmaps from IPC messages, the wallpaper and screen captures,
  and the drag-and-drop data handed to the server thread.

**6. Callbacks that capture an object belong to it.** The display-link and
host-settings callbacks are installed with an owner (`set_*_callback(owner,
fn)`): only the first owner installs, and only that owner can clear the callback.

**7. Process-wide objects are never destroyed.** `WaylandClient::the()` and the
portal connection slots are intentionally leaked. The detached server thread may
still be using them during `__cxa_finalize`, and tearing them down there races
it.

**8. Other shared state.** The portal fd table in `LibWM.cpp` has its own mutex
(portals can be requested from any thread), and `SOCKET_TAKEOVER` is set under a
mutex while spawning a service, because it is process-global.
