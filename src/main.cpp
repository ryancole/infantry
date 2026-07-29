#include "Camera.h"
#include "Game.h"
#include "Input.h"
#include "Renderer.h"

#include <windows.h>
#include <windowsx.h>
#include <algorithm>
#include <exception>

namespace
{
    constexpr uint32_t kInitialWidth = 1280;
    constexpr uint32_t kInitialHeight = 720;
    constexpr float kClearColor[4] = { 0.030f, 0.038f, 0.055f, 1.0f };

    struct App
    {
        Renderer renderer;
        IsoCamera camera;
        Game game;
        Input input;
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
        case WM_KEYDOWN:
            if (app)
                app->input.keys[wParam & 0xFF] = true;
            if (wParam == VK_ESCAPE)
                DestroyWindow(hwnd);
            return 0;
        case WM_KEYUP:
            if (app)
                app->input.keys[wParam & 0xFF] = false;
            return 0;
        case WM_MOUSEMOVE:
            if (app)
            {
                app->input.mouseX = static_cast<float>(GET_X_LPARAM(lParam));
                app->input.mouseY = static_cast<float>(GET_Y_LPARAM(lParam));
            }
            return 0;
        case WM_LBUTTONDOWN:
            if (app) app->input.mouseDown[0] = true;
            SetCapture(hwnd);
            return 0;
        case WM_LBUTTONUP:
            if (app) app->input.mouseDown[0] = false;
            ReleaseCapture();
            return 0;
        case WM_RBUTTONDOWN:
            if (app) app->input.mouseDown[1] = true;
            return 0;
        case WM_RBUTTONUP:
            if (app) app->input.mouseDown[1] = false;
            return 0;
        case WM_MOUSEWHEEL:
            if (app)
                app->input.wheel += static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
            return 0;
        case WM_KILLFOCUS:
            if (app)
                app->input = Input{}; // drop stuck keys when focus is lost
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
    App app;

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

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Infantry", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                rect.right - rect.left, rect.bottom - rect.top,
                                nullptr, nullptr, hInstance, &app);
    if (!hwnd)
        return 1;

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

        app.camera.zoom = std::clamp(app.camera.zoom - app.input.wheel * 2.0f, 10.0f, 60.0f);
        app.game.Update(dt, app.input, app.camera);
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

        app.input.NextFrame();
    }

    app.rendererReady = false;
    app.renderer.Shutdown();
    return 0;
}
