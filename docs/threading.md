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
holds a reference to the connection. Event order is preserved, since it is one
queue. Paths that change connection state `VERIFY` they are on the server
thread.

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

**5. Strings crossing threads are deep-copied.** `ByteString` reference counts
are not atomic. Getters that hand state from one thread to another return
`ByteString { value.view() }`, not a shared copy.

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
