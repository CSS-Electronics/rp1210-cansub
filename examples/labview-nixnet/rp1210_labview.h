/* The functions of CANSUB32.dll, in plain C types so LabVIEW's
 * Tools > Import > Shared Library (.dll) wizard can parse them without windows.h.
 * Matches CANSUB32.dll (RP1210E plus two CSS Electronics extensions). RP1210_Ioctl is left out:
 * the driver supports no Ioctl. Byte buffers are declared as arrays so the wizard makes them U8 arrays. */

short __stdcall RP1210_ClientConnect(void *hwndClient, short nDeviceID, char *fpchProtocol,
                                     long lTxBufferSize, long lRcvBufferSize, short nIsAppPacketizingIncomingMsgs);
short __stdcall RP1210_ClientDisconnect(short nClientID);
short __stdcall RP1210_ReadMessage(short nClientID, unsigned char fpchAPIMessage[], short nBufferSize,
                                   short nBlockOnRead);
short __stdcall RP1210_SendMessage(short nClientID, unsigned char fpchClientMessage[], short nMessageSize,
                                   short nNotifyStatusOnTx, short nBlockOnSend);
short __stdcall RP1210_SendCommand(short nCommandNumber, short nClientID, unsigned char fpchClientCommand[],
                                   short nMessageSize);
void __stdcall RP1210_ReadVersion(char *fpchDLLMajorVersion, char *fpchDLLMinorVersion,
                                  char *fpchAPIMajorVersion, char *fpchAPIMinorVersion);
short __stdcall RP1210_ReadDetailedVersion(short nClientID, char *fpchAPIVersionInfo,
                                           char *fpchDLLVersionInfo, char *fpchFWVersionInfo);
short __stdcall RP1210_GetErrorMsg(short ErrorCode, char *fpchDescription);
short __stdcall RP1210_GetLastErrorMsg(short ErrorCode, int *SubErrorCode, char *fpchDescription,
                                       short nClientID);
short __stdcall RP1210_GetHardwareStatus(short nClientID, unsigned char fpchClientInfo[], short nInfoSize,
                                         short nBlockOnRequest);

/* CSS Electronics extension: frames in NI-XNET raw format, for XNET Convert (Frame Raw to Signal).
 * Includes CAN FD frames. */
long __stdcall CANSUB_ReadXnetFrames(short nClientID, unsigned char pBuffer[], long nBufferSize, long *pFrameCount);

/* CSS Electronics extension: send frames given in NI-XNET raw format, from XNET Convert (Signal to Frame Raw).
 * CAN FD frames need a client connected with DataBaud=. Returns the number of frames sent, or a negative
 * RP1210 error code. */
long __stdcall CANSUB_WriteXnetFrames(short nClientID, const unsigned char pBuffer[], long nBufferSize, long *pFrameCount);
