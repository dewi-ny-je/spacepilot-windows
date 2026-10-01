#pragma once
/* Binary interface of the legacy 3DxWare "SpaceWare input" library
 * (siappdll.dll), independently transcribed from public interface facts.
 * Only types that cross the DLL boundary are declared. Applications allocate
 * these structures from their own headers, so Axial writes only the fields
 * listed here and never more than the event payload that it reports. */
#include <windows.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define SPWAPI __stdcall
typedef int32_t SPWint32;
typedef uint32_t SPWuint32;
typedef long SPWbool;
typedef float SPWfloat32;
typedef int SiDevID;
typedef struct SiHandle* SiHdl;

#define SI_STRSIZE 128
#define SI_MAXBUF 128
#define SI_MAXPATH 512
#define SI_MAXPORTNAME 260
#define SI_ANY_DEVICE -1
#define SI_NO_DEVICE -1
#define SI_NO_BUTTON -1
#define SI_EVENT 0x0001
#define SI_POLL 0x0002
#define SI_AVERAGE_EVENTS 0x0001
#define SI_UI_ALL_CONTROLS 0xffffffffL
#define SI_UI_NO_CONTROLS 0x00000000L

typedef enum SpwRetVal {
    SPW_NO_ERROR=0,SPW_ERROR,SI_BAD_HANDLE,SI_BAD_ID,SI_BAD_VALUE,SI_IS_EVENT,SI_SKIP_EVENT,
    SI_NOT_EVENT,SI_NO_DRIVER,SI_NO_RESPONSE,SI_UNSUPPORTED,SI_UNINITIALIZED,SI_WRONG_DRIVER,
    SI_INTERNAL_ERROR,SI_BAD_PROTOCOL,SI_OUT_OF_MEMORY,SPW_DLL_LOAD_ERROR,SI_NOT_OPEN,
    SI_ITEM_NOT_FOUND,SI_UNSUPPORTED_DEVICE
} SpwRetVal;

typedef enum SiEventType {
    SI_BUTTON_EVENT=1,SI_MOTION_EVENT,SI_COMBO_EVENT,SI_ZERO_EVENT,SI_EXCEPTION_EVENT,
    SI_OUT_OF_BAND,SI_ORIENTATION_EVENT,SI_KEYBOARD_EVENT,SI_LPFK_EVENT,SI_APP_EVENT,
    SI_SYNC_EVENT,SI_BUTTON_PRESS_EVENT,SI_BUTTON_RELEASE_EVENT,SI_DEVICE_CHANGE_EVENT,
    SI_MOUSE_EVENT,SI_JOYSTICK_EVENT
} SiEventType;

/* Legacy device type codes; newer devices report their USB product ID. */
enum {
    SI_UNKNOWN_DEVICE=0,SI_SPACEEXPLORER=4,SI_SPACENAVIGATOR_FOR_NOTEBOOKS=5,
    SI_SPACENAVIGATOR=6,SI_SPACEBALL_5000=21,SI_TRAVELER=25,SI_SPACEPILOT=29
};
enum {SI_DEVICE_CHANGE_CONNECT=0,SI_DEVICE_CHANGE_DISCONNECT=1};
enum {SI_TX=0,SI_TY,SI_TZ,SI_RX,SI_RY,SI_RZ};

/* Button N is reported as bit N; bit 0 is unused by numbered buttons. */
typedef struct SiButtonData {SPWuint32 last,current,pressed,released;} SiButtonData;
typedef struct SiSpwData {
    SiButtonData bData;
    SPWint32 mData[6];
    SPWint32 period;   /* milliseconds since the previous motion event */
} SiSpwData;
typedef struct SiHWButtonData {SPWuint32 buttonNumber;} SiHWButtonData;
typedef struct SiDeviceChangeHeader {SPWint32 type;SiDevID devID;} SiDeviceChangeHeader;
typedef struct SiSpwEvent {
    int type;
    union {
        SiSpwData spwData;
        SiHWButtonData hwButtonEvent;
        SiDeviceChangeHeader deviceChangeEventData;
        char exData[SI_MAXBUF];
    } u;
} SiSpwEvent;

/* Filled by SiOpenWinInit; Axial reads and writes only the window handle. */
typedef struct SiOpenData {HWND hWnd;} SiOpenData;
typedef struct SiGetEventData {UINT msg;WPARAM wParam;LPARAM lParam;} SiGetEventData;
typedef struct SiTypeMask {unsigned char mask[8];} SiTypeMask;
typedef struct SiDevInfo {
    int devType;int numButtons;int numDegrees;SPWbool canBeep;char firmware[SI_STRSIZE];
} SiDevInfo;
typedef struct SiVerInfo {int major,minor,build;char version[SI_STRSIZE];char date[SI_STRSIZE];} SiVerInfo;
typedef struct SiDevPort {SiDevID devID;int devType;int devClass;char devName[SI_STRSIZE];char portName[SI_MAXPORTNAME];} SiDevPort;
typedef struct SiDeviceName {char name[SI_STRSIZE];} SiDeviceName;
typedef struct SiButtonName {char name[SI_STRSIZE];} SiButtonName;
typedef struct SiEventHandler {int (*func)(SiOpenData*,SiGetEventData*,SiSpwEvent*,void*);void* data;} SiEventHandler;
typedef struct SiSpwHandlers {SiEventHandler button,motion,combo,zero,exception;} SiSpwHandlers;

SpwRetVal SPWAPI SiInitialize(void);
void SPWAPI SiTerminate(void);
SPWbool SPWAPI SiIsInitialized(void);
int SPWAPI SiGetNumDevices(void);
SiDevID SPWAPI SiDeviceIndex(int index);
void SPWAPI SiOpenWinInit(SiOpenData* data,HWND window);
SiHdl SPWAPI SiOpen(const char* application,SiDevID device,const SiTypeMask* mask,int mode,const SiOpenData* data);
SiHdl SPWAPI SiOpenPort(const char* application,const SiDevPort* port,int mode,const SiOpenData* data);
SpwRetVal SPWAPI SiClose(SiHdl handle);
void SPWAPI SiGetEventWinInit(SiGetEventData* data,UINT message,WPARAM wParam,LPARAM lParam);
SpwRetVal SPWAPI SiGetEvent(SiHdl handle,int flags,const SiGetEventData* data,SiSpwEvent* event);
SpwRetVal SPWAPI SiPeekEvent(SiHdl handle,int flags,const SiGetEventData* data,SiSpwEvent* event);
SPWbool SPWAPI SiIsSpaceWareEvent(const SiGetEventData* data,SiHdl handle);
int SPWAPI SiDispatch(SiHdl handle,SiGetEventData* data,SiSpwEvent* event,SiSpwHandlers* handlers);
int SPWAPI SiButtonPressed(SiSpwEvent* event);
int SPWAPI SiButtonReleased(SiSpwEvent* event);
SpwRetVal SPWAPI SiGetButtonName(SiHdl handle,SPWuint32 button,SiButtonName* name);
SpwRetVal SPWAPI SiGetDeviceName(SiHdl handle,SiDeviceName* name);
SpwRetVal SPWAPI SiGetDeviceInfo(SiHdl handle,SiDevInfo* info);
SiDevID SPWAPI SiGetDeviceID(SiHdl handle);
SpwRetVal SPWAPI SiGetDevicePort(SiDevID device,SiDevPort* port);
void SPWAPI SiGetLibraryInfo(SiVerInfo* info);
SpwRetVal SPWAPI SiGetDriverInfo(SiVerInfo* info);
SpwRetVal SPWAPI SiBeep(SiHdl handle,char* text);
SpwRetVal SPWAPI SiRezero(SiHdl handle);
SpwRetVal SPWAPI SiGrabDevice(SiHdl handle,SPWbool exclusive);
SpwRetVal SPWAPI SiReleaseDevice(SiHdl handle);
SpwRetVal SPWAPI SiSetUiMode(SiHdl handle,SPWuint32 mode);
SpwRetVal SPWAPI SiSetLEDs(SiHdl handle,SPWuint32 mask);
SpwRetVal SPWAPI SiGetDeviceImageFileName(SiHdl handle,char* name,SPWuint32* length);
const char* SPWAPI SpwErrorString(SpwRetVal error);
#ifdef __cplusplus
}
#endif
