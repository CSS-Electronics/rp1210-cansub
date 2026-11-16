#ifndef CANSUB_RP1210_H
#define CANSUB_RP1210_H

/* The TMC RP1210E API, as a diagnostic application sees it.
 *
 * Written from the RP1210E specification (TMC/ATA, 2026) with signatures cross-checked against two
 * MIT-licensed reference implementations. No proprietary vendor material is used; see NOTICE.md.
 *
 * Every entry point is __stdcall and exported undecorated, which the .def file arranges: a host
 * application resolves them by plain name via GetProcAddress. */

#include <windows.h>

#include "rp1210_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Protocol identifiers. Only CAN is implemented; see README.md. */
#define RP1210_CAN_PRO_ID      10
#define RP1210_J1939_PRO_ID    20
#define RP1210_ISO15765_PRO_ID 30

/* Blocking mode for RP1210_ReadMessage. RP1210E ignores the flag on send. */
#define RP1210_BLOCKING_IO     1
#define RP1210_NON_BLOCKING_IO 0

#define RP1210_ECHO_OFF 0
#define RP1210_ECHO_ON  1

/* Filter mode for RPCMD_SET_CAN_FILTER_TYPE. */
#define RP1210_FILTER_INCLUSIVE 0x00
#define RP1210_FILTER_EXCLUSIVE 0x01

/* Message Type byte, first byte of a CAN message body (RP1210E sections 16.8 and 17.7). */
#define RP1210_STANDARD_CAN 0x00
#define RP1210_EXTENDED_CAN 0x01

/* Baud identifiers for RPCMD_SET_CAN_BAUD. */
#define RP1210_BAUD_125k  4
#define RP1210_BAUD_250k  5
#define RP1210_BAUD_500k  6
#define RP1210_BAUD_1000k 7

/* Longest CAN message body: 4 timestamp + 1 echo + 1 type + 4 identifier + 8 data. */
#define RP1210_CAN_MESSAGE_MAX 18

#define RP1210_MAX_ERR_MSG_LENGTH 80

short WINAPI RP1210_ClientConnect(HWND hwndClient, short nDeviceID, const char *fpchProtocol,
                                  long lTxBufferSize, long lRcvBufferSize,
                                  short nIsAppPacketizingIncomingMsgs);

short WINAPI RP1210_ClientDisconnect(short nClientID);

/* nNotifyStatusOnTx and nBlockOnSend are ignored in RP1210E; sends are always non-blocking. */
short WINAPI RP1210_SendMessage(short nClientID, unsigned char *fpchClientMessage,
                                short nMessageSize, short nNotifyStatusOnTx, short nBlockOnSend);

/* Returns the byte count including the four timestamp bytes, 0 when no message is waiting, or the
 * additive inverse of an error code. */
short WINAPI RP1210_ReadMessage(short nClientID, unsigned char *fpchAPIMessage, short nBufferSize,
                                short nBlockOnRead);

short WINAPI RP1210_SendCommand(short nCommandNumber, short nClientID,
                                const char *fpchClientCommand, short nMessageSize);

void WINAPI RP1210_ReadVersion(char *fpchDLLMajorVersion, char *fpchDLLMinorVersion,
                               char *fpchAPIMajorVersion, char *fpchAPIMinorVersion);

short WINAPI RP1210_ReadDetailedVersion(short nClientID, char *fpchAPIVersionInfo,
                                        char *fpchDLLVersionInfo, char *fpchFWVersionInfo);

short WINAPI RP1210_GetErrorMsg(short err_code, char *fpchDescription);

short WINAPI RP1210_GetLastErrorMsg(short err_code, int *SubErrorCode, char *fpchDescription,
                                    short nClientID);

short WINAPI RP1210_GetHardwareStatus(short nClientID, char *fpchClientInfo, short nInfoSize,
                                      short nBlockOnRequest);

short WINAPI RP1210_Ioctl(short nClientID, long nIoctlID, void *pInput, void *pOutput);

/* CSS Electronics extension, not part of RP1210. RP1210 host applications never look it up, so it
 * changes nothing for them.
 *
 * For LabVIEW with NI-XNET: drains every CAN frame already received on a generic CAN client into
 * pBuffer in NI-XNET's raw frame format, which "XNET Convert (Frame Raw to Signal)" decodes with a
 * DBC directly. That keeps byte unpacking out of the LabVIEW diagram. Never blocks. Delivers CAN FD
 * frames too (types CAN FD and CAN FD+BRS), which RP1210_ReadMessage skips.
 *
 * Returns the number of bytes written (0 when nothing is queued) and the frame count in
 * *pFrameCount (may be NULL), or a negative RP1210 error code. nBufferSize must be at least
 * CANSUB_XNET_FRAME_MAX, one CAN FD frame at its largest. */
#define CANSUB_XNET_FRAME_MAX 80

long WINAPI CANSUB_ReadXnetFrames(short nClientID, unsigned char *pBuffer, long nBufferSize,
                                  long *pFrameCount);

/* CSS Electronics extension, the mirror of CANSUB_ReadXnetFrames: sends the frames in pBuffer, given
 * in NI-XNET's raw frame format as "XNET Convert (Signal to Frame Raw)" produces them, so LabVIEW
 * can send by DBC signal name. Generic CAN clients only. Classic CAN data and remote frames, and
 * CAN FD frames (with or without bit rate switch) on a client connected with DataBaud=. Timestamps
 * and flags in the buffer are ignored. A listen-only client (ListenOnly=1) sends nothing.
 *
 * Returns the number of frames sent (also in *pFrameCount, which may be NULL), or a negative RP1210
 * error code. A malformed buffer sends nothing. If a send fails part-way, *pFrameCount holds the
 * frames sent before it. */
long WINAPI CANSUB_WriteXnetFrames(short nClientID, const unsigned char *pBuffer, long nBufferSize,
                                   long *pFrameCount);

#ifdef __cplusplus
}
#endif

#endif
