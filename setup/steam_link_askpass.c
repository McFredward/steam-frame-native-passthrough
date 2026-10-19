/* Own OpenSSH askpass child: one authenticated loopback request, no console UI.
 * The broker owns all interaction. No credentials enter argv, environment,
 * files or diagnostic output; stdout is exclusively OpenSSH's response pipe.
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

#define MAX_PROMPT_BYTES 4096
#define MAX_ANSWER_BYTES 1024
#define REQUEST_TIMEOUT_MS 300000UL

static DWORD remaining(ULONGLONG deadline) {
    const ULONGLONG now = GetTickCount64();
    return now < deadline ? (DWORD)(deadline - now) : 0;
}

static int exact_io(SOCKET connection, char *bytes, int count, int sending,
                    ULONGLONG deadline) {
    int offset = 0;
    while (offset < count) {
        const DWORD timeout = remaining(deadline);
        int done;
        if (!timeout || setsockopt(connection, SOL_SOCKET,
                sending ? SO_SNDTIMEO : SO_RCVTIMEO,
                (const char *)&timeout, (int)sizeof(timeout)) != 0) return 0;
        done = sending ? send(connection, bytes + offset, count - offset, 0)
                       : recv(connection, bytes + offset, count - offset, 0);
        if (done <= 0) return 0;
        offset += done;
    }
    return 1;
}

static int connect_local(SOCKET connection, unsigned short port, ULONGLONG deadline) {
    struct sockaddr_in address;
    u_long nonblocking = 1;
    int status, error = 0, error_bytes = (int)sizeof(error);
    fd_set writable, failed;
    struct timeval wait;
    DWORD timeout;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (ioctlsocket(connection, FIONBIO, &nonblocking) != 0) return 0;
    status = connect(connection, (const struct sockaddr *)&address, (int)sizeof(address));
    if (status == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) return 0;
    if (status != 0) {
        timeout = remaining(deadline);
        if (!timeout) return 0;
        FD_ZERO(&writable); FD_ZERO(&failed);
        FD_SET(connection, &writable); FD_SET(connection, &failed);
        wait.tv_sec = (long)(timeout / 1000);
        wait.tv_usec = (long)((timeout % 1000) * 1000);
        if (select(0, NULL, &writable, &failed, &wait) <= 0 ||
                !FD_ISSET(connection, &writable) || FD_ISSET(connection, &failed) ||
                getsockopt(connection, SOL_SOCKET, SO_ERROR, (char *)&error, &error_bytes) != 0 || error) return 0;
    }
    nonblocking = 0;
    return ioctlsocket(connection, FIONBIO, &nonblocking) == 0;
}

static int environment(wchar_t *port_text, wchar_t *token) {
    DWORD port_count = GetEnvironmentVariableW(L"SFNP_ASKPASS_PORT", port_text, 7);
    DWORD token_count = GetEnvironmentVariableW(L"SFNP_ASKPASS_TOKEN", token, 66);
    unsigned i;
    if (!port_count || port_count > 5 || token_count != 64) return 0;
    for (i = 0; i < token_count; ++i)
        if (!((token[i] >= L'0' && token[i] <= L'9') ||
              (token[i] >= L'a' && token[i] <= L'f') ||
              (token[i] >= L'A' && token[i] <= L'F'))) return 0;
    return 1;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line, int show) {
    wchar_t port_text[7] = {0}, token[66] = {0};
    wchar_t validated[MAX_ANSWER_BYTES + 1];
    char packet[8 + 64 + 4 + MAX_PROMPT_BYTES];
    char answer[MAX_ANSWER_BYTES + 1] = {0};
    unsigned char response[4];
    LPWSTR *arguments = NULL;
    WSADATA winsock;
    SOCKET connection = INVALID_SOCKET;
    ULONGLONG deadline;
    uint32_t count;
    unsigned long port = 0;
    unsigned i;
    int argc = 0, prompt_bytes, started = 0, result = 1;
    HANDLE output;
    DWORD written = 0;
    (void)instance; (void)previous; (void)command_line; (void)show;
    if (!environment(port_text, token) || port_text[0] == L'0') goto done;
    for (i = 0; port_text[i]; ++i) {
        if (port_text[i] < L'0' || port_text[i] > L'9') goto done;
        port = port * 10 + (unsigned long)(port_text[i] - L'0');
    }
    if (!port || port > 65535) goto done;
    arguments = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!arguments || argc != 2) goto done;
    prompt_bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, arguments[1], -1, NULL, 0, NULL, NULL);
    if (prompt_bytes <= 0 || prompt_bytes - 1 > MAX_PROMPT_BYTES) goto done;
    --prompt_bytes;
    memcpy(packet, "SFNPASK1", 8);
    for (i = 0; i < 64; ++i) packet[8 + i] = (char)token[i];
    count = htonl((uint32_t)prompt_bytes);
    memcpy(packet + 72, &count, 4);
    if (prompt_bytes && WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, arguments[1],
            (int)wcslen(arguments[1]), packet + 76, prompt_bytes, NULL, NULL) != prompt_bytes) goto done;
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) goto done;
    started = 1;
    deadline = GetTickCount64() + REQUEST_TIMEOUT_MS;
    connection = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (connection == INVALID_SOCKET || !connect_local(connection, (unsigned short)port, deadline) ||
            !exact_io(connection, packet, 76 + prompt_bytes, 1, deadline) ||
            !exact_io(connection, (char *)response, 4, 0, deadline)) goto done;
    count = ((uint32_t)response[0] << 24) | ((uint32_t)response[1] << 16) |
            ((uint32_t)response[2] << 8) | (uint32_t)response[3];
    if (count == UINT32_MAX || count > MAX_ANSWER_BYTES ||
            !exact_io(connection, answer, (int)count, 0, deadline)) goto done;
    for (i = 0; i < count; ++i) if (!answer[i] || answer[i] == '\n' || answer[i] == '\r') goto done;
    if (count && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, answer, (int)count,
            validated, MAX_ANSWER_BYTES + 1) <= 0) goto done;
    output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!output || output == INVALID_HANDLE_VALUE) goto done;
    answer[count] = '\n';
    if (!WriteFile(output, answer, count + 1, &written, NULL) || written != count + 1) goto done;
    result = 0;
done:
    SecureZeroMemory(answer, sizeof(answer));
    SecureZeroMemory(validated, sizeof(validated));
    SecureZeroMemory(packet, sizeof(packet));
    SecureZeroMemory(token, sizeof(token));
    if (connection != INVALID_SOCKET) closesocket(connection);
    if (started) WSACleanup();
    if (arguments) LocalFree(arguments);
    return result;
}
