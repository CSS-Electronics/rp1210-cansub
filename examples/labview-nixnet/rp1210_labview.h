/* RP1210 functions used by the LabVIEW example, in plain C types so LabVIEW's
 * Tools > Import > Shared Library (.dll) wizard can parse them without windows.h.
 * Matches CANSUB32.dll (RP1210E). On 64-bit LabVIEW, use the x64 build of the DLL. */

short __stdcall RP1210_ClientConnect(void *hwndClient, short nDeviceID, char *fpchProtocol,
                                     long lTxBufferSize, long lRcvBufferSize, short nIsAppPacketizingIncomingMsgs);
short __stdcall RP1210_ClientDisconnect(short nClientID);
short __stdcall RP1210_ReadMessage(short nClientID, unsigned char *fpchAPIMessage, short nBufferSize,
                                   short nBlockOnRead);
short __stdcall RP1210_SendMessage(short nClientID, unsigned char *fpchClientMessage, short nMessageSize,
                                   short nNotifyStatusOnTx, short nBlockOnSend);
short __stdcall RP1210_SendCommand(short nCommandNumber, short nClientID, char *fpchClientCommand,
                                   short nMessageSize);
short __stdcall RP1210_GetErrorMsg(short ErrorCode, char *fpchDescription);

/* CSS Electronics extension: frames in NI-XNET raw format, for XNET Convert (Frame Raw to Signal). */
long __stdcall CANSUB_ReadXnetFrames(short nClientID, unsigned char *pBuffer, long nBufferSize, long *pFrameCount);

/* CSS Electronics extension: send frames given in NI-XNET raw format, from XNET Convert (Signal to Frame Raw).
 * Returns the number of frames sent, or a negative RP1210 error code. */
long __stdcall CANSUB_WriteXnetFrames(short nClientID, const unsigned char *pBuffer, long nBufferSize, long *pFrameCount);
