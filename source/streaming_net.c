#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <3ds.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>

#include <vpx/vpx_config.h>
#define VPX_CODEC_DISABLE_COMPAT 1
#include <vpx/vpx_decoder.h>
#include <vpx/vp8dx.h>
#define interface_vp8 (vpx_codec_vp8_dx())

#include "yuv2rgb.h"

#include <vorbis/vorbisfile.h>

#define IVF_FILE_HDR_SZ  (32)
#define IVF_FRAME_HDR_SZ (12)

typedef struct
{
	volatile int sockfd;
} ctrserver;

struct datablock_entry
{
	u8 *volatile data;
	volatile u32 datasize;
	struct datablock_entry *volatile next;
};

int listen_sock;
ctrserver net_server;

struct datablock_entry *volatile first_datablockentry = NULL;

#define THREAD_STACKSIZE 0x1000
u64 network_threadstack[THREAD_STACKSIZE>>3];

size_t recv_fread(void *ptr, size_t size, size_t nmemb, void* fd);

static ov_callbacks ov_callbacks_recvstream = {
  (size_t (*)(void *, size_t, size_t, void *))  recv_fread,
  (int (*)(void *, ogg_int64_t, int))           NULL,
  (int (*)(void *))                             NULL,
  (long (*)(void *))                            NULL
};

static u8 *gspHeap = NULL;

static volatile int media_mode = MEDIAMODE;//0 = video, 1 = audio.
static volatile int enable_codec = CODEC;//0 = disable, 1 = enable.
static volatile int enable_hidreport = HIDREPORT;//0 = disable, 1 = enable.

static volatile int audio_started = 0, video_started = 0;
static volatile unsigned int recvpos=0, recvsize=0, maxsize=0;
static volatile unsigned int framebuf_pos=0;
static volatile unsigned int recvchunk=0;
static volatile u64 prevtick=0, curtick=0;
static volatile unsigned int total_recvdata = 0, total_framebufdata = 0;
static volatile unsigned int pixsz = 3;

static volatile int networkthread_terminateflag = 0;

int ctrserver_recvdata(ctrserver *server, u8 *buf, int size)
{
	int ret, pos=0;
	int tmpsize=size;

	while(tmpsize)
	{
		if((ret = recv(server->sockfd, &buf[pos], tmpsize, 0))<=0)
		{
			if(ret<0)ret = SOC_GetErrno();
			if(ret == -EWOULDBLOCK)continue;
			return ret;
		}
		pos+= ret;
		tmpsize-= ret;
	}

	return size;
}

int ctrserver_senddata(ctrserver *server, u8 *buf, int size)
{
	int ret, pos=0;
	int tmpsize=size;

	while(tmpsize)
	{
		if((ret = send(server->sockfd, &buf[pos], tmpsize, 0))<0)
		{
			ret = SOC_GetErrno();
			if(ret == -EWOULDBLOCK)continue;
			return ret;
		}
		pos+= ret;
		tmpsize-= ret;
	}

	return size;
}

size_t recv_fread(void *ptr, size_t size, size_t nmemb, void* fd)
{
	ctrserver *server = (ctrserver*)fd;
	int ret;

	ret = ctrserver_recvdata(server, (u8*)ptr, (int)(size*nmemb));
	if(ret<0)ret = 0;

	return (size_t)ret;
}

int process_stream_connection(ctrserver *server)
{
	int ret = 0;
	unsigned int size=0;
	u8 *heap = gspHeap;

	OggVorbis_File vf;
	int bs = 0;
	struct datablock_entry *volatile cur_entry = NULL, *volatile next_entry = NULL;
	u8 *audio_dataptr;

	u32 outdata[0x10c>>2];

	u32 ivf_filehdr[0x20>>2];
	u32 ivf_framehdr[0xc>>2];

	if(!media_mode)
	{
		maxsize = 0x700000;
		recvchunk = 240*320*pixsz;
	}
	else
	{
		maxsize = 0x700000;
		recvchunk = 44100*2;
	}

	recvsize = recvchunk;
	recvpos = 0;

	framebuf_pos=0;
	total_recvdata = 0;
	total_framebufdata = 0;

	memset(heap, 0, maxsize);
	GSPGPU_FlushDataCache(NULL, heap, maxsize);

	if(!media_mode)video_started = 1;

	if(enable_codec && !media_mode)
	{
		ret = ctrserver_recvdata(server, (u8*)ivf_filehdr, 0x20);
		if(ret<=0)return ret;

		if(ivf_filehdr[0]!=0x46494b44)return 2;//FourCC for .ivf: DKIF
	}

	if(media_mode)
	{
		if(enable_codec)
		{
			if(ov_open_callbacks(server, &vf, NULL, 0, ov_callbacks_recvstream) < 0)return 7;
		}

		if(first_datablockentry)
		{
			cur_entry = first_datablockentry;
			while(cur_entry)
			{
				if(cur_entry->data)free(cur_entry->data);
				cur_entry->data = NULL;
				cur_entry->datasize = 0;

				next_entry = cur_entry->next;
				free(cur_entry);
				cur_entry = next_entry;
			}
		}

		first_datablockentry = (struct datablock_entry*)malloc(sizeof(struct datablock_entry));
		memset(first_datablockentry, 0, sizeof(struct datablock_entry));
		
		cur_entry = first_datablockentry;
	}

	while(1)
	{
		if(enable_codec && !media_mode)
		{
			ret = ctrserver_recvdata(server, (u8*)ivf_framehdr, 0xc);
			if(ret<=0)return ret;

			memcpy(&heap[recvpos], ivf_framehdr, 0xc);

			size = (unsigned int)ret;

			if(ivf_framehdr[0] + recvpos > maxsize)
			{
				recvpos=0;//return 3;
				total_recvdata+= 0xc;
				size = 0;

				if(ivf_framehdr[0] > maxsize)return 5;
			}

			ret = ctrserver_recvdata(server, &heap[recvpos+0xc], ivf_framehdr[0]);
			if(ret<=0)return ret;

			size+= (unsigned int)ret;
		}

		if(media_mode)
		{
			cur_entry->data = NULL;
			while(cur_entry->data==NULL)cur_entry->data = (u8*)malloc(recvchunk);

			audio_dataptr = cur_entry->data;

			cur_entry->next = NULL;
			while(cur_entry->next==NULL)cur_entry->next = (struct datablock_entry*)malloc(sizeof(struct datablock_entry));
			memset(cur_entry->next, 0, sizeof(struct datablock_entry));

			if(enable_codec)
			{
				size = recvchunk;
				while(size)
				{
					ret = ov_read(&vf, (char*)audio_dataptr, size, 0, 2, 1, &bs);
					if(bs!=0)
					{
						return 8;//From oggdec.c: fprintf(stderr, _("Logical bitstreams with changing parameters are not supported\n"));
					}

					if(ret<=0)break;

					size-= ret;
					audio_dataptr+= ret;
				}
			}
			else
			{
				ret = ctrserver_recvdata(server, audio_dataptr, recvsize);
			}

			if(ret<=0)
			{
				video_started = 0;
				audio_started = 0;

				return ret;
			}

			cur_entry->datasize = recvchunk;
			cur_entry = cur_entry->next;

			size = (unsigned int)ret;
		}

		recvpos+= size;
		recvsize-= size;
		total_recvdata+= size;

		if(prevtick==0)prevtick = svcGetSystemTick();
		curtick = svcGetSystemTick();

		if(curtick - prevtick >= 268123480/60 && !media_mode && enable_hidreport)
		{
			prevtick = curtick;

			memset(outdata, 0, 0x10c);
			memcpy(outdata, (u32*)hidSharedMem, 0x108);
			outdata[0x108>>2] = *((u32*)0x1FF81080);//3D_SLIDERSTATE

			ret = ctrserver_senddata(server, (u8*)outdata, 0x10c);
			//if(ret<0)return ret;
		}

		if(recvsize==0 || enable_codec)
		{
			recvsize = recvchunk;

			if(recvpos+recvsize>=maxsize)
			{
				recvpos = 0;
			}
		}
	}

	return 1;
}

void network_initialize()
{
	int ret=0;
	int tmp=0;
	struct sockaddr_in addr;

	listen_sock = socket(AF_INET, SOCK_STREAM, 0);
	if(listen_sock<0)
	{
		((u32*)0x84000000)[3] = (u32)listen_sock;
		return;
	}

	tmp = fcntl(listen_sock, F_GETFL);
	fcntl(listen_sock, F_SETFL, tmp | O_NONBLOCK);

	addr.sin_family = AF_INET;
	addr.sin_port = 0x8e20;//0x8e20 = big-endian 8334.
	addr.sin_addr.s_addr = INADDR_ANY;
	ret = bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr));
	if(ret<0)
	{
		((u32*)0x84000000)[5] = (u32)SOC_GetErrno();
		return;
	}

	ret = listen(listen_sock, 1);
	if(ret==-1)ret = SOC_GetErrno();
	if(ret<0)
	{
		((u32*)0x84000000)[8] = (u32)ret;
		return;
	}
}

void displayupdate_framebuffer(u8 *heap)
{
	// Flush and swap framebuffers
	gfxFlushBuffers();
	gfxSwapBuffers();
}

void network_thread()
{
	//int ret;
	u32 i;
	u8 *heap = gspHeap;
	u8 *framebufptr;
	unsigned int size = recvchunk;
	u32 width = 240, height = 320;
	//u32 tmpsize;
	//u32 flag=0;
	u32 pos, ypos;
	int framebuf_order = 1;

	vpx_codec_ctx_t  codec;
	int              flags = 0;//, frame_cnt = 0;
	//vpx_codec_err_t  res;

	int frame_size=0;
        vpx_codec_iter_t  iter = NULL;
        vpx_image_t *img;

	int decode_success = 1;

	u8 *rgb_buf;
	u8 *planedata[3];
	u32 planewidth[3];
	u32 planesize[3];

	struct datablock_entry *volatile cur_entry, *volatile next_entry = NULL;
	u32 audio_playback_status = 0;

	//setup_fpscr();

	if(!media_mode)
	{
		if(vpx_codec_dec_init(&codec, interface_vp8, NULL, flags))
		{
			((u32*)0x84000000)[4] = 0x544e4943;
			//svcExitThread();
		}
	}

	planedata[0] = NULL;
	planedata[1] = NULL;
	planedata[2] = NULL;
	rgb_buf = NULL;
	memset(planesize, 0, sizeof(planesize));

	while(1)
	{
		if(planedata[0] && !media_mode)
		{
			free(planedata[0]);
			free(planedata[1]);
			free(planedata[2]);
			free(rgb_buf);

			planedata[0] = NULL;
			planedata[1] = NULL;
			planedata[2] = NULL;
			rgb_buf = NULL;
		}

		while(net_server.sockfd==0)
		{
			if(networkthread_terminateflag)break;
		}

		if(networkthread_terminateflag)break;

		if(!media_mode)while(!video_started);

		if(media_mode)
		{
			while(first_datablockentry==NULL);
			cur_entry = first_datablockentry;
		}

		if(networkthread_terminateflag)break;

		while(net_server.sockfd!=0 || (!media_mode && total_framebufdata < total_recvdata))
		{

			if(prevtick==0)prevtick = svcGetSystemTick();
			curtick = svcGetSystemTick();

			if(media_mode)
			{
				while(cur_entry->data==NULL);
				while(cur_entry->datasize==0);

				memcpy(heap, cur_entry->data, cur_entry->datasize);
				GSPGPU_FlushDataCache(NULL, heap, cur_entry->datasize);

				CSND_playsound(0x8, CSND_LOOP_DISABLE, CSND_ENCODING_PCM16, 44100, (u32*)heap, NULL, cur_entry->datasize, 2, 0);

				free(cur_entry->data);
				cur_entry->data = NULL;
				cur_entry->datasize = 0;

				while(cur_entry->next==NULL);
				next_entry = cur_entry->next;
				free(cur_entry);
				cur_entry = next_entry;

				audio_playback_status = 1;

				while(1)
				{
					while(CSND_getchannelstate_isplaying(0, (u8*)&audio_playback_status)!=0);
					if(audio_playback_status==0)break;

					//svcSleepThread(16666667);
					svcSleepThread(1000000);
				}
			}

			if(!media_mode)
			{
				while(total_framebufdata >= total_recvdata);

				prevtick = curtick;

				decode_success = 1;

				if(!enable_codec && !media_mode)
				{
					framebufptr = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
					memcpy(framebufptr, &heap[framebuf_pos], size);
				}
				else
				{
					frame_size = *((unsigned int*)&heap[framebuf_pos]);

					size = 0xc;

					if(framebuf_pos + frame_size > maxsize)
					{
						size = 0;
						framebuf_pos = 0;
						total_framebufdata+= 0xc;
					}

					if(vpx_codec_decode(&codec, &heap[framebuf_pos+0xc], frame_size, NULL, 0))decode_success = 0;

					iter = NULL;

					if(decode_success)
					{
						while((img = vpx_codec_get_frame(&codec, &iter)))
						{
							if(planedata[0]==NULL)
							{
								width = img->d_w;
								height = img->d_h;

								planewidth[0] = width;
								planewidth[1] = width>>1;
								planewidth[2] = planewidth[1];
								planesize[0] = planewidth[0]*height;
								planesize[1] = planewidth[1]*height;
								planesize[2] = planesize[1];

								planedata[0] = (u8*)malloc(planesize[0]);
								planedata[1] = (u8*)malloc(planesize[1]);
								planedata[2] = (u8*)malloc(planesize[2]);

								rgb_buf = (u8*)malloc(width*height*pixsz);
							}

							memset(planedata[0], 0, planesize[0]);
							memset(planedata[1], 0, planesize[1]);
							memset(planedata[2], 0, planesize[2]);
							memset(rgb_buf, 0, width*height*pixsz);

							for(i=0; i<3; i++)
							{
								pos = 0;
								for(ypos=0; ypos<height; ypos++)
								{
									memcpy(&planedata[i][ypos*planewidth[i]], &img->planes[i][pos], planewidth[i]);
									pos+= img->stride[i];
								}
							}

							if(pixsz==2)yuv420_2_rgb565(rgb_buf, planedata[0], planedata[1], planedata[2], width, height, width, width>>1, width*pixsz, yuv2rgb565_table, 0);
							if(pixsz==3)yuv420_2_rgb888(rgb_buf, planedata[0], planedata[1], planedata[2], width, height, width, width>>1, width*pixsz, yuv2rgb565_table, 0);

							vpx_img_free(img);
							if(height==320)
							{
								framebuf_order = 1;
							}
							else
							{
								framebuf_order = 0;
							}

							gfxSet3D(false);

							if(framebuf_order == 1)
							{
								framebufptr = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
								memcpy(framebufptr, rgb_buf, width*320*pixsz);
							}
							else if(height==400)
							{
								framebufptr = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
								memcpy(framebufptr, rgb_buf, width*400*pixsz);
							}
							else if(height==320+400)
							{
								framebufptr = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
								memcpy(framebufptr, rgb_buf, width*400*pixsz);
								framebufptr = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
								memcpy(framebufptr, &rgb_buf[width*400*pixsz], width*320*pixsz);
							}
							else if(height>=800)
							{
								gfxSet3D(true);

								framebufptr = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
								memcpy(framebufptr, rgb_buf, width*400*pixsz);
								framebufptr = gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, NULL, NULL);
								memcpy(framebufptr, &rgb_buf[width*400*pixsz], width*400*pixsz);
								framebufptr = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
								if(height==800+320)memcpy(framebufptr, &rgb_buf[width*400*pixsz*2], width*320*pixsz);
							}

							displayupdate_framebuffer(heap);
						}
					}

					size += frame_size;
				}

				framebuf_pos+= size;
				total_framebufdata += size;
				if(framebuf_pos+recvchunk>=maxsize)framebuf_pos = 0;

				if(!enable_codec && !media_mode)
				{
					displayupdate_framebuffer(heap);
				}
			}

		}

		if(media_mode)
		{
			CSND_setchannel_playbackstate(0x8, 0);
			CSND_sharedmemtype0_cmdupdatestate(0);

			/*while(cur_entry)
			{
				//if(cur_entry->data)free(cur_entry->data);
				cur_entry->data = NULL;
				cur_entry->datasize = 0;

				next_entry = cur_entry->next;
				//free(cur_entry);
				cur_entry = next_entry;
			}*/
		}

		//while(net_server.sockfd!=0);
	}

	networkthread_terminateflag = 2;

	svcExitThread();
}

void network_main()
{
	int ret;
	u32 threadhandle;

	net_server.sockfd = 0;

	gspHeap = linearAlloc(0xc00000);
	if(gspHeap==NULL)((u32*)0x84000000)[21] = 0x99;

	if(media_mode)
	{
		ret = CSND_initialize(NULL);
		if(ret!=0)
		{
			((u32*)0x84000000)[20] = (u32)ret;
		}
	}

	aptOpenSession();
	ret = APT_SetAppCpuTimeLimit(NULL, 30);//Using 80 on old3ds results in the below thread not running correctly?(or this app doesn't work right then etc...) With 30 video decoding+playback is slower than just running on appcore maybe?
	aptCloseSession();

	ret = svcCreateThread(&threadhandle, network_thread, 0, (u32*)&network_threadstack[THREAD_STACKSIZE>>3], 0x18, 1);

	network_initialize();

	while (aptMainLoop())
	{
		gspWaitForVBlank();
		hidScanInput();

		// Your code goes here

		u32 kDown = hidKeysDown();
		if (kDown & KEY_START)
			break; // break in order to return to hbmenu

		net_server.sockfd = accept(listen_sock, NULL, NULL);
		if(net_server.sockfd==-1)net_server.sockfd = SOC_GetErrno();
		if(net_server.sockfd<0)
		{
			if(net_server.sockfd == -EWOULDBLOCK)continue;
			((u32*)0x84000000)[9] = (u32)net_server.sockfd;
			break;
		}

		ret = process_stream_connection(&net_server);
		if(ret>1 || ret<0)
		{
			//if(ret!=-1)((u32*)0x84000000)[10] = ret;
			//if(ret==-1)((u32*)0x84000000)[11] = SOC_GetErrno();
		}

		closesocket(net_server.sockfd);
		net_server.sockfd = 0;
	}

	networkthread_terminateflag = 1;
	svcWaitSynchronization(threadhandle, U64_MAX);

	if(media_mode)CSND_shutdown();
}

