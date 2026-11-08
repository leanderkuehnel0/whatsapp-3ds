// WA3DS: placeholder entry point so the build pipeline has something to compile.
// The real UI (chat list on the bottom screen, conversation on the top screen)
// replaces this once the bridge API exists.
#include <3ds.h>
#include <stdio.h>

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

int main(void)
{
	gfxInitDefault();
	consoleInit(GFX_TOP, NULL);

	printf("WA3DS %s\n\n", APP_VERSION);
	printf("Bridge not configured yet.\n");
	printf("Press START to exit.\n");

	while (aptMainLoop())
	{
		hidScanInput();
		if (hidKeysDown() & KEY_START)
			break;
		gfxFlushBuffers();
		gfxSwapBuffers();
		gspWaitForVBlank();
	}

	gfxExit();
	return 0;
}
