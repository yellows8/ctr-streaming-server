#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>

/*#include <netinet/in.h>
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>*/

/*
Build with: gcc parse_hidstream.c -o parse_hidstream
Usage: "... | nc <3ds-serveraddr> <port> | parse_hidstream

This *nix app parses the 0x10c-byte blocks sent from the 3ds server, for simulating HID input. See the README for the 3ds server portion of this.
*/

int sock=0;

int uinput_fd=0;

int keymappings[12] = {
KEY_A, //0x1
KEY_B, //0x2
KEY_Q, //0x4
KEY_W, //0x8
KEY_RIGHT, //0x10
KEY_LEFT, //0x20
KEY_UP, //0x40
KEY_DOWN, //0x80
KEY_R, //0x100
BTN_LEFT, //0x200
KEY_X, //0x400
KEY_Y //0x800
};


void sigterm(int sig)
{
	//close(sock);

	if(ioctl(uinput_fd, UI_DEV_DESTROY) < 0)perror("uinput ioctl error: UI_DEV_DESTROY");
	close(uinput_fd);

	exit(0);
}

int init_uinput()
{
	int i;
	struct uinput_user_dev uidev;

	uinput_fd = open("/dev/input/uinput", O_WRONLY | O_NONBLOCK);
	if(uinput_fd < 0)
	{
		uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
		if(uinput_fd < 0)
		{
			perror("uinput error: open");
			return 1;
		}
	}

	if(ioctl(uinput_fd, UI_SET_EVBIT, EV_KEY) < 0)
	{
		perror("uinput ioctl error: UI_SET_EVBIT");
		return 1;
	}

	for(i=0; i<12; i++)
	{
		if(ioctl(uinput_fd, UI_SET_KEYBIT, keymappings[i]) < 0)
		{
			perror("uinput ioctl error: UI_SET_KEYBIT");
			return 1;
		}
	}

	if(ioctl(uinput_fd, UI_SET_EVBIT, EV_REL) < 0)
	{
		perror("uinput ioctl error: UI_SET_EVBIT");
		return 1;
	}

	if(ioctl(uinput_fd, UI_SET_RELBIT, REL_X) < 0)
	{
		perror("uinput ioctl error: UI_SET_RELBIT");
		return 1;
	}

	if(ioctl(uinput_fd, UI_SET_RELBIT, REL_Y) < 0)
	{
		perror("uinput ioctl error: UI_SET_RELBIT");
		return 1;
	}

	if(ioctl(uinput_fd, UI_SET_EVBIT, EV_ABS) < 0)
	{
		perror("uinput ioctl error: UI_SET_EVBIT");
		return 1;
	}

	if(ioctl(uinput_fd, UI_SET_ABSBIT, ABS_X) < 0)
	{
		perror("uinput ioctl error: UI_SET_ABSBIT");
		return 1;
	}

	if(ioctl(uinput_fd, UI_SET_ABSBIT, ABS_Y) < 0)
	{
		perror("uinput ioctl error: UI_SET_ABSBIT");
		return 1;
	}

	memset(&uidev, 0, sizeof(uidev));
	snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "uinput-3dshidstream");
	uidev.id.bustype = BUS_USB;
	uidev.id.vendor  = 0x1;
	uidev.id.product = 0x1;
	uidev.id.version = 1;
	uidev.absmin[ABS_X]=0;
	uidev.absmax[ABS_X]=320;
	uidev.absfuzz[ABS_X]=0;
	uidev.absflat[ABS_X]=320;
	uidev.absmin[ABS_Y]=0;
	uidev.absmax[ABS_Y]=240;
	uidev.absfuzz[ABS_Y]=0;
	uidev.absflat[ABS_Y]=240;

	if(write(uinput_fd, &uidev, sizeof(uidev)) < 0)
	{
		perror("uinput write error for device initialization");
		return 1;
	}

	if(ioctl(uinput_fd, UI_DEV_CREATE) < 0)
        {
		perror("uinput ioctl error: UI_DEV_CREATE");
		return 1;
	}

	return 0;
}

int simulate_keypress(int keycode)
{
	struct input_event ev;
	size_t ret=0;

	memset(&ev, 0, sizeof(ev));

	ev.type = EV_KEY;
	ev.code = keycode;
	ev.value = 1;

	ret = write(uinput_fd, &ev, sizeof(ev));

	if(ret<0)
	{
		perror("uinput write error");
		sigterm(0);
	}

	return 0;
}

int simulate_keyrelease(int keycode)
{
	struct input_event ev;
	size_t ret=0;

	memset(&ev, 0, sizeof(ev));

	ev.type = EV_KEY;
	ev.code = keycode;
	ev.value = 0;

	ret = write(uinput_fd, &ev, sizeof(ev));

	if(ret<0)
	{
		perror("uinput write error");
		sigterm(0);
	}

	return 0;
}

int simulate_keypressrelease(int keycode)
{
	simulate_keypress(keycode);
	simulate_keyrelease(keycode);
}

int recvdata(int sockfd, unsigned char *buf, int size)
{
	int ret, pos=0;
	int tmpsize=size;

	while(tmpsize)
	{
		if((ret = read(STDIN_FILENO, &buf[pos], tmpsize))<=0)return ret;
		//if((ret = recv(sockfd, &buf[pos], tmpsize, 0))<=0)return ret;
		pos+= ret;
		tmpsize-= ret;
	}

	return size;
}

int main(int argc, char **argv)
{
	int ret;
	//struct sockaddr_in addr;
	unsigned char hid8[0x10c];
	unsigned int *hid32 = (unsigned int*)hid8;
	signed short tmp0, tmp1;
	unsigned int prev_hidstate = 0, cur_hidstate = 0;
	unsigned int pressed_hidstate = 0, released_hidstate = 0;
	signed short cur_padx=0, cur_pady=0;
	signed short prev_padx=0, prev_pady=0;
	signed short padx_diff, pady_diff;
	int i;

	struct input_event ev;

	/*sock = socket(AF_INET, SOCK_STREAM, 0);
	if(sock<0)return 1;

	addr.sin_family = AF_INET;
	addr.sin_port = 0x8d20;// 0x3905 = big-endian 1337. 0x8d20 = big-endian 8333.
	addr.sin_addr.s_addr= 0x2801a8c0;//0x2801a8c0 = 192.168.1.40.
	ret = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
	if(ret<0)return 1;*/

	if(init_uinput()!=0)
	{
		//close(sock);
		return 1;
	}

	signal(SIGTERM, sigterm);

	while(1)
	{
		ret = recvdata(sock, hid8, 0x10c);
		if(ret<=0)sigterm(0);

		tmp0 = hid32[0x34>>2] & 0xffff;
		tmp1 = (hid32[0x34>>2] >> 16) & 0xffff;

		printf("HID state: %x\n", hid32[0x1c>>2]);
		printf("Circle-pad state: X = %d, Y = %d\n", (int)tmp0, (int)tmp1);

		if(hid32[0xcc>>2]==0)
		{
			printf("Touch screen isn't being pressed.\n");
		}
		else
		{
			printf("Touch screen: X = %u, Y = %u\n", hid32[0xc8>>2] & 0xffff, (hid32[0xc8>>2] >> 16) & 0xffff);
		}

		printf("3D slider state: %f\n", *((float*)&hid8[0x108]));

		prev_hidstate = cur_hidstate;
		cur_hidstate = hid32[0x1c>>2];

		pressed_hidstate = cur_hidstate & ~prev_hidstate;
		released_hidstate = prev_hidstate & ~cur_hidstate;

		prev_padx = cur_padx;
		prev_pady = cur_pady;
		cur_padx = hid32[0x34>>2] & 0xffff;
		cur_pady = (hid32[0x34>>2] >> 16) & 0xffff;
		padx_diff = cur_padx - prev_padx;
		pady_diff = (-cur_pady) - (-prev_pady);

		for(i=0; i<12; i++)
		{
			if((pressed_hidstate >> i) & 1)simulate_keypress(keymappings[i]);
		}

		for(i=0; i<12; i++)
		{
			if((released_hidstate >> i) & 1)simulate_keyrelease(keymappings[i]);
		}

		/*for(i=0; i<12; i++)
		{
			if((pressed_hidstate >> i) & 1)simulate_keypressrelease(keymappings[i]);
		}*/

		/*for(i=0; i<12; i++)
		{
			if((cur_hidstate >> i) & 1)simulate_keypressrelease(keymappings[i]);
		}*/

		if(padx_diff!=0 || pady_diff!=0)
		{
			memset(&ev, 0, sizeof(struct input_event));
			ev.type = EV_REL;
			ev.code = REL_X;
			ev.value = (int)padx_diff;
			if(write(uinput_fd, &ev, sizeof(struct input_event)) < 0)
			{
				perror("uinput write error for EV_REL REL_X");
				sigterm(0);
			}

			memset(&ev, 0, sizeof(struct input_event));
			ev.type = EV_REL;
			ev.code = REL_Y;
			ev.value = (int)pady_diff;
			if(write(uinput_fd, &ev, sizeof(struct input_event)) < 0)
			{
				perror("uinput write error for EV_REL REL_Y");
				sigterm(0);
			}
		}

		if(hid32[0xcc>>2]!=0)
		{
			memset(&ev, 0, sizeof(struct input_event));
			ev.type = EV_ABS;
			ev.code = ABS_X;
			ev.value = (int)(hid32[0xc8>>2] & 0xffff);
			if(write(uinput_fd, &ev, sizeof(struct input_event)) < 0)
			{
				perror("uinput write error for EV_ABS ABS_X");
				sigterm(0);
			}

			memset(&ev, 0, sizeof(struct input_event));
			ev.type = EV_ABS;
			ev.code = ABS_Y;
			ev.value = (int)((hid32[0xc8>>2] >> 16) & 0xffff);
			if(write(uinput_fd, &ev, sizeof(struct input_event)) < 0)
			{
				perror("uinput write error for EV_ABS ABS_Y");
				sigterm(0);
			}
		}

		if(cur_hidstate || pressed_hidstate || released_hidstate || padx_diff!=0 || pady_diff!=0 || hid32[0xcc>>2]!=0)
		{
			memset(&ev, 0, sizeof(struct input_event));
			ev.type = EV_SYN;
			ev.code = 0;
			ev.value = 0;
			if(write(uinput_fd, &ev, sizeof(struct input_event)) < 0)
			{
				perror("uinput write error for SYN");
				sigterm(0);
			}
		}
	}

	return 0;
}

