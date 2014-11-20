#include <3ds.h>
#include <malloc.h>

void network_main();

int main()
{
	Result ret;
	u32 *ptr;

	// Initialize services
	srvInit();
	aptInit();
	hidInit(NULL);
	gfxInit();
	//gfxSet3D(true); // uncomment if using stereoscopic 3D

	ptr = (u32*)memalign(0x1000, 0x48000);
	if(ptr==NULL)((u32*)0x84000000)[9] = 0x20202020;

	ret = SOC_Initialize(ptr, 0x48000);
	if(ret!=0)
	{
		((u32*)0x84000000)[1] = ret;
	}

	network_main();

	// Exit services
	SOC_Shutdown();
	gfxExit();
	hidExit();
	aptExit();
	srvExit();
	return 0;
}

