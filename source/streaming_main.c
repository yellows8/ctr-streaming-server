#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <ctr/types.h>
#include <ctr/svc.h>
#include <ctr/srv.h>
#include <ctr/GSP.h>
#include <ctr/APT.h>
#include <ctr/HID.h>
#include <ctr/CSND.h>
#include <ctr/AC.h>
#include <ctr/SOC.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

u8* gspHeap;
u32* gxCmdBuf;
Handle gspEvent, gspSharedMemHandle;

void network_main();

extern u32 *fake_heap_start;
extern u32 *fake_heap_end;

void gspGpuInit()
{
	gspInit();

	GSPGPU_AcquireRight(NULL, 0x0);
	GSPGPU_SetLcdForceBlack(NULL, 0x0);

	//setup our gsp shared mem section
	u8 threadID;
	svc_createEvent(&gspEvent, 0x0);
	GSPGPU_RegisterInterruptRelayQueue(NULL, gspEvent, 0x1, &gspSharedMemHandle, &threadID);
	svc_mapMemoryBlock(gspSharedMemHandle, 0x10002000, 0x3, 0x10000000);

	//map GSP heap
	svc_controlMemory((u32*)&gspHeap, 0, 0, 0xc00000, 0x10003, 3);

	//wait until we can write stuff to it
	svc_waitSynchronization1(gspEvent, 0x55bcb0);

	//GSP shared mem : 0x2779F000
	gxCmdBuf=(u32*)(0x10002000+0x800+threadID*0x200);
}

void gspGpuExit()
{
	GSPGPU_UnregisterInterruptRelayQueue(NULL);

	//unmap GSP shared mem
	svc_unmapMemoryBlock(gspSharedMemHandle, 0x10002000);
	svc_closeHandle(gspSharedMemHandle);
	svc_closeHandle(gspEvent);
	
	gspExit();

	//free GSP heap
	svc_controlMemory((u32*)&gspHeap, (u32)gspHeap, 0x0, 0xc00000, MEMOP_FREE, 0x0);
}

int main(int argc, char **argv)
{
	void (*funcptr)(u32);
	Result ret;
	u32 tmp=0;
	u32 heap_size = 0;

	initSrv();

	aptInit(APPID_APPLICATION);
	aptSetupEventHandler();

	gspGpuInit();

	heap_size = 0xc00000;

	ret = svc_controlMemory(&tmp, 0x08000000, 0, heap_size, 0x3, 3);
	if(ret!=0)*((u32*)0x58000004) = ret;

	fake_heap_start = (u32*)0x08048000;
	fake_heap_end = (u32*)(0x08000000 + heap_size);

	ret = hidInit(NULL);
	if(ret!=0)
	{
		funcptr = (void*)0x44444948;
		funcptr(ret);
	}

	ret = CSND_initialize(NULL);
	if(ret!=0)
	{
		funcptr = (void*)0x444e5343;
		funcptr(ret);
	}

	ret = SOC_Initialize((u32*)0x08000000, 0x48000);
	if(ret!=0)
	{
		((u32*)0x84000000)[1] = ret;
	}

	network_main();

	hidExit();
	gspGpuExit();
	CSND_shutdown();
	SOC_Shutdown();
	aptExit();
	svc_exitProcess();

	return 0;
}

