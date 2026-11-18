# Out-of-process SerenityOS services, built from the upstream sources.
#
# LibWM spawns these on demand when an application connects to their portal
# (see make_portal() in src/LibWM.cpp), handing over a socket through the same
# SOCKET_TAKEOVER contract SystemServer uses, so no Serenity SystemServer is
# needed.

# --- WebContent ---------------------------------------------------------------
# A single-client service: one process per connection. Help, Browser and any
# other WebView app reach it through the webcontent portal.
set(WEBCONTENT_SRC "${SERENITY_PROJECT_ROOT}/Userland/Services/WebContent")
add_executable(WebContent
    "${WEBCONTENT_SRC}/ConnectionFromClient.cpp"
    "${WEBCONTENT_SRC}/ConsoleGlobalEnvironmentExtensions.cpp"
    "${WEBCONTENT_SRC}/ImageCodecPluginSerenity.cpp"
    "${WEBCONTENT_SRC}/PageClient.cpp"
    "${WEBCONTENT_SRC}/PageHost.cpp"
    "${WEBCONTENT_SRC}/WebContentConsoleClient.cpp"
    "${WEBCONTENT_SRC}/WebDriverConnection.cpp"
    "${WEBCONTENT_SRC}/main.cpp"
)
target_include_directories(WebContent PRIVATE "${SERENITY_PROJECT_ROOT}/Userland/Services")
target_link_options(WebContent PRIVATE "-Wl,--no-as-needed")
# WebContent is a client-stack process too: link the shim so its outgoing portal
# connects (RequestServer, ImageDecoder) go through LibWM and are spawned/served
# on demand, exactly as an application's are.
target_link_libraries(WebContent PRIVATE LibCore LibFileSystem LibIPC LibGfx LibAudio LibImageDecoderClient LibJS LibWebView LibWeb LibLocale LibMain LibURL LibWM)
add_dependencies(WebContent
    generate_WebContentClientEndpoint.h
    generate_WebContentServerEndpoint.h
    generate_WebDriverClientEndpoint.h
    generate_WebDriverServerEndpoint.h
)

# --- RequestServer ------------------------------------------------------------
# The HTTP/TLS backend WebContent and the browser need for page loads. Also a
# single-client service, spawned on demand the same way.
set(REQUESTSERVER_SRC "${SERENITY_PROJECT_ROOT}/Userland/Services/RequestServer")
add_executable(RequestServer
    "${REQUESTSERVER_SRC}/ConnectionFromClient.cpp"
    "${REQUESTSERVER_SRC}/ConnectionCache.cpp"
    "${REQUESTSERVER_SRC}/GeminiProtocol.cpp"
    "${REQUESTSERVER_SRC}/GeminiRequest.cpp"
    "${REQUESTSERVER_SRC}/HttpProtocol.cpp"
    "${REQUESTSERVER_SRC}/HttpRequest.cpp"
    "${REQUESTSERVER_SRC}/HttpsProtocol.cpp"
    "${REQUESTSERVER_SRC}/HttpsRequest.cpp"
    "${REQUESTSERVER_SRC}/Protocol.cpp"
    "${REQUESTSERVER_SRC}/Request.cpp"
    "${REQUESTSERVER_SRC}/main.cpp"
)
target_include_directories(RequestServer PRIVATE "${SERENITY_PROJECT_ROOT}/Userland/Services")
target_link_options(RequestServer PRIVATE "-Wl,--no-as-needed")
target_link_libraries(RequestServer PRIVATE LibCore LibCrypto LibIPC LibGemini LibHTTP LibMain LibTLS LibWebSocket LibURL LibThreading)
add_dependencies(RequestServer
    generate_RequestClientEndpoint.h
    generate_RequestServerEndpoint.h
)

# --- SQLServer ----------------------------------------------------------------
# A multi-client service (IPC::MultiServer). LibWM binds its listening socket
# and hands that over, rather than spawning one per connection. Browser needs it
# for history/bookmarks/cookies.
set(SQLSERVER_SRC "${SERENITY_PROJECT_ROOT}/Userland/Services/SQLServer")
add_executable(SQLServer
    "${SQLSERVER_SRC}/ConnectionFromClient.cpp"
    "${SQLSERVER_SRC}/DatabaseConnection.cpp"
    "${SQLSERVER_SRC}/SQLStatement.cpp"
    "${SQLSERVER_SRC}/main.cpp"
)
target_include_directories(SQLServer PRIVATE "${SERENITY_PROJECT_ROOT}/Userland/Services")
target_link_options(SQLServer PRIVATE "-Wl,--no-as-needed")
target_link_libraries(SQLServer PRIVATE LibCore LibIPC LibSQL LibMain)
add_dependencies(SQLServer
    generate_SQLClientEndpoint.h
    generate_SQLServerEndpoint.h
)

# The SerenityOS taskbar applets (WorkspacePicker and friends) are part of the
# desktop environment, not the port: SDE builds them (serenity-desktop-environment
# /Applets, scripts/build-applets.sh), replacing any SerenityOS kernel interface
# with its Linux equivalent.
