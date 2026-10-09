/*
 * Copyright (c) 2026, libgui-wayland contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibCore/EventLoop.h>
#include <pthread.h>

namespace LibWM {

// Lets the LibWM server thread and the main (GUI) thread hand work to each
// other.
//
// LibWM serves its portals on a dedicated thread, but some requests need real
// GUI (e.g. showing a file picker), and GUI objects belong to the main thread.
// The server thread must not *block* waiting for such work either, because the
// GUI it is waiting on talks back to the WindowServer that the server thread
// owns. So instead both directions are asynchronous: a task is posted to the
// other thread's event loop (thread-safe and wakes it), and any continuation is
// posted back. All access to a portal connection stays on the server thread.
//
// EventLoop::deferred_invoke() on another thread's loop enqueues to that loop's
// ThreadEventQueue (mutex-protected) and wakes it, so this is safe.
class MainThreadInvoker {
public:
    // Called once from the main thread, before the server thread starts.
    static void install_main()
    {
        s_main_thread = pthread_self();
        s_main_loop = &Core::EventLoop::current();
    }

    // Called once from the server thread, at its start.
    static void install_server(Core::EventLoop& server_loop)
    {
        s_server_loop = &server_loop;
    }

    static bool on_main_thread() { return pthread_equal(pthread_self(), s_main_thread); }

    static void post_to_main(Function<void()> fn)
    {
        if (s_main_loop)
            s_main_loop->deferred_invoke(move(fn));
    }

    static void post_to_server(Function<void()> fn)
    {
        if (s_server_loop)
            s_server_loop->deferred_invoke(move(fn));
    }

private:
    static inline pthread_t s_main_thread {};
    static inline Core::EventLoop* s_main_loop { nullptr };
    static inline Core::EventLoop* s_server_loop { nullptr };
};

}
