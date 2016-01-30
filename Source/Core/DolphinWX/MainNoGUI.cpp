// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <getopt.h>
#include <string>
#include <unistd.h>

#include "Common/CommonTypes.h"
#include "Common/Event.h"
#include "Common/MsgHandler.h"
#include "Common/Logging/LogManager.h"

#include "Core/BootManager.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/Host.h"
#include "Core/State.h"
#include "Core/HW/Wiimote.h"
#include "Core/IPC_HLE/WII_IPC_HLE_Device_usb.h"
#include "Core/IPC_HLE/WII_IPC_HLE_WiiMote.h"
#include "Core/PowerPC/PowerPC.h"

#include "UICommon/UICommon.h"

#include "VideoCommon/VideoBackendBase.h"

static bool rendererHasFocus = true;
static bool rendererIsFullscreen = false;
static bool running = true;

class Platform
{
public:
	virtual void Init() {}
	virtual void SetTitle(const std::string &title) {}
	virtual void MainLoop() { while(running) {} }
	virtual void Shutdown() {}
	virtual ~Platform() {}
};

static Platform* platform;

void Host_NotifyMapLoaded() {}
void Host_RefreshDSPDebuggerWindow() {}

static Common::Event updateMainFrameEvent;
void Host_Message(int Id)
{
	if (Id == WM_USER_STOP)
		running = false;
}

static void* s_window_handle = nullptr;
void* Host_GetRenderHandle()
{
	return s_window_handle;
}

void Host_UpdateTitle(const std::string& title)
{
	platform->SetTitle(title);
}

void Host_UpdateDisasmDialog(){}

void Host_UpdateMainFrame()
{
	updateMainFrameEvent.Set();
}

void Host_RequestRenderWindowSize(int width, int height) {}

void Host_RequestFullscreen(bool enable_fullscreen) {}

void Host_SetStartupDebuggingParameters()
{
	SConfig& StartUp = SConfig::GetInstance();
	StartUp.bEnableDebugging = false;
	StartUp.bBootToPause = false;
}

bool Host_UIHasFocus()
{
	return false;
}

bool Host_RendererHasFocus()
{
	return rendererHasFocus;
}

bool Host_RendererIsFullscreen()
{
	return rendererIsFullscreen;
}

void Host_ConnectWiimote(int wm_idx, bool connect)
{
	if (Core::IsRunning() && SConfig::GetInstance().bWii)
	{
		bool was_unpaused = Core::PauseAndLock(true);
		GetUsbPointer()->AccessWiiMote(wm_idx | 0x100)->Activate(connect);
		Host_UpdateMainFrame();
		Core::PauseAndLock(false, was_unpaused);
	}
}

void Host_SetWiiMoteConnectionState(int _State) {}

void Host_ShowVideoConfig(void*, const std::string&, const std::string&) {}

#if HAVE_X11
#include <X11/keysym.h>
#include "DolphinWX/X11Utils.h"

class PlatformX11 : public Platform
{
	Display *dpy;
	Window win;
	Cursor blankCursor = None;
#if defined(HAVE_XRANDR) && HAVE_XRANDR
	X11Utils::XRRConfiguration *XRRConfig;
#endif

	void Init() override
	{
		XInitThreads();
		dpy = XOpenDisplay(nullptr);
		if (!dpy)
		{
			PanicAlert("No X11 display found");
			exit(1);
		}

		win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy),
					  SConfig::GetInstance().iRenderWindowXPos,
					  SConfig::GetInstance().iRenderWindowYPos,
					  SConfig::GetInstance().iRenderWindowWidth,
					  SConfig::GetInstance().iRenderWindowHeight,
					  0, 0, BlackPixel(dpy, 0));
		XSelectInput(dpy, win, KeyPressMask | FocusChangeMask);
		Atom wmProtocols[1];
		wmProtocols[0] = XInternAtom(dpy, "WM_DELETE_WINDOW", True);
		XSetWMProtocols(dpy, win, wmProtocols, 1);
		XMapRaised(dpy, win);
		XFlush(dpy);
		s_window_handle = (void*)win;

		if (SConfig::GetInstance().bDisableScreenSaver)
			X11Utils::InhibitScreensaver(dpy, win, true);

#if defined(HAVE_XRANDR) && HAVE_XRANDR
		XRRConfig = new X11Utils::XRRConfiguration(dpy, win);
#endif

		if (SConfig::GetInstance().bHideCursor)
		{
			// make a blank cursor
			Pixmap Blank;
			XColor DummyColor;
			char ZeroData[1] = { 0 };
			Blank = XCreateBitmapFromData(dpy, win, ZeroData, 1, 1);
			blankCursor = XCreatePixmapCursor(dpy, Blank, Blank, &DummyColor, &DummyColor, 0, 0);
			XFreePixmap(dpy, Blank);
			XDefineCursor(dpy, win, blankCursor);
		}
	}

	void SetTitle(const std::string &string) override
	{
		XStoreName(dpy, win, string.c_str());
	}

	void MainLoop() override
	{
		bool fullscreen = SConfig::GetInstance().bFullscreen;

		if (fullscreen)
		{
			rendererIsFullscreen = X11Utils::ToggleFullscreen(dpy, win);
#if defined(HAVE_XRANDR) && HAVE_XRANDR
			XRRConfig->ToggleDisplayMode(True);
#endif
		}

		// The actual loop
		while (running)
		{
			XEvent event;
			KeySym key;
			for (int num_events = XPending(dpy); num_events > 0; num_events--)
			{
				XNextEvent(dpy, &event);
				switch (event.type)
				{
				case KeyPress:
					key = XLookupKeysym((XKeyEvent*)&event, 0);
					if (key == XK_Escape)
					{
						if (Core::GetState() == Core::CORE_RUN)
						{
							if (SConfig::GetInstance().bHideCursor)
								XUndefineCursor(dpy, win);
							Core::SetState(Core::CORE_PAUSE);
						}
						else
						{
							if (SConfig::GetInstance().bHideCursor)
								XDefineCursor(dpy, win, blankCursor);
							Core::SetState(Core::CORE_RUN);
						}
					}
					else if ((key == XK_Return) && (event.xkey.state & Mod1Mask))
					{
						fullscreen = !fullscreen;
						X11Utils::ToggleFullscreen(dpy, win);
#if defined(HAVE_XRANDR) && HAVE_XRANDR
						XRRConfig->ToggleDisplayMode(fullscreen);
#endif
					}
					else if (key >= XK_F1 && key <= XK_F8)
					{
						int slot_number = key - XK_F1 + 1;
						if (event.xkey.state & ShiftMask)
							State::Save(slot_number);
						else
							State::Load(slot_number);
					}
					else if (key == XK_F9)
						Core::SaveScreenShot();
					else if (key == XK_F11)
						State::LoadLastSaved();
					else if (key == XK_F12)
					{
						if (event.xkey.state & ShiftMask)
							State::UndoLoadState();
						else
							State::UndoSaveState();
					}
					break;
				case FocusIn:
					rendererHasFocus = true;
					if (SConfig::GetInstance().bHideCursor &&
					    Core::GetState() != Core::CORE_PAUSE)
						XDefineCursor(dpy, win, blankCursor);
					break;
				case FocusOut:
					rendererHasFocus = false;
					if (SConfig::GetInstance().bHideCursor)
						XUndefineCursor(dpy, win);
					break;
				case ClientMessage:
					if ((unsigned long) event.xclient.data.l[0] == XInternAtom(dpy, "WM_DELETE_WINDOW", False))
						running = false;
					break;
				}
			}
			if (!fullscreen)
			{
				Window winDummy;
				unsigned int borderDummy, depthDummy;
				XGetGeometry(dpy, win, &winDummy,
					     &SConfig::GetInstance().iRenderWindowXPos,
					     &SConfig::GetInstance().iRenderWindowYPos,
					     (unsigned int *)&SConfig::GetInstance().iRenderWindowWidth,
					     (unsigned int *)&SConfig::GetInstance().iRenderWindowHeight,
					     &borderDummy, &depthDummy);
				rendererIsFullscreen = false;
			}
			usleep(100000);
		}
	}

	void Shutdown() override
	{
#if defined(HAVE_XRANDR) && HAVE_XRANDR
		delete XRRConfig;
#endif

		if (SConfig::GetInstance().bHideCursor)
			XFreeCursor(dpy, blankCursor);

		XCloseDisplay(dpy);
	}
};
#endif

static Platform* GetPlatform()
{
#if defined(USE_EGL) && defined(USE_HEADLESS)
	return new Platform();
#elif HAVE_X11
	return new PlatformX11();
#endif
	return nullptr;
}

int main(int argc, char* argv[])
{
	int ch, help = 0;
	struct option longopts[] = {
		{ "exec",    no_argument, nullptr, 'e' },
		{ "help",    no_argument, nullptr, 'h' },
		{ "version", no_argument, nullptr, 'v' },
		{ nullptr,      0,           nullptr,  0  }
	};

	while ((ch = getopt_long(argc, argv, "eh?v", longopts, 0)) != -1)
	{
		switch (ch)
		{
		case 'e':
			break;
		case 'h':
		case '?':
			help = 1;
			break;
		case 'v':
			fprintf(stderr, "%s\n", scm_rev_str);
			return 1;
		}
	}

	if (help == 1 || argc == optind)
	{
		fprintf(stderr, "%s\n\n", scm_rev_str);
		fprintf(stderr, "A multi-platform GameCube/Wii emulator\n\n");
		fprintf(stderr, "Usage: %s [-e <file>] [-h] [-v]\n", argv[0]);
		fprintf(stderr, "  -e, --exec     Load the specified file\n");
		fprintf(stderr, "  -h, --help     Show this help message\n");
		fprintf(stderr, "  -v, --version  Print version and exit\n");
		return 1;
	}

	platform = GetPlatform();
	if (!platform)
	{
		fprintf(stderr, "No platform found\n");
		return 1;
	}

	UICommon::SetUserDirectory(""); // Auto-detect user folder
	UICommon::Init();

	platform->Init();

	if (!BootManager::BootCore(argv[optind]))
	{
		fprintf(stderr, "Could not boot %s\n", argv[optind]);
		return 1;
	}

	while (!Core::IsRunning())
		updateMainFrameEvent.Wait();

	platform->MainLoop();
	Core::Stop();
	while (PowerPC::GetState() != PowerPC::CPU_POWERDOWN)
		updateMainFrameEvent.Wait();

	Core::Shutdown();
	platform->Shutdown();
	UICommon::Shutdown();

	delete platform;

	return 0;
}
