#include "Camera.h"
#include "Game.h"
#include "Input.h"
#include "Renderer.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <algorithm>
#include <exception>
#include <memory>
#include <string>

namespace
{
    constexpr uint32_t kInitialWidth = 1280;
    constexpr uint32_t kInitialHeight = 720;
    constexpr float kClearColor[4] = { 0.030f, 0.038f, 0.055f, 1.0f };
    constexpr float kOrbitSensitivity = 0.008f; // radians per relative mouse count

    struct App
    {
        Renderer renderer;
        IsoCamera camera;
        Game game;
        Input input;
        std::unique_ptr<DirectX::Keyboard> keyboard;
        std::unique_ptr<DirectX::Mouse> mouse;
        std::unique_ptr<DirectX::GamePad> gamepad;
        bool rendererReady = false;
    };

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        App* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        switch (msg)
        {
        case WM_CREATE:
        {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return 0;
        }
        case WM_SIZE:
            if (app && app->rendererReady && wParam != SIZE_MINIMIZED)
            {
                app->renderer.Resize(LOWORD(lParam), HIWORD(lParam));
                app->camera.SetViewport(LOWORD(lParam), HIWORD(lParam));
            }
            return 0;
        case WM_ACTIVATE:
        case WM_ACTIVATEAPP:
            // The toolkit resets held state here, so keys can't stick when
            // focus is lost.
            DirectX::Keyboard::ProcessMessage(msg, wParam, lParam);
            DirectX::Mouse::ProcessMessage(msg, wParam, lParam);
            break;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYUP:
            // Escape used to close the window from right here. It belongs to
            // the game now: with menus to back out through, what it means
            // depends on the screen you're on, and this end can't see that.
            DirectX::Keyboard::ProcessMessage(msg, wParam, lParam);
            return 0;
        case WM_INPUT:
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MOUSEWHEEL:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        case WM_MOUSEHOVER:
            DirectX::Mouse::ProcessMessage(msg, wParam, lParam);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// The command line, wide and quoted by Windows, folded down to the two
// options the game takes: `--connect <host>` points it at somebody else's
// server, and `--class <name>` skips the menus and takes the field as that
// class on launch. Together they put a second machine into a fight in one
// shortcut; `--class` on its own does the same for a match hosted on this
// one, which is the shortest road from a build to a firefight.
static void ParseArgs(Game& game)
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return;

    const auto narrow = [](const wchar_t* wide) {
        std::string out;
        for (; *wide; ++wide)
            out.push_back(static_cast<char>(*wide)); // hosts and class names are ASCII
        return out;
    };

    std::string host, cls;
    for (int i = 1; i < argc; ++i)
    {
        if (wcscmp(argv[i], L"--connect") == 0 && i + 1 < argc)
            host = narrow(argv[++i]);
        else if (wcscmp(argv[i], L"--class") == 0 && i + 1 < argc)
            cls = narrow(argv[++i]);
    }
    if (!host.empty() || !cls.empty())
        game.SetMultiplayer(host, cls);
    LocalFree(argv);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
    // GamePad's Windows.Gaming.Input backend activates WinRT factories, which
    // needs COM initialized on this thread before the first GetState call.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    App app;
    ParseArgs(app.game);
    app.keyboard = std::make_unique<DirectX::Keyboard>();
    app.mouse = std::make_unique<DirectX::Mouse>();
    app.gamepad = std::make_unique<DirectX::GamePad>();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    wc.lpszClassName = L"InfantryWindowClass";
    RegisterClassExW(&wc);

    RECT rect = { 0, 0, static_cast<LONG>(kInitialWidth), static_cast<LONG>(kInitialHeight) };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    const LONG windowWidth = rect.right - rect.left;
    const LONG windowHeight = rect.bottom - rect.top;

    // Center on the primary monitor's work area so the frame clears the taskbar.
    RECT work = {};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0))
        work = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    const int windowX = work.left + (work.right - work.left - windowWidth) / 2;
    const int windowY = work.top + (work.bottom - work.top - windowHeight) / 2;

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Infantry", WS_OVERLAPPEDWINDOW,
                                windowX, windowY,
                                windowWidth, windowHeight,
                                nullptr, nullptr, hInstance, &app);
    if (!hwnd)
        return 1;
    app.mouse->SetWindow(hwnd);

    try
    {
        app.renderer.Init(hwnd, kInitialWidth, kInitialHeight);
        app.game.LoadContent(app.renderer);
    }
    catch (const std::exception& e)
    {
        MessageBoxA(hwnd, e.what(), "Startup failed", MB_OK | MB_ICONERROR);
        return 1;
    }
    app.rendererReady = true;
    app.camera.SetViewport(kInitialWidth, kInitialHeight);
    app.camera.SnapToTarget();

    ShowWindow(hwnd, nCmdShow);

    LARGE_INTEGER freq, prev;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);

    bool running = true;
    while (running)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
                running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running)
            break;

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float dt = static_cast<float>(now.QuadPart - prev.QuadPart) / static_cast<float>(freq.QuadPart);
        prev = now;
        dt = std::min(dt, 0.1f); // avoid huge steps after stalls

        app.input.Update();

        // Hold the orbit control to turn the camera: relative mode captures and
        // hides the cursor and reports raw deltas until it's let go. Bound like
        // everything else, which is why the mode switch reads the two edges of
        // an action rather than the two edges of the middle mouse button.
        const Bindings& binds = app.game.Binds();
        if (binds.Pressed(app.input, Bindings::Action::OrbitCamera))
            app.mouse->SetMode(DirectX::Mouse::MODE_RELATIVE);
        else if (binds.Released(app.input, Bindings::Action::OrbitCamera))
            app.mouse->SetMode(DirectX::Mouse::MODE_ABSOLUTE);
        if (app.input.mouse.positionMode == DirectX::Mouse::MODE_RELATIVE)
            app.camera.AddYaw(static_cast<float>(app.input.mouse.x) * kOrbitSensitivity);

        app.camera.AddZoom(app.input.wheel);
        app.game.Update(dt, app.input, app.camera);
        // Quitting from the menu takes the same road out as the close box: tear
        // the window down and let the next pump pick up the WM_QUIT, rather than
        // dropping out of the loop here and leaving a second way to shut down.
        // The `continue` is what keeps this frame from rendering into it.
        if (app.game.QuitRequested())
        {
            DestroyWindow(hwnd);
            continue;
        }
        app.camera.Update(dt);

        if (app.renderer.Width() > 0 && app.renderer.Height() > 0)
        {
            try
            {
                app.renderer.BeginFrame(kClearColor);
                app.renderer.SetViewProj(app.camera.ViewProj());
                app.game.Render(app.renderer);
                app.renderer.EndFrame();
            }
            catch (const std::exception& e)
            {
                MessageBoxA(hwnd, e.what(), "Render error", MB_OK | MB_ICONERROR);
                running = false;
            }
        }
    }

    app.rendererReady = false;
    app.game.Shutdown();
    app.renderer.Shutdown();
    return 0;
}
