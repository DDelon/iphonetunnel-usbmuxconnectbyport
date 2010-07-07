﻿// iTunnel mod with restore mode comm support
// Based on iPhone_tunnel by novi (novi.mad@gmail.com ) http://novis.jimdo.com
// thanks
// http://i-funbox.com/blog/2008/09/itunesmobiledevicedll-changed-in-itunes-80/
// 2010 msftguy

#include "platform.h"
#include "MyMobileDevice.h"

#include "itunes_private.h"

enum exit_status {
	EXIT_SUCCESS_I			= 0x00,
	EXIT_DISCONNECTED		= 0x01,
	EXIT_CONNECT_ERROR		= 0x02,
	EXIT_SERVICE_ERROR		= 0x03,
	EXIT_BIND_ERROR			= 0x04,
	EXIT_GENERAL_ERROR		= 0x05,
	EXIT_OS_TOO_FUCKING_OLD = 0x06,
	EXIT_REGISTRY_ERROR		= 0x07,
	EXIT_LOAD_ERROR			= 0x08,
	EXIT_BAD_OPTIONS		= 0x09,
	EXIT_BAD_OPTION_COMBO	= 0x0A,
};


typedef enum PROGRAM_MODE {
	MODE_NONE,
	MODE_TUNNEL, 
	MODE_AUTOBOOT,
	MODE_ICMD,
} PROGRAM_MODE;

static PROGRAM_MODE g_programMode = MODE_NONE;

static char g_ibss[BUFSIZ] = "";
static char g_exploit[BUFSIZ] = "";
static char g_ibec[BUFSIZ] = "";
static char g_ramdisk[BUFSIZ] = "";
static char g_devicetree[BUFSIZ] = "";
static char g_kernelcache[BUFSIZ] = "";
static bool g_autoboot = FALSE;

typedef char OPTION_T [BUFSIZ];


#pragma mark Prototype definition

#define TRUE 1
#define FALSE 0
#define BUFFER_SIZE 256
#define MAX_SOCKETS 512

#define SOCKET_ERROR -1

struct connection
{
	int from_handle;
	int to_handle;
};

void* THREADPROCATTR wait_for_device(void*);
void wait_connections();
void notification(struct am_device_notification_callback_info*);
void*THREADPROCATTR conn_forwarding_thread(void* arg);

static int threadCount = 0;

static int  sock;

static muxconn_t muxConn = 0;

static const char* target_device_id = nil;

static am_device_t target_device = NULL;

#if !WIN32
void recv_signal(int sig)
{
	printf("Info: Signal received. (%d)\n", sig);

	fflush(stdout);
	signal(sig, SIG_DFL);
	raise(sig);
}
#endif

#pragma mark Main function

#if WIN32 
	const unsigned short default_local_port = 22;
#else
	const unsigned short default_local_port = 2022;
#endif

int g_iphone_port = 22;
int g_local_port = default_local_port;

void print_error(int error = 0) {
	int err = error != 0 ? error :
#if WIN32
		GetLastError();
#else
		errno;
#endif
	printf("Error 0x%X (%i): '%s'\n", err, err, strerror(err));
}

typedef enum ICMD_STATE {
	ICMD_ZERO,
	ICMD_SENT_IBSS,
	ICMD_SENT_EXPLOIT,
	ICMD_SENT_IBEC,
	ICMD_SENT_RAMDISK,
	ICMD_SENT_DEVICETREE,
	ICMD_SENT_KERNELCACHE,
} ICMD_STATE;

static ICMD_STATE g_icmdState = ICMD_ZERO;

void dfu_connect_callback(AMRecoveryModeDevice device) 
{
	printf("dfu_connect_callback\n");
	if (*g_ibss) {
		uploadFileDfu(device, g_ibss);
		Log(LOG_INFO, "iBSS %s loaded", g_ibss);
	}
	g_icmdState = ICMD_SENT_IBSS;
}

void dfu_disconnect_callback(AMRecoveryModeDevice device) 
{
	printf("dfu_disconnect_callback\n");	
}

void recovery_connect_callback(AMRecoveryModeDevice device) 
{
	printf("recovery_connect_callback\n");
	switch (g_programMode) {
	case MODE_AUTOBOOT:
		AMRecoveryModeDeviceSetAutoBoot(device, true);
		AMRecoveryModeDeviceReboot(device);	
		break;
	case MODE_ICMD:
		if (g_icmdState == ICMD_ZERO) {
			if (!*g_ibss) {
				g_icmdState = ICMD_SENT_IBSS;
			}
		}
		if (g_icmdState == ICMD_SENT_IBSS)  {
			if (*g_exploit) {
				uploadUsbExploit(device, g_exploit);
				Log(LOG_INFO, "Payload %s sent", g_exploit);
			}
			g_icmdState = ICMD_SENT_EXPLOIT;
		}
		if (g_icmdState == ICMD_SENT_EXPLOIT)  {
			g_icmdState = ICMD_SENT_IBEC;
			if (*g_ibec) {
				uploadFile(device, g_ibec);
				sendCommand(device, "go");
				Log(LOG_INFO, "iBEC %s loaded", g_ibec);
				break; // continue after reconnect
			}
		}
		if (g_icmdState == ICMD_SENT_IBEC) {
			if (*g_ramdisk) {
				uploadFile(device, g_ramdisk);
				sendCommand(device, "ramdisk");
				Log(LOG_INFO, "Ramdisk %s loaded", g_ramdisk);
			}
			g_icmdState = ICMD_SENT_RAMDISK;
		}
		if (g_icmdState == ICMD_SENT_RAMDISK) {
			if (*g_devicetree) {
				uploadFile(device, g_devicetree);
				sendCommand(device, "devicetree");
				Log(LOG_INFO, "Devicetree %s loaded", g_devicetree);
			}
			g_icmdState = ICMD_SENT_DEVICETREE;
		}
		if (g_icmdState == ICMD_SENT_DEVICETREE) {
			if (*g_kernelcache) {
				uploadFile(device, g_kernelcache);
				sendCommand(device, "bootx");
				Log(LOG_INFO, "Kernelcache %s loaded", g_kernelcache);
			}
			g_icmdState = ICMD_SENT_KERNELCACHE;
		}
	}
}

void recovery_disconnect_callback(AMRecoveryModeDevice device) 
{
	printf("recovery_disconnect_callback\n");
	if (g_icmdState == ICMD_SENT_KERNELCACHE) {
		g_icmdState = ICMD_ZERO;
	}
}

void register_for_recovery_notifications()
{
	printf("Will try to kick connected devices out of the Recovery mode..\n");
	AMRestoreRegisterForDeviceNotifications(dfu_connect_callback, recovery_connect_callback, dfu_disconnect_callback, recovery_disconnect_callback, 0, NULL);
	Sleep(-1);
}

int parse_args(int argc, char *argv [])
{
	char** pArg;
	for (pArg = argv + 1; pArg < argv + argc; ++pArg) {
		const char* arg = *pArg;
		OPTION_T* pOpt = NULL;
		int* pIntOpt = NULL;
		PROGRAM_MODE newMode = MODE_NONE;
		if (!strcmp(arg, "--tunnel")) {
			newMode = MODE_TUNNEL;
		} if (!strcmp(arg, "--ibss")) {
			pOpt = &g_ibss; newMode = MODE_ICMD;
		} else if (!strcmp(arg, "--exploit")) {
			pOpt = &g_exploit; newMode = MODE_ICMD;
		} else if (!strcmp(arg, "--ibec")) {
			pOpt = &g_ibec; newMode = MODE_ICMD;
		} else if (!strcmp(arg, "--ramdisk")) {
			pOpt = &g_ramdisk; newMode = MODE_ICMD;
		} else if (!strcmp(arg, "--devicetree")) {
			pOpt = &g_devicetree; newMode = MODE_ICMD;
		} else if (!strcmp(arg, "--kernelcache")) {
			pOpt = &g_kernelcache; newMode = MODE_ICMD;
		} else if (!strcmp(arg, "--autoboot")) {
			g_autoboot = true; newMode = MODE_AUTOBOOT;
		} else if (!strcmp(arg, "--iport")) {
			pIntOpt = &g_iphone_port; newMode = MODE_TUNNEL;
		} else if (!strcmp(arg, "--lport")) {
			pIntOpt = &g_local_port; newMode = MODE_TUNNEL;
		} else {
			return EXIT_BAD_OPTIONS;
		}

		if (newMode != MODE_NONE && newMode != g_programMode) {
			if (g_programMode == MODE_NONE) 
				g_programMode = newMode;
			else
				return EXIT_BAD_OPTION_COMBO;
		}
		if (pOpt != NULL || pIntOpt != NULL) {
			if (pArg + 1 >= argv + argc) {
				return EXIT_BAD_OPTIONS;
			}
			++pArg;
			if (pOpt) {
				strncpy(*pOpt,  *pArg, sizeof(OPTION_T));
			} else if (pIntOpt) {
				if (sscanf(*pArg, "%i", pIntOpt) != 1) {
					return EXIT_BAD_OPTIONS;
				}
			}
		}
	}
	return 0;
}


void usage()
{
printf(
			"\niphone_tunnel v2.0 for Win/Mac\n"
			"Created by novi. (novi.mad@gmail.com)\n"
			"Restore mode hack by msft.guy ((rev 5))\n"
			"\n"
			"Usage: iphone_tunnel --tunnel [--iport <iPhone port>] [--lport <Local port>] [Device ID, 40 digit]]\n"
			"OR: iphone_tunnel --autoboot to kick out of the recovery mode\n"
			"OR: iphone_tunnel [--ibss <iBSS file>] [--exploit <iBSS USB exploit payload>]\n"
			"\t[--ibec <iBEC file>] [--ramdisk <ramdisk file>]\n"
			"\t[--devicetree <devicetree file>] [--kernelcache <kernelcache file>]\n"
			"Example: iphone_tunnel 22 9876 0123456...abcdef\n"
			"Default ports are 22 %hu\n", default_local_port
			);
}

#if WIN32
void platform_init() {

	WSADATA useless;
	WSAStartup(WINSOCK_VERSION, &useless);

	WCHAR wbuf[MAX_PATH];
	DWORD cbBuf = sizeof(wbuf);

	//for necrophiles - XP support
	typedef LSTATUS
		(APIENTRY
		*RegGetValue_t) (
			__in HKEY    hkey,
			__in_opt LPCWSTR  lpSubKey,
			__in_opt LPCWSTR  lpValue,
			__in_opt DWORD    dwFlags,
			__out_opt LPDWORD pdwType,
			__out_bcount_part_opt(*pcbData,*pcbData) PVOID   pvData,
			__inout_opt LPDWORD pcbData
			);

	RegGetValue_t rgv = (RegGetValue_t)GetProcAddress(LoadLibraryW(L"advapi32"), "RegGetValueW");
	if  (rgv == nil) {
		rgv = (RegGetValue_t)GetProcAddress(LoadLibraryW(L"Shlwapi"), "SHRegGetValueW");
	}
	if  (rgv == nil) {
		printf("Your OS is too fucking old!\n");
		exit(EXIT_OS_TOO_FUCKING_OLD);
	}
	//End XP support 
	HRESULT error = S_OK;

	error = rgv(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Apple Inc.\\Apple Application Support", L"InstallDir", RRF_RT_REG_SZ|RRF_ZEROONFAILURE, NULL, (LPBYTE)wbuf, &cbBuf);
	if (ERROR_SUCCESS != error) {
		print_error(error);
		printf("Could not locate 'Apple Application Support' folder path in registry: ABORTING\n");
		exit(EXIT_REGISTRY_ERROR);
	}

	SetDllDirectoryW(wbuf);

	if (!LoadLibraryW(L"ASL")) {
		print_error();
		printf("WARNING: Could not load ASL from '%ws'\n", wbuf);
	}

	if (!LoadLibraryW(L"CoreFoundation")) {
		print_error();
		printf("Could not load CoreFoundation from '%ws': ABORTING\n", wbuf);
		exit(EXIT_LOAD_ERROR);
	}

	cbBuf = sizeof(wbuf);
	error = rgv(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Apple Inc.\\Apple Mobile Device Support", L"InstallDir", RRF_RT_REG_SZ|RRF_ZEROONFAILURE, NULL, (LPBYTE)wbuf, &cbBuf);
	if (ERROR_SUCCESS != error) {
		print_error(error);
		printf("Could not locate 'Apple Mobile Device Support' folder path in registry: ABORTING\n");
		exit(EXIT_REGISTRY_ERROR);
	}
	wcscat_s<MAX_PATH>(wbuf, L"\\iTunesMobileDevice.dll");
	if (!LoadLibraryW(wbuf)) {
		print_error();
		printf("Could not load %ws: ABORTING\n", wbuf);
		exit(EXIT_LOAD_ERROR);
	}
}
#else // OS X
void platform_init() {
	signal(SIGABRT, recv_signal);
	signal(SIGILL, recv_signal);
	signal(SIGINT, recv_signal);
	signal(SIGSEGV, recv_signal);
	signal(SIGTERM, recv_signal);
}
#endif

int main (int argc, char *argv [])
{
	platform_init();
	
	int ret = parse_args(argc, argv);
	if (ret != 0 || argc == 1  || g_programMode == MODE_NONE) {
		usage();
		return ret;
	}

	switch (g_programMode) {
	case MODE_AUTOBOOT:
	case MODE_ICMD:
		register_for_recovery_notifications();
		break;
	case MODE_TUNNEL:
		wait_connections();
		break;
	}

	return 0;
}

void wait_connections()
{
	struct sockaddr_in saddr;
	int ret = 0;

	memset(&saddr, 0, sizeof(saddr));
	saddr.sin_family = AF_INET;
	saddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	saddr.sin_port = htons(g_local_port);     
	sock = socket(AF_INET, SOCK_STREAM, 0);

	int temp = 1;
	if(setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&temp, sizeof(temp))) {
		print_error();
		printf("setsockopt() failed - ignorable\n");
	}

	ret = bind(sock, (struct sockaddr*)&saddr, sizeof(struct sockaddr));

	if ( ret == SOCKET_ERROR ) {
		fflush(stdout);
		printf("bind error %i !\n", ret);
		fflush(stdout);
		exit(EXIT_BIND_ERROR);
	}

	listen(sock, 0);
			
	int lpThreadId;
	pthread_t socket_thread;
	lpThreadId = pthread_create(&socket_thread, NULL, wait_for_device, NULL);
	pthread_detach(socket_thread);

	printf("Waiting for device...\n");
	fflush(stdout);

	while (1) {
		am_device_callbacks_t callbacks; 
		ret = AMDeviceNotificationSubscribe(notification, 0, 0, 0, &callbacks);
		if (ret != ERR_SUCCESS) {
				printf("AMDeviceNotificationSubscribe = %i\n", ret);
				exit(EXIT_GENERAL_ERROR);
		}
		
#if WIN32
		Sleep(-1);
#else
		CFRunLoopRun();
#endif
		printf("RUN LOOP EXIT\n");
		Sleep(1000);
	}
}

/****************************************************************************/

void notification(struct am_device_notification_callback_info* info)
{
	char deviceName[BUFFER_SIZE];
	CFStringRef devId = AMDeviceCopyDeviceIdentifier(info->dev);
	CFStringGetCString(devId, deviceName, sizeof(deviceName), kCFStringEncodingASCII);
	
	if (target_device_id != nil) {
		if (0 != strcasecmp(deviceName, target_device_id)) {
			printf("Ignoring device %s (need %s)\n", deviceName, target_device_id);
			return;
		}
	}
	
	if (info -> msg == ADNCI_MSG_CONNECTED) {
		target_device = info->dev;
		printf("Device connected: %s\n", deviceName);
	} else {
		printf("Device disconnected: %s\n", deviceName);
		target_device = NULL;
		muxConn = 0;
	}
}

void* THREADPROCATTR wait_for_device(void* arg)
{
	int ret;
	int handle = -1;
	restore_dev_t restore_dev;
			
	while (1) {
		
		if (target_device == NULL) {
			sleep(1);
			continue;
		}
		
		printf("Info: Waiting for new TCP connection on port %hu\n", g_local_port);
						
		struct sockaddr_in sockAddrin;
		socklen_t len = sizeof(sockAddrin);
		int new_sock = accept(sock, (struct sockaddr*) &sockAddrin , &len);
		
		
		if (new_sock == -1) {
			printf("accept error\n");
			continue;
		}
		
		printf("Info: New connection...\n");
		
		if (muxConn == 0)
		{
			ret = AMDeviceConnect(target_device);
			if (ret == ERR_SUCCESS) {
				muxConn = AMDeviceGetConnectionID(target_device);
			} else if (ret == -402653144) {
				restore_dev = AMRestoreModeDeviceCreate(0, AMDeviceGetConnectionID(target_device), 0);
				printf("restore_dev = %p\n", restore_dev);
				muxConn = AMRestoreModeDeviceGetDeviceID(restore_dev);
				printf("muxConn = %X\n", muxConn);
				AMRestoreModeDeviceReboot(restore_dev);
				sleep(10);
			} else {
				printf("AMDeviceConnect = %i\n", ret);
				goto error_connect;
			}
		}                               
		puts("Info: Device connected.");
						
		ret = USBMuxConnectByPort(muxConn, htons(g_iphone_port), &handle);
		if (ret != ERR_SUCCESS) {
			printf("USBMuxConnectByPort = %x, handle=%x\n", ret, handle);
			goto error_service;
		}
						
		puts("Info: Service started.");
		
		struct connection* connection1;
		struct connection* connection2;
		
		connection1 = new connection;
		if (!connection1) {
			exit(EXIT_GENERAL_ERROR);
		}
		connection2 = new connection;    
		if (!connection2) {
			exit(EXIT_GENERAL_ERROR);
		}
		
		connection1->from_handle = new_sock;
		connection1->to_handle = handle;
		connection2->from_handle = handle;
		connection2->to_handle = new_sock;
		
		printf("sock handle newsock:%d iphone:%d\n", new_sock, handle);
		fflush(stdout);
		
		int lpThreadId;
		int lpThreadId2;
		pthread_t thread1;
		pthread_t thread2;
		
		lpThreadId = pthread_create(&thread1, NULL, conn_forwarding_thread, (void*)connection1);
		lpThreadId2 = pthread_create(&thread2, NULL, conn_forwarding_thread, (void*)connection2);
		
		pthread_detach(thread2);
		pthread_detach(thread1);

		Sleep(100);
		
		continue;

error_connect:
		printf("Error: Device Connect\n");
		AMDeviceStopSession(target_device);
		AMDeviceDisconnect(target_device);
		sleep(1);
		
		continue;
		
error_service:
		printf("Error: Device Service\n");
		AMDeviceStopSession(target_device);
		AMDeviceDisconnect(target_device);
		sleep(1);
		continue;
		
	}
}


/****************************************************************************/

void* THREADPROCATTR conn_forwarding_thread(void* arg)
{
	connection* con = (connection*)arg;
	uint8_t buffer[BUFFER_SIZE];
	int bytes_recv, bytes_send;
	
	threadCount++;
	fflush(stdout);
	printf("threadcount=%d\n",threadCount);
	fflush(stdout);
	
	while (1) {
		bytes_recv = recv(con->from_handle, (char*)buffer, BUFFER_SIZE, 0);
		
		bytes_send = send(con->to_handle, (char*)buffer, bytes_recv, 0);
		
		if (bytes_recv == 0 || bytes_recv == SOCKET_ERROR || bytes_send == 0 || bytes_send == SOCKET_ERROR) {
			threadCount--;
			fflush(stdout);
			printf("threadcount=%d\n", threadCount);
			fflush(stdout);
			
			close(con->from_handle);
			close(con->to_handle);
									
			delete con;
			
			break;
		}
	}
	return nil;
}
