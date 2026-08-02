#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <wincrypt.h>
#include <unknwn.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Storage.Streams.h>
#include <sstream>
#include <iomanip>
#include <thread>
#include <vector>
#include <string>
#include <mutex>
#include <cmath>
#include <atomic>
#include <algorithm>
#include <fstream>
#include <cctype>
#include <cwctype>
#include <string_view>
#include "XiaomiIoTPlugin.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "onecore.lib")
#pragma comment(lib, "comdlg32.lib")

using namespace winrt;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace Windows::Storage::Streams;

// 全局单例定义
CXiaomiIoTPlugin CXiaomiIoTPlugin::s_instance;

static float GetDPIScale(HWND hWnd)
{
    typedef UINT(WINAPI* GetDpiForWindowFunc)(HWND);
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32)
    {
        GetDpiForWindowFunc pGetDpi = (GetDpiForWindowFunc)GetProcAddress(hUser32, "GetDpiForWindow");
        if (pGetDpi && hWnd)
        {
            UINT dpi = pGetDpi(hWnd);
            if (dpi > 0) return (float)dpi / 96.0f;
        }
    }
    HDC hdc = GetDC(NULL);
    int dpiX = GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(NULL, hdc);
    return (float)dpiX / 96.0f;
}

static std::string WStringToString(const std::wstring& wstr)
{
    if (wstr.empty()) return "";
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

static std::wstring StringToWString(const std::string& value)
{
    if (value.empty()) return L"";
    int needed = MultiByteToWideChar(CP_UTF8, 0, value.data(), (int)value.size(), nullptr, 0);
    std::wstring result(needed, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), (int)value.size(), result.data(), needed);
    return result;
}

static std::string JsonEscape(const std::wstring& value)
{
    std::string in = WStringToString(value), out;
    out.reserve(in.size() + 8);
    for (char ch : in) {
        if (ch == '\\' || ch == '"') { out.push_back('\\'); out.push_back(ch); }
        else if (ch == '\n') out += "\\n";
        else if (ch == '\r') out += "\\r";
        else if (ch == '\t') out += "\\t";
        else out.push_back(ch);
    }
    return out;
}

static bool JsonReadString(const std::string& text, const char* key, std::wstring& out)
{
    const std::string marker = std::string("\"") + key + "\":\"";
    size_t pos = text.find(marker);
    if (pos == std::string::npos) return false;
    pos += marker.size(); std::string raw;
    bool escaped = false;
    for (; pos < text.size(); ++pos) {
        char ch = text[pos];
        if (escaped) { raw.push_back(ch == 'n' ? '\n' : ch == 'r' ? '\r' : ch == 't' ? '\t' : ch); escaped = false; }
        else if (ch == '\\') escaped = true;
        else if (ch == '"') { out = StringToWString(raw); return true; }
        else raw.push_back(ch);
    }
    return false;
}

static double JsonReadNumber(const std::string& text, const char* key, double fallback)
{
    const std::string marker = std::string("\"") + key + "\":";
    size_t pos = text.find(marker);
    return pos == std::string::npos ? fallback : std::atof(text.c_str() + pos + marker.size());
}

static bool JsonReadBool(const std::string& text, const char* key, bool fallback)
{
    const std::string marker = std::string("\"") + key + "\":";
    size_t pos = text.find(marker);
    if (pos == std::string::npos) return fallback;
    return text.compare(pos + marker.size(), 4, "true") == 0;
}

static std::vector<std::string> JsonReadObjectArray(const std::string& text, const char* key)
{
    std::vector<std::string> objects;
    const std::string marker = std::string("\"") + key + "\":[";
    size_t pos = text.find(marker);
    if (pos == std::string::npos) return objects;
    pos += marker.size();
    int depth = 0; size_t start = std::string::npos; bool inString = false, escaped = false;
    for (; pos < text.size(); ++pos) {
        char ch = text[pos];
        if (inString) { if (escaped) escaped = false; else if (ch == '\\') escaped = true; else if (ch == '"') inString = false; continue; }
        if (ch == '"') { inString = true; continue; }
        if (ch == '{') { if (depth++ == 0) start = pos; }
        else if (ch == '}' && depth > 0 && --depth == 0 && start != std::string::npos) { objects.push_back(text.substr(start, pos - start + 1)); start = std::string::npos; }
        else if (ch == ']' && depth == 0) break;
    }
    return objects;
}

static std::vector<uint8_t> HexStringToBytes(const std::wstring& hex);

static std::wstring MakeStableId(const std::wstring& seed, const wchar_t* prefix)
{
    // FNV-1a is deterministic across process restarts, unlike std::hash.
    unsigned long long hash = 1469598103934665603ULL;
    for (wchar_t ch : seed) {
        hash ^= (unsigned long long)(unsigned short)ch;
        hash *= 1099511628211ULL;
    }
    wchar_t value[48]{};
    swprintf_s(value, L"%s_%016llx", prefix, hash);
    return value;
}

static bool IsPlaceholderToken(const std::wstring& token)
{
    std::vector<uint8_t> value = HexStringToBytes(token);
    return value.size() == 16 && std::all_of(value.begin(), value.end(), [](uint8_t b) { return b == 0xFF; });
}

static std::wstring TrimWhitespace(std::wstring value)
{
    const size_t begin = value.find_first_not_of(L" \t\r\n");
    if (begin == std::wstring::npos) return L"";
    const size_t end = value.find_last_not_of(L" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

static std::wstring GetEnvFilePath(const std::wstring& config_path)
{
    const size_t separator = config_path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? L".env" : config_path.substr(0, separator + 1) + L".env";
}

static std::wstring ReadDotEnvValue(const std::wstring& env_path, const std::wstring& key)
{
    std::wifstream input(env_path);
    if (!input.is_open()) return L"";

    std::wstring line;
    while (std::getline(input, line))
    {
        line = TrimWhitespace(line);
        if (line.empty() || line[0] == L'#') continue;
        if (line.rfind(L"export ", 0) == 0) line = TrimWhitespace(line.substr(7));

        const size_t equals = line.find(L'=');
        if (equals == std::wstring::npos || TrimWhitespace(line.substr(0, equals)) != key) continue;

        std::wstring value = TrimWhitespace(line.substr(equals + 1));
        if (value.size() >= 2 && ((value.front() == L'\"' && value.back() == L'\"') || (value.front() == L'\'' && value.back() == L'\'')))
            value = value.substr(1, value.size() - 2);
        return value;
    }
    return L"";
}

static std::wstring FormatLocalTime(std::time_t timestamp)
{
    if (timestamp == 0) return L"--";
    tm local{};
    localtime_s(&local, &timestamp);
    wchar_t value[32]{};
    wcsftime(value, 32, L"%H:%M:%S", &local);
    return value;
}

// 纯 C++ 原生 CryptoAPI + Winsock2 加密通信函数
static std::vector<uint8_t> NativeMD5(const std::vector<uint8_t>& data)
{
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    std::vector<uint8_t> result(16, 0);

    if (CryptAcquireContextA(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
    {
        if (CryptCreateHash(hProv, CALG_MD5, 0, 0, &hHash))
        {
            CryptHashData(hHash, data.data(), (DWORD)data.size(), 0);
            DWORD len = 16;
            CryptGetHashParam(hHash, HP_HASHVAL, result.data(), &len, 0);
            CryptDestroyHash(hHash);
        }
        CryptReleaseContext(hProv, 0);
    }
    return result;
}

static std::vector<uint8_t> NativeAES_CBC(const std::vector<uint8_t>& data, const std::vector<uint8_t>& key, const std::vector<uint8_t>& iv, bool encrypt)
{
    HCRYPTPROV hProv = 0;
    HCRYPTKEY hKey = 0;
    std::vector<uint8_t> buf = data;

    if (CryptAcquireContextA(&hProv, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
    {
        struct KeyBlob {
            BLOBHEADER header;
            DWORD keySize;
            BYTE keyData[16];
        } blob;
        blob.header.bType = PLAINTEXTKEYBLOB;
        blob.header.bVersion = CUR_BLOB_VERSION;
        blob.header.reserved = 0;
        blob.header.aiKeyAlg = CALG_AES_128;
        blob.keySize = 16;
        memcpy(blob.keyData, key.data(), 16);

        if (CryptImportKey(hProv, (BYTE*)&blob, sizeof(blob), 0, 0, &hKey))
        {
            DWORD mode = CRYPT_MODE_CBC;
            CryptSetKeyParam(hKey, KP_MODE, (BYTE*)&mode, 0);
            CryptSetKeyParam(hKey, KP_IV, iv.data(), 0);

            DWORD dataLen = (DWORD)buf.size();
            if (encrypt)
            {
                uint8_t pad = 16 - (dataLen % 16);
                for (int i = 0; i < pad; ++i) buf.push_back(pad);
                dataLen = (DWORD)buf.size();
                CryptEncrypt(hKey, 0, FALSE, 0, buf.data(), &dataLen, dataLen);
            }
            else
            {
                CryptDecrypt(hKey, 0, FALSE, 0, buf.data(), &dataLen);
                buf.resize(dataLen);
            }
            CryptDestroyKey(hKey);
        }
        CryptReleaseContext(hProv, 0);
    }
    return buf;
}

static std::vector<uint8_t> HexStringToBytes(const std::wstring& hex)
{
    std::vector<uint8_t> bytes;
    std::string ascii;
    for (wchar_t ch : hex) {
        if ((ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f') || (ch >= L'A' && ch <= L'F')) {
            ascii.push_back((char)ch);
        }
    }
    for (size_t i = 0; i + 1 < ascii.length(); i += 2) {
        std::string byteString = ascii.substr(i, 2);
        uint8_t byte = (uint8_t)strtol(byteString.c_str(), NULL, 16);
        bytes.push_back(byte);
    }
    return bytes;
}

// 动态通过 UDP 加密查询设备真实的 MiIO 型号 (miIO.info)
static bool QueryDeviceModelNativeUDP(const std::wstring& ipWStr, const std::wstring& tokenHexWStr, std::wstring& outModelName)
{
    std::string ipStr = WStringToString(ipWStr);
    std::vector<uint8_t> token = HexStringToBytes(tokenHexWStr);
    if (token.size() != 16) return false;

    std::vector<uint8_t> key = NativeMD5(token);
    std::vector<uint8_t> key_plus_token = key;
    key_plus_token.insert(key_plus_token.end(), token.begin(), token.end());
    std::vector<uint8_t> iv = NativeMD5(key_plus_token);

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return false;

    sockaddr_in localAddr{};
    localAddr.sin_family = AF_INET;
    localAddr.sin_addr.s_addr = INADDR_ANY;
    localAddr.sin_port = 0;
    bind(sock, (sockaddr*)&localAddr, sizeof(localAddr));

    int timeout = 800;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(54321);
    inet_pton(AF_INET, ipStr.c_str(), &serverAddr.sin_addr);

    std::vector<uint8_t> handshake(32, 0xFF);
    handshake[0] = 0x21; handshake[1] = 0x31;
    handshake[2] = 0x00; handshake[3] = 0x20;

    if (sendto(sock, (const char*)handshake.data(), (int)handshake.size(), 0, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR)
    {
        closesocket(sock);
        return false;
    }

    std::vector<uint8_t> resp(1024, 0);
    int respLen = recv(sock, (char*)resp.data(), (int)resp.size(), 0);
    if (respLen < 32 || resp[0] != 0x21 || resp[1] != 0x31)
    {
        closesocket(sock);
        return false;
    }

    uint32_t stamp = (resp[12] << 24) | (resp[13] << 16) | (resp[14] << 8) | resp[15];
    stamp += 1;

    std::string json_req = "{\"id\":100,\"method\":\"miIO.info\",\"params\":[]}";
    std::vector<uint8_t> plain_payload(json_req.begin(), json_req.end());
    plain_payload.push_back(0x00);

    std::vector<uint8_t> enc_payload = NativeAES_CBC(plain_payload, key, iv, true);
    uint16_t packet_len = (uint16_t)(32 + enc_payload.size());

    std::vector<uint8_t> header_16(16, 0);
    header_16[0] = 0x21; header_16[1] = 0x31;
    header_16[2] = (packet_len >> 8) & 0xFF; header_16[3] = packet_len & 0xFF;
    header_16[4] = 0; header_16[5] = 0; header_16[6] = 0; header_16[7] = 0;
    header_16[8] = resp[8]; header_16[9] = resp[9]; header_16[10] = resp[10]; header_16[11] = resp[11];
    header_16[12] = (stamp >> 24) & 0xFF; header_16[13] = (stamp >> 16) & 0xFF;
    header_16[14] = (stamp >> 8) & 0xFF; header_16[15] = stamp & 0xFF;

    std::vector<uint8_t> check_buf;
    check_buf.insert(check_buf.end(), header_16.begin(), header_16.end());
    check_buf.insert(check_buf.end(), token.begin(), token.end());
    check_buf.insert(check_buf.end(), enc_payload.begin(), enc_payload.end());

    std::vector<uint8_t> checksum = NativeMD5(check_buf);

    std::vector<uint8_t> packet;
    packet.insert(packet.end(), header_16.begin(), header_16.end());
    packet.insert(packet.end(), checksum.begin(), checksum.end());
    packet.insert(packet.end(), enc_payload.begin(), enc_payload.end());

    sendto(sock, (const char*)packet.data(), (int)packet.size(), 0, (sockaddr*)&serverAddr, sizeof(serverAddr));

    respLen = recv(sock, (char*)resp.data(), (int)resp.size(), 0);
    if (respLen > 32)
    {
        std::vector<uint8_t> resp_enc(resp.begin() + 32, resp.begin() + respLen);
        std::vector<uint8_t> resp_plain = NativeAES_CBC(resp_enc, key, iv, false);
        std::string resp_json(resp_plain.begin(), resp_plain.end());

        size_t model_pos = resp_json.find("\"model\":\"");
        if (model_pos != std::string::npos)
        {
            size_t end_pos = resp_json.find("\"", model_pos + 9);
            if (end_pos != std::string::npos)
            {
                std::string raw_model = resp_json.substr(model_pos + 9, end_pos - (model_pos + 9));
                wchar_t wModel[128] = { 0 };
                MultiByteToWideChar(CP_UTF8, 0, raw_model.c_str(), -1, wModel, 128);
                outModelName = wModel;
                closesocket(sock);
                return true;
            }
        }
    }

    closesocket(sock);
    return false;
}

static bool QueryPlugNativeUDP(const std::wstring& ipWStr, const std::wstring& tokenHexWStr, bool isChuangmi, double& outPower)
{
    std::string ipStr = WStringToString(ipWStr);
    std::vector<uint8_t> token = HexStringToBytes(tokenHexWStr);
    if (token.size() != 16) return false;

    std::vector<uint8_t> key = NativeMD5(token);
    std::vector<uint8_t> key_plus_token = key;
    key_plus_token.insert(key_plus_token.end(), token.begin(), token.end());
    std::vector<uint8_t> iv = NativeMD5(key_plus_token);

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return false;

    sockaddr_in localAddr{};
    localAddr.sin_family = AF_INET;
    localAddr.sin_addr.s_addr = INADDR_ANY;
    localAddr.sin_port = 0;
    bind(sock, (sockaddr*)&localAddr, sizeof(localAddr));

    int timeout = 500;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(54321);
    inet_pton(AF_INET, ipStr.c_str(), &serverAddr.sin_addr);

    std::vector<uint8_t> handshake(32, 0xFF);
    handshake[0] = 0x21; handshake[1] = 0x31;
    handshake[2] = 0x00; handshake[3] = 0x20;

    if (sendto(sock, (const char*)handshake.data(), (int)handshake.size(), 0, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR)
    {
        closesocket(sock);
        return false;
    }

    std::vector<uint8_t> resp(1024, 0);
    int respLen = recv(sock, (char*)resp.data(), (int)resp.size(), 0);
    if (respLen < 32 || resp[0] != 0x21 || resp[1] != 0x31)
    {
        closesocket(sock);
        return false;
    }

    uint32_t stamp = (resp[12] << 24) | (resp[13] << 16) | (resp[14] << 8) | resp[15];
    stamp += 1;

    std::string json_req = isChuangmi
        ? "{\"id\":1,\"method\":\"get_properties\",\"params\":[{\"did\":\"power\",\"siid\":5,\"piid\":6}]}"
        : "{\"id\":1,\"method\":\"get_properties\",\"params\":[{\"did\":\"power\",\"siid\":11,\"piid\":2}]}";

    std::vector<uint8_t> plain_payload(json_req.begin(), json_req.end());
    plain_payload.push_back(0x00);

    std::vector<uint8_t> enc_payload = NativeAES_CBC(plain_payload, key, iv, true);

    uint16_t packet_len = (uint16_t)(32 + enc_payload.size());

    std::vector<uint8_t> header_16(16, 0);
    header_16[0] = 0x21; header_16[1] = 0x31;
    header_16[2] = (packet_len >> 8) & 0xFF; header_16[3] = packet_len & 0xFF;
    header_16[4] = 0; header_16[5] = 0; header_16[6] = 0; header_16[7] = 0;
    header_16[8] = resp[8]; header_16[9] = resp[9]; header_16[10] = resp[10]; header_16[11] = resp[11];
    header_16[12] = (stamp >> 24) & 0xFF; header_16[13] = (stamp >> 16) & 0xFF;
    header_16[14] = (stamp >> 8) & 0xFF; header_16[15] = stamp & 0xFF;

    std::vector<uint8_t> check_buf;
    check_buf.insert(check_buf.end(), header_16.begin(), header_16.end());
    check_buf.insert(check_buf.end(), token.begin(), token.end());
    check_buf.insert(check_buf.end(), enc_payload.begin(), enc_payload.end());

    std::vector<uint8_t> checksum = NativeMD5(check_buf);

    std::vector<uint8_t> packet;
    packet.insert(packet.end(), header_16.begin(), header_16.end());
    packet.insert(packet.end(), checksum.begin(), checksum.end());
    packet.insert(packet.end(), enc_payload.begin(), enc_payload.end());

    sendto(sock, (const char*)packet.data(), (int)packet.size(), 0, (sockaddr*)&serverAddr, sizeof(serverAddr));

    respLen = recv(sock, (char*)resp.data(), (int)resp.size(), 0);
    if (respLen > 32)
    {
        std::vector<uint8_t> resp_enc(resp.begin() + 32, resp.begin() + respLen);
        std::vector<uint8_t> resp_plain = NativeAES_CBC(resp_enc, key, iv, false);
        std::string resp_json(resp_plain.begin(), resp_plain.end());

        size_t val_pos = resp_json.find("\"value\":");
        if (val_pos != std::string::npos)
        {
            int raw_val = std::atoi(resp_json.c_str() + val_pos + 8);
            outPower = isChuangmi ? (raw_val / 100.0) : (double)raw_val;
            closesocket(sock);
            return true;
        }
    }

    closesocket(sock);
    return false;
}

struct PlugQuerySnapshot
{
    double power = 0.0;
    PlugSwitchState state = PlugSwitchState::Unknown;
    bool state_supported = false;
};

// Sends one encrypted MiIO request after the protocol handshake.  Keeping this
// at the protocol boundary prevents the polling loop from inferring state from
// wattage or from a successful TCP/UDP send.
static bool SendMiioRequest(const std::wstring& ipWStr, const std::wstring& tokenHexWStr,
                            const std::string& jsonReq, std::string& responseJson)
{
    const std::string ip = WStringToString(ipWStr);
    const std::vector<uint8_t> token = HexStringToBytes(tokenHexWStr);
    if (token.size() != 16 || IsPlaceholderToken(tokenHexWStr)) return false;
    std::vector<uint8_t> key = NativeMD5(token);
    std::vector<uint8_t> keyToken = key;
    keyToken.insert(keyToken.end(), token.begin(), token.end());
    const std::vector<uint8_t> iv = NativeMD5(keyToken);

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return false;
    int timeout = 700;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons(54321);
    if (inet_pton(AF_INET, ip.c_str(), &server.sin_addr) != 1) { closesocket(sock); return false; }

    std::vector<uint8_t> hello(32, 0xFF);
    hello[0] = 0x21; hello[1] = 0x31; hello[2] = 0; hello[3] = 0x20;
    if (sendto(sock, reinterpret_cast<const char*>(hello.data()), (int)hello.size(), 0,
               reinterpret_cast<sockaddr*>(&server), sizeof(server)) == SOCKET_ERROR) { closesocket(sock); return false; }
    std::vector<uint8_t> response(2048, 0);
    int length = recv(sock, reinterpret_cast<char*>(response.data()), (int)response.size(), 0);
    if (length < 32 || response[0] != 0x21 || response[1] != 0x31) { closesocket(sock); return false; }

    uint32_t stamp = (uint32_t(response[12]) << 24) | (uint32_t(response[13]) << 16) |
                     (uint32_t(response[14]) << 8) | uint32_t(response[15]);
    ++stamp;
    std::vector<uint8_t> plain(jsonReq.begin(), jsonReq.end());
    plain.push_back(0);
    std::vector<uint8_t> encrypted = NativeAES_CBC(plain, key, iv, true);
    const uint16_t packetLength = (uint16_t)(32 + encrypted.size());
    std::vector<uint8_t> header(16, 0);
    header[0] = 0x21; header[1] = 0x31; header[2] = (packetLength >> 8) & 0xff; header[3] = packetLength & 0xff;
    header[8] = response[8]; header[9] = response[9]; header[10] = response[10]; header[11] = response[11];
    header[12] = (stamp >> 24) & 0xff; header[13] = (stamp >> 16) & 0xff; header[14] = (stamp >> 8) & 0xff; header[15] = stamp & 0xff;
    std::vector<uint8_t> check = header;
    check.insert(check.end(), token.begin(), token.end());
    check.insert(check.end(), encrypted.begin(), encrypted.end());
    std::vector<uint8_t> checksum = NativeMD5(check);
    std::vector<uint8_t> packet = header;
    packet.insert(packet.end(), checksum.begin(), checksum.end());
    packet.insert(packet.end(), encrypted.begin(), encrypted.end());
    sendto(sock, reinterpret_cast<const char*>(packet.data()), (int)packet.size(), 0,
           reinterpret_cast<sockaddr*>(&server), sizeof(server));
    length = recv(sock, reinterpret_cast<char*>(response.data()), (int)response.size(), 0);
    closesocket(sock);
    if (length <= 32) return false;
    std::vector<uint8_t> encryptedResponse(response.begin() + 32, response.begin() + length);
    std::vector<uint8_t> decoded = NativeAES_CBC(encryptedResponse, key, iv, false);
    responseJson.assign(decoded.begin(), decoded.end());
    return !responseJson.empty() && responseJson.find("\"error\"") == std::string::npos;
}

static bool QueryPlugSnapshotUDP(const PlugDeviceConfig& device, PlugQuerySnapshot& out)
{
    const int powerSiid = device.is_chuangmi ? 5 : 11;
    const int powerPiid = device.is_chuangmi ? 6 : 2;
    // Both currently supported plug families expose the main switch at 2/1.
    std::ostringstream req;
    req << "{\"id\":1,\"method\":\"get_properties\",\"params\":["
        << "{\"did\":\"power\",\"siid\":" << powerSiid << ",\"piid\":" << powerPiid << "},"
        << "{\"did\":\"switch\",\"siid\":2,\"piid\":1}]}";
    std::string json;
    if (!SendMiioRequest(device.ip, device.token, req.str(), json)) return false;

    std::vector<std::string> values;
    size_t pos = 0;
    while ((pos = json.find("\"value\":", pos)) != std::string::npos) {
        pos += 8;
        size_t end = json.find_first_of(",}", pos);
        values.push_back(json.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    if (values.empty()) return false;
    out.power = device.is_chuangmi ? std::atof(values[0].c_str()) / 100.0 : std::atof(values[0].c_str());
    if (values.size() > 1 && values[1] != "null") {
        out.state_supported = true;
        out.state = (values[1].find("true") != std::string::npos || std::atoi(values[1].c_str()) != 0)
            ? PlugSwitchState::On : PlugSwitchState::Off;
    }
    return true;
}

// 纯 C++ 原生 UDP 远程控制插座电源开关
static bool ControlPlugPowerUDP(const std::wstring& ipWStr, const std::wstring& tokenHexWStr, bool isChuangmi, bool turnOn)
{
    std::string ipStr = WStringToString(ipWStr);
    std::vector<uint8_t> token = HexStringToBytes(tokenHexWStr);
    if (token.size() != 16) return false;

    std::vector<uint8_t> key = NativeMD5(token);
    std::vector<uint8_t> key_plus_token = key;
    key_plus_token.insert(key_plus_token.end(), token.begin(), token.end());
    std::vector<uint8_t> iv = NativeMD5(key_plus_token);

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return false;

    sockaddr_in localAddr{};
    localAddr.sin_family = AF_INET;
    localAddr.sin_addr.s_addr = INADDR_ANY;
    localAddr.sin_port = 0;
    bind(sock, (sockaddr*)&localAddr, sizeof(localAddr));

    int timeout = 600;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(54321);
    inet_pton(AF_INET, ipStr.c_str(), &serverAddr.sin_addr);

    std::vector<uint8_t> handshake(32, 0xFF);
    handshake[0] = 0x21; handshake[1] = 0x31;
    handshake[2] = 0x00; handshake[3] = 0x20;

    if (sendto(sock, (const char*)handshake.data(), (int)handshake.size(), 0, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR)
    {
        closesocket(sock);
        return false;
    }

    std::vector<uint8_t> resp(1024, 0);
    int respLen = recv(sock, (char*)resp.data(), (int)resp.size(), 0);
    if (respLen < 32 || resp[0] != 0x21 || resp[1] != 0x31)
    {
        closesocket(sock);
        return false;
    }

    uint32_t stamp = (resp[12] << 24) | (resp[13] << 16) | (resp[14] << 8) | resp[15];
    stamp += 1;

    std::string json_req = turnOn
        ? "{\"id\":1,\"method\":\"set_properties\",\"params\":[{\"siid\":2,\"piid\":1,\"value\":true}]}"
        : "{\"id\":1,\"method\":\"set_properties\",\"params\":[{\"siid\":2,\"piid\":1,\"value\":false}]}";

    std::vector<uint8_t> plain_payload(json_req.begin(), json_req.end());
    plain_payload.push_back(0x00);

    std::vector<uint8_t> enc_payload = NativeAES_CBC(plain_payload, key, iv, true);

    uint16_t packet_len = (uint16_t)(32 + enc_payload.size());

    std::vector<uint8_t> header_16(16, 0);
    header_16[0] = 0x21; header_16[1] = 0x31;
    header_16[2] = (packet_len >> 8) & 0xFF; header_16[3] = packet_len & 0xFF;
    header_16[4] = 0; header_16[5] = 0; header_16[6] = 0; header_16[7] = 0;
    header_16[8] = resp[8]; header_16[9] = resp[9]; header_16[10] = resp[10]; header_16[11] = resp[11];
    header_16[12] = (stamp >> 24) & 0xFF; header_16[13] = (stamp >> 16) & 0xFF;
    header_16[14] = (stamp >> 8) & 0xFF; header_16[15] = stamp & 0xFF;

    std::vector<uint8_t> check_buf;
    check_buf.insert(check_buf.end(), header_16.begin(), header_16.end());
    check_buf.insert(check_buf.end(), token.begin(), token.end());
    check_buf.insert(check_buf.end(), enc_payload.begin(), enc_payload.end());

    std::vector<uint8_t> checksum = NativeMD5(check_buf);

    std::vector<uint8_t> packet;
    packet.insert(packet.end(), header_16.begin(), header_16.end());
    packet.insert(packet.end(), checksum.begin(), checksum.end());
    packet.insert(packet.end(), enc_payload.begin(), enc_payload.end());

    sendto(sock, (const char*)packet.data(), (int)packet.size(), 0, (sockaddr*)&serverAddr, sizeof(serverAddr));

    respLen = recv(sock, (char*)resp.data(), (int)resp.size(), 0);
    closesocket(sock);
    return (respLen > 32);
}

struct DiscoveredDevice {
    std::wstring ip;
    uint32_t did;
    uint32_t stamp;
};

// 局域网全网 C++ 多线程并发 1~254 子网探索扫描
static std::wstring ScanLocalMiioDevicesUDP(const std::vector<std::wstring>& configured_ips = {}, const std::wstring& cidr = L"")
{
    std::wstringstream log;
    log << L"==============================================\r\n"
        << L"   局域网全网 C++ 多线程探索发现结果\r\n"
        << L"==============================================\r\n\r\n";

    char hostName[256] = { 0 };
    gethostname(hostName, sizeof(hostName));
    hostent* host = gethostbyname(hostName);
    std::string prefix = "192.168.2.";

    if (host && host->h_addr_list[0])
    {
        in_addr addr;
        memcpy(&addr, host->h_addr_list[0], sizeof(in_addr));
        std::string myIp = inet_ntoa(addr);
        size_t lastDot = myIp.find_last_of('.');
        if (lastDot != std::string::npos) {
            prefix = myIp.substr(0, lastDot + 1);
        }
    }

    std::vector<std::string> prefixes;
    auto add_prefix = [&prefixes](const std::string& ip) {
        sockaddr_in addr{};
        if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) return;
        size_t lastDot = ip.find_last_of('.');
        if (lastDot == std::string::npos) return;
        std::string candidate = ip.substr(0, lastDot + 1);
        if (std::find(prefixes.begin(), prefixes.end(), candidate) == prefixes.end())
            prefixes.push_back(candidate);
    };

    add_prefix(prefix + "1");
    for (const auto& configured_ip : configured_ips)
        add_prefix(WStringToString(configured_ip));
    if (!cidr.empty()) {
        const std::string value = WStringToString(cidr);
        const size_t slash = value.find('/');
        const int bits = slash == std::string::npos ? 24 : std::atoi(value.c_str() + slash + 1);
        if (bits == 24) add_prefix(value.substr(0, slash));
        else log << L"提示：手动扫描当前仅支持 /24 网段，已忽略 " << cidr << L"。\r\n";
    }

    std::vector<std::string> scanTargets;
    for (const auto& scanPrefix : prefixes)
    {
        for (int i = 1; i <= 254; ++i)
            scanTargets.push_back(scanPrefix + std::to_string(i));
    }

    std::mutex discMutex;
    std::vector<DiscoveredDevice> discList;

    const int THREAD_COUNT = 16;
    std::vector<std::thread> threads;

    for (int t = 0; t < THREAD_COUNT; ++t)
    {
        threads.emplace_back([t, THREAD_COUNT, &scanTargets, &discMutex, &discList]() {
            SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (sock == INVALID_SOCKET) return;

            int timeout = 250;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

            std::vector<uint8_t> handshake(32, 0xFF);
            handshake[0] = 0x21; handshake[1] = 0x31;
            handshake[2] = 0x00; handshake[3] = 0x20;

            for (size_t i = t; i < scanTargets.size(); i += THREAD_COUNT)
            {
                const std::string& targetIp = scanTargets[i];
                sockaddr_in serverAddr{};
                serverAddr.sin_family = AF_INET;
                serverAddr.sin_port = htons(54321);
                inet_pton(AF_INET, targetIp.c_str(), &serverAddr.sin_addr);

                sendto(sock, (const char*)handshake.data(), (int)handshake.size(), 0, (sockaddr*)&serverAddr, sizeof(serverAddr));

                std::vector<uint8_t> resp(1024, 0);
                sockaddr_in fromAddr{};
                int fromLen = sizeof(fromAddr);
                int respLen = recvfrom(sock, (char*)resp.data(), (int)resp.size(), 0, (sockaddr*)&fromAddr, &fromLen);

                if (respLen >= 32 && resp[0] == 0x21 && resp[1] == 0x31 &&
                    fromAddr.sin_addr.s_addr == serverAddr.sin_addr.s_addr)
                {
                    char ipBuf[64] = { 0 };
                    inet_ntop(AF_INET, &fromAddr.sin_addr, ipBuf, sizeof(ipBuf));
                    uint32_t devId = (resp[8] << 24) | (resp[9] << 16) | (resp[10] << 8) | resp[11];
                    uint32_t stamp = (resp[12] << 24) | (resp[13] << 16) | (resp[14] << 8) | resp[15];

                    wchar_t wIp[64];
                    MultiByteToWideChar(CP_ACP, 0, ipBuf, -1, wIp, 64);

                    DiscoveredDevice dev;
                    dev.ip = wIp;
                    dev.did = devId;
                    dev.stamp = stamp;

                    std::lock_guard<std::mutex> lock(discMutex);
                    const bool already_found = std::any_of(discList.begin(), discList.end(),
                        [&](const DiscoveredDevice& existing) { return existing.ip == dev.ip; });
                    if (!already_found) discList.push_back(dev);
                }
            }
            closesocket(sock);
        });
    }

    for (auto& th : threads) {
        if (th.joinable()) th.join();
    }

    if (discList.empty())
    {
        log << L"未扫描到响应的小米设备，请确认设备已连入子网: " << std::wstring(prefix.begin(), prefix.end()).c_str() << L"*\r\n";
    }
    else
    {
        log << L"🎉 扫描成功！在子网 " << std::wstring(prefix.begin(), prefix.end()).c_str() << L"* 找到 " << discList.size() << L" 台在线小米智能设备:\r\n\r\n";
        int idx = 1;
        for (const auto& dev : discList)
        {
            log << L"[" << idx++ << L"] 在线硬件设备:\r\n"
                << L"    IP 地址: " << dev.ip << L"\r\n"
                << L"    设备 ID (DID): 0x" << std::hex << dev.did << std::dec << L" (" << dev.did << L")\r\n"
                << L"    握手时间戳 Stamp: " << dev.stamp << L"\r\n\r\n";
        }
    }

    return log.str();
}

// 1. CTotalPowerItem (插座总功率)
const wchar_t* CTotalPowerItem::GetItemName() const { return L"插座总功率"; }
const wchar_t* CTotalPowerItem::GetItemId() const { return L"xiaomi_total_power"; }
const wchar_t* CTotalPowerItem::GetItemLableText() const
{
    static std::wstring label;
    label = m_plugin.GetTotalPowerLabelText();
    return label.c_str();
}
const wchar_t* CTotalPowerItem::GetItemValueText() const
{
    static std::wstring text;
    text = m_plugin.GetTotalPowerValueText();
    return text.c_str();
}
const wchar_t* CTotalPowerItem::GetItemValueSampleText() const { return L"999.99 W"; }
int CTotalPowerItem::OnMouseEvent(MouseEventType type, int x, int y, void* hWnd, int flag)
{
    if (type == MT_DBCLICKED)
    {
        m_plugin.ShowOptionsDialog(hWnd);
        return 1;
    }
    return 0;
}
float CTotalPowerItem::GetResourceUsageGraphValue() const
{
    return m_plugin.GetPowerGraphValue();
}

// 2. CDynamicPlugItem (stable device id; list order is no longer an identity)
const wchar_t* CDynamicPlugItem::GetItemName() const
{
    static std::wstring name;
    name = m_plugin.GetPlugLabelText(m_device_id) + L" 功率";
    return name.c_str();
}
const wchar_t* CDynamicPlugItem::GetItemId() const
{
    m_id_cache = L"xiaomi_plug_power_" + m_device_id;
    return m_id_cache.c_str();
}
const wchar_t* CDynamicPlugItem::GetItemLableText() const
{
    static std::wstring label;
    label = m_plugin.GetPlugLabelText(m_device_id);
    return label.c_str();
}
const wchar_t* CDynamicPlugItem::GetItemValueText() const
{
    static std::wstring text;
    text = m_plugin.GetPlugPowerValueText(m_device_id);
    return text.c_str();
}
const wchar_t* CDynamicPlugItem::GetItemValueSampleText() const { return L"999.99 W"; }
int CDynamicPlugItem::OnMouseEvent(MouseEventType type, int x, int y, void* hWnd, int flag)
{
    if (type == MT_DBCLICKED)
    {
        m_plugin.TogglePlugAsync(m_device_id);
        return 1;
    }
    return 0;
}

const wchar_t* CPlugStatusItem::GetItemName() const
{
    static std::wstring name;
    name = m_plugin.GetPlugLabelText(m_device_id) + L" 状态";
    return name.c_str();
}
const wchar_t* CPlugStatusItem::GetItemId() const { m_id_cache = L"xiaomi_plug_status_" + m_device_id; return m_id_cache.c_str(); }
const wchar_t* CPlugStatusItem::GetItemLableText() const
{
    static std::wstring label;
    label = m_plugin.GetPlugLabelText(m_device_id);
    return label.c_str();
}
const wchar_t* CPlugStatusItem::GetItemValueText() const
{
    static std::wstring text;
    text = m_plugin.GetPlugStatusValueText(m_device_id);
    return text.c_str();
}
const wchar_t* CPlugStatusItem::GetItemValueSampleText() const { return L"开启 在线 12:34"; }

const wchar_t* CGroupPowerItem::GetItemName() const
{
    static std::wstring name;
    name = m_plugin.GetGroupLabelText(m_group_id) + L" 功率";
    return name.c_str();
}
const wchar_t* CGroupPowerItem::GetItemId() const { m_id_cache = L"xiaomi_group_power_" + m_group_id; return m_id_cache.c_str(); }
const wchar_t* CGroupPowerItem::GetItemLableText() const
{
    static std::wstring label;
    label = m_plugin.GetGroupLabelText(m_group_id);
    return label.c_str();
}
const wchar_t* CGroupPowerItem::GetItemValueText() const
{
    static std::wstring text;
    text = m_plugin.GetGroupPowerValueText(m_group_id);
    return text.c_str();
}
const wchar_t* CGroupPowerItem::GetItemValueSampleText() const { return L"999.99 W"; }

// 3. 室内温度显示项
const wchar_t* CTemperatureItem::GetItemName() const { return L"室内温度"; }
const wchar_t* CTemperatureItem::GetItemId() const { return L"xiaomi_ble_temp"; }
const wchar_t* CTemperatureItem::GetItemLableText() const
{
    static std::wstring label;
    label = m_plugin.GetTemperatureLabelText();
    return label.c_str();
}
const wchar_t* CTemperatureItem::GetItemValueText() const
{
    static std::wstring text;
    text = m_plugin.GetTemperatureValueText();
    return text.c_str();
}
const wchar_t* CTemperatureItem::GetItemValueSampleText() const { return L"99.9 \u2103"; }
int CTemperatureItem::OnMouseEvent(MouseEventType type, int x, int y, void* hWnd, int flag)
{
    if (type == MT_DBCLICKED)
    {
        m_plugin.ShowOptionsDialog(hWnd);
        return 1;
    }
    return 0;
}

// 4. 室内湿度显示项
const wchar_t* CHumidityItem::GetItemName() const { return L"室内湿度"; }
const wchar_t* CHumidityItem::GetItemId() const { return L"xiaomi_ble_humidity"; }
const wchar_t* CHumidityItem::GetItemLableText() const
{
    static std::wstring label;
    label = m_plugin.GetHumidityLabelText();
    return label.c_str();
}
const wchar_t* CHumidityItem::GetItemValueText() const
{
    static std::wstring text;
    text = m_plugin.GetHumidityValueText();
    return text.c_str();
}
const wchar_t* CHumidityItem::GetItemValueSampleText() const { return L"100 %"; }
int CHumidityItem::OnMouseEvent(MouseEventType type, int x, int y, void* hWnd, int flag)
{
    if (type == MT_DBCLICKED)
    {
        m_plugin.ShowOptionsDialog(hWnd);
        return 1;
    }
    return 0;
}

// --- CXiaomiIoTPlugin 实现 ---
CXiaomiIoTPlugin::CXiaomiIoTPlugin()
    : m_total_power_item(*this),
      m_temp_item(*this),
      m_humidity_item(*this)
{
    // TrafficMonitor supplies the plugin configuration directory in OnLoad.
    // Building dynamic items before that point can leave the host with stale
    // pointers from a fallback configuration, shown as "未知插座" later.
}

CXiaomiIoTPlugin::~CXiaomiIoTPlugin()
{
    m_running = false;
    if (m_ble_thread.joinable()) m_ble_thread.join();
    if (m_plug_thread.joinable()) m_plug_thread.join();
}

CXiaomiIoTPlugin& CXiaomiIoTPlugin::Instance()
{
    return s_instance;
}

void CXiaomiIoTPlugin::RebuildPlugItems()
{
    m_plug_items.clear();
    m_status_items.clear();
    m_group_items.clear();
    for (const auto& device : m_config.devices)
    {
        m_plug_items.push_back(std::make_unique<CDynamicPlugItem>(*this, device.id));
        m_status_items.push_back(std::make_unique<CPlugStatusItem>(*this, device.id));
    }
    for (const auto& group : m_config.groups)
    {
        m_group_items.push_back(std::make_unique<CGroupPowerItem>(*this, group.id));
    }
}

IPluginItem* CXiaomiIoTPlugin::GetItem(int index)
{
    if (index == 0) return &m_total_power_item;

    size_t plugCount = m_plug_items.size();
    size_t statusCount = m_status_items.size();
    size_t groupCount = m_group_items.size();
    if (index >= 1 && (size_t)index <= plugCount) return m_plug_items[index - 1].get();
    if ((size_t)index > plugCount && (size_t)index <= plugCount + statusCount)
        return m_status_items[index - 1 - plugCount].get();
    if ((size_t)index > plugCount + statusCount && (size_t)index <= plugCount + statusCount + groupCount)
        return m_group_items[index - 1 - plugCount - statusCount].get();
    if ((size_t)index == plugCount + statusCount + groupCount + 1) return &m_temp_item;
    if ((size_t)index == plugCount + statusCount + groupCount + 2) return &m_humidity_item;

    return nullptr;
}

void CXiaomiIoTPlugin::DataRequired()
{
}

const wchar_t* CXiaomiIoTPlugin::GetInfo(PluginInfoIndex index)
{
    switch (index)
    {
    case TMI_NAME: return L"小米智能设备插件";
    case TMI_DESCRIPTION: return L"小米智能设备监控插件，支持智能插座功率监控与控制，以及蓝牙温湿度计数据展示。";
    case TMI_AUTHOR: return L"daimiaopeng";
    case TMI_COPYRIGHT: return L"Copyright (C) 2026 Daimiaopeng";
    case TMI_VERSION: return L"1.0.0";
    case TMI_URL: return L"https://github.com/daimiaopeng/TrafficMonitor-Xiaomi-IoT-Plugin";
    default: return L"";
    }
}

int CXiaomiIoTPlugin::GetCommandCount()
{
    std::lock_guard<std::mutex> lock(m_data_mutex);
    return (int)(m_config.devices.size() * 2 + 5);
}

const wchar_t* CXiaomiIoTPlugin::GetCommandName(int command_index)
{
    PluginConfig cfg = GetConfig();
    size_t devCount = cfg.devices.size();

    if (command_index >= 0 && (size_t)command_index < devCount * 2)
    {
        size_t devIdx = command_index / 2;
        bool isTurnOn = (command_index % 2 == 0);
        m_cmd_name_cache = (isTurnOn ? L"⚡ 开启 " : L"🔌 关闭 ") + cfg.devices[devIdx].label;
        return m_cmd_name_cache.c_str();
    }
    else if ((size_t)command_index == devCount * 2)
    {
        return L"🔍 局域网全网 C++ 多线程极速扫描";
    }
    else if ((size_t)command_index == devCount * 2 + 1)
    {
        return L"⚙️ 打开小米插件高级设置与诊断";
    }
    else if ((size_t)command_index == devCount * 2 + 2) return L"恢复插座类默认设置";
    else if ((size_t)command_index == devCount * 2 + 3) return L"恢复温湿度类默认设置";
    else if ((size_t)command_index == devCount * 2 + 4) return L"恢复报警默认设置";

    return nullptr;
}

void CXiaomiIoTPlugin::OnPluginCommand(int command_index, void* hWnd, void* para)
{
    PluginConfig cfg = GetConfig();
    HWND hWndParent = (HWND)hWnd;
    size_t devCount = cfg.devices.size();

    if (command_index >= 0 && (size_t)command_index < devCount * 2)
    {
        size_t devIdx = command_index / 2;
        bool turnOn = (command_index % 2 == 0);
        PlugDeviceConfig devCfg = cfg.devices[devIdx];
        // Commands are explicit on/off, while a display double-click toggles.
        std::thread([this, devCfg, turnOn]() {
            bool sent = ControlPlugPowerUDP(devCfg.ip, devCfg.token, devCfg.is_chuangmi, turnOn);
            if (m_app) {
                const std::wstring msg = sent
                    ? devCfg.label + (turnOn ? L" 已发送开启指令，将在下一次刷新后确认。" : L" 已发送关闭指令，将在下一次刷新后确认。")
                    : devCfg.label + L" 开关指令发送失败。";
                m_app->ShowNotifyMessage(msg.c_str());
            }
        }).detach();
        return;

        std::thread([hWndParent, devCfg, turnOn]() {
            bool ok = ControlPlugPowerUDP(devCfg.ip, devCfg.token, devCfg.is_chuangmi, turnOn);
            std::wstring msgStr = devCfg.label + (turnOn ? L" 已成功通电开启！" : L" 已成功断电关闭！");
            MessageBoxW(hWndParent, ok ? msgStr.c_str() : L"发送指令失败，请检查 IP 与 Token！",
                        L"电源控制结果", ok ? MB_ICONINFORMATION : MB_ICONERROR);
        }).detach();
    }
    else if ((size_t)command_index == devCount * 2)
    {
        std::vector<std::wstring> configured_ips;
        for (const auto& dev : cfg.devices) configured_ips.push_back(dev.ip);
        std::thread([hWndParent, configured_ips]() {
            std::wstring scanRes = ScanLocalMiioDevicesUDP(configured_ips);
            MessageBoxW(hWndParent, scanRes.c_str(), L"局域网小米智能设备探索报告", MB_ICONINFORMATION);
        }).detach();
    }
    else if ((size_t)command_index == devCount * 2 + 1)
    {
        ShowOptionsDialog(hWndParent);
    }
    else if ((size_t)command_index >= devCount * 2 + 2 && (size_t)command_index <= devCount * 2 + 4)
    {
        ResetCategoryDefaults(command_index - (int)(devCount * 2 + 2));
        if (m_app) m_app->ShowNotifyMessage(L"已恢复对应类别的默认设置。");
    }
}

const wchar_t* CXiaomiIoTPlugin::GetTooltipInfo()
{
    std::lock_guard<std::mutex> lock(m_data_mutex);
    std::wstringstream ss;
    double monthly_cost = (m_total_power / 1000.0) * 24.0 * 30.0 * m_config.electricity_price;

    double dewPoint = m_ble_temperature - ((100.0 - m_ble_humidity) / 5.0);
    std::wstring comfort = L"舒适";
    if (m_ble_temperature > 28.0 && m_ble_humidity > 60.0) comfort = L"闷热";
    else if (m_ble_temperature < 16.0) comfort = L"偏冷";

    ss << L"小米智能设备监控\r\n"
       << L"---------------------------------------\r\n";

    for (const auto& dev : m_config.devices)
    {
        ss << L"⚡ " << dev.label << L": " << std::fixed << std::setprecision(m_config.precision) << dev.power << L" W (" << (dev.online ? L"🟢 在线" : L"🔴 离线") << L")\r\n";
    }

    ss << L"💡 实时" << m_config.total_label << L": " << std::fixed << std::setprecision(m_config.precision) << m_total_power << L" W\r\n"
       << L"💰 预估月电费: ~" << std::fixed << std::setprecision(1) << monthly_cost << L" 元 (单价:" << m_config.electricity_price << L"元/度)\r\n"
       << L"---------------------------------------\r\n"
       << L"🌡️ 室内" << m_config.temp_label << L": " << (m_ble_online ? std::to_wstring((int)m_ble_temperature) + L" ℃ (🟢 已连接)" : L"-- ℃ (🔴 离线)") << L"\r\n"
       << L"💧 室内" << m_config.hum_label << L": " << (m_ble_online ? std::to_wstring((int)m_ble_humidity) + L" %" : L"-- %") << L"\r\n"
       << L"🍃 露点/体感: " << (m_ble_online ? std::to_wstring((int)dewPoint) + L" ℃ (" + comfort + L")" : L"--") << L"\r\n"
       << L"💡 提示: 鼠标双击直接打开高阶控制设置";

    m_tooltip_cache = ss.str();
    return m_tooltip_cache.c_str();
}

float CXiaomiIoTPlugin::GetPowerGraphValue() const
{
    std::lock_guard<std::mutex> lock(m_data_mutex);
    if (m_config.alarm_power_limit <= 0.0) return 0.0f;
    float val = (float)(m_total_power / m_config.alarm_power_limit);
    if (val > 1.0f) val = 1.0f;
    if (val < 0.0f) val = 0.0f;
    return val;
}

std::wstring CXiaomiIoTPlugin::GetHistoryFilePath() const
{
    std::wstring path = GetConfigFilePath();
    const std::wstring suffix = L".ini";
    if (path.size() >= suffix.size() && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0)
        path.resize(path.size() - suffix.size());
    return path + L"_power_history.csv";
}

std::vector<PowerHistoryPoint> CXiaomiIoTPlugin::GetPowerHistorySnapshot(const std::wstring& series_id) const
{
    std::lock_guard<std::mutex> lock(m_data_mutex);
    const auto it = m_power_history.find(series_id);
    return it == m_power_history.end() ? std::vector<PowerHistoryPoint>{}
                                      : std::vector<PowerHistoryPoint>(it->second.begin(), it->second.end());
}

std::vector<PowerHistoryPoint> CXiaomiIoTPlugin::GetPowerHistorySnapshot() const
{
    std::lock_guard<std::mutex> lock(m_data_mutex);
    const auto it = m_power_history.find(m_selected_history_series);
    return it == m_power_history.end() ? std::vector<PowerHistoryPoint>{}
                                      : std::vector<PowerHistoryPoint>(it->second.begin(), it->second.end());
}

std::vector<std::pair<std::wstring, std::wstring>> CXiaomiIoTPlugin::GetHistorySeries() const
{
    std::lock_guard<std::mutex> lock(m_data_mutex);
    std::vector<std::pair<std::wstring, std::wstring>> result{{L"total", m_config.total_label}};
    for (const auto& group : m_config.groups) result.push_back({L"group:" + group.id, group.name});
    for (const auto& device : m_config.devices) result.push_back({L"device:" + device.id, device.label});
    return result;
}

void CXiaomiIoTPlugin::SetHistorySeries(const std::wstring& series_id)
{
    std::lock_guard<std::mutex> lock(m_data_mutex);
    m_selected_history_series = series_id.empty() ? L"total" : series_id;
}

std::wstring CXiaomiIoTPlugin::GetHistoryStatisticsText() const
{
    std::lock_guard<std::mutex> lock(m_data_mutex);
    const auto it = m_power_history.find(m_selected_history_series);
    if (it == m_power_history.end() || it->second.empty()) return L"今日暂无历史数据";
    const std::time_t now = std::time(nullptr);
    tm day{}; localtime_s(&day, &now); day.tm_hour = day.tm_min = day.tm_sec = 0;
    const std::time_t midnight = std::mktime(&day);
    double peak = 0.0, low = 0.0, sum = 0.0; size_t count = 0;
    for (const auto& point : it->second) {
        if (point.timestamp < midnight) continue;
        if (count == 0) { peak = low = point.power; }
        peak = std::max(peak, point.power); low = std::min(low, point.power); sum += point.power; ++count;
    }
    if (!count) return L"今日暂无历史数据";
    std::wstringstream ss;
    ss << std::fixed << std::setprecision(1) << L"今日峰值 " << peak << L" W   平均 " << sum / count << L" W   最低 " << low << L" W";
    return ss.str();
}

void CXiaomiIoTPlugin::LoadPowerHistoryFromFile()
{
    std::ifstream file(WStringToString(GetHistoryFilePath()));
    if (!file.is_open()) return;

    std::unordered_map<std::wstring, std::deque<PowerHistoryPoint>> loaded;
    std::string line;
    while (std::getline(file, line))
    {
        std::stringstream ss(line);
        long long timestamp = 0; double power = 0.0; std::string series; char comma = 0;
        if (ss >> timestamp >> comma && comma == ',') {
            if (std::getline(ss, series, ',') && ss >> power)
                loaded[std::wstring(series.begin(), series.end())].push_back({ (std::time_t)timestamp, power });
            else {
                // Version 1 files used timestamp,power and are migrated into total.
                power = std::atof(series.c_str());
                if (!series.empty()) loaded[L"total"].push_back({ (std::time_t)timestamp, power });
            }
        }
    }

    const size_t maxPoints = 30 * 24 * 60;
    for (auto& entry : loaded) while (entry.second.size() > maxPoints) entry.second.pop_front();
    std::lock_guard<std::mutex> lock(m_data_mutex);
    m_power_history = std::move(loaded);
    const auto total = m_power_history.find(L"total");
    if (total != m_power_history.end() && !total->second.empty()) m_last_history_sample = total->second.back().timestamp;
}

void CXiaomiIoTPlugin::RecordPowerHistory(const std::unordered_map<std::wstring, double>& samples)
{
    const std::time_t now = std::time(nullptr);
    std::unordered_map<std::wstring, std::deque<PowerHistoryPoint>> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        if (m_last_history_sample != 0 && now - m_last_history_sample < 60) return;
        m_last_history_sample = now;
        const size_t maxPoints = 30 * 24 * 60;
        for (const auto& sample : samples) {
            auto& series = m_power_history[sample.first];
            series.push_back({ now, sample.second });
            while (series.size() > maxPoints) series.pop_front();
        }
        snapshot = m_power_history;
    }

    std::ofstream file(WStringToString(GetHistoryFilePath()), std::ios::trunc);
    if (!file.is_open()) return;
    file << "timestamp,series,power\n";
    for (const auto& entry : snapshot)
        for (const auto& point : entry.second)
            file << (long long)point.timestamp << "," << WStringToString(entry.first) << "," << point.power << "\n";
}

void CXiaomiIoTPlugin::RecordPowerHistory(double total_power)
{
    RecordPowerHistory({{L"total", total_power}});
}

void CXiaomiIoTPlugin::ClearPowerHistory()
{
    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        m_power_history.clear();
        m_last_history_sample = 0;
    }
    DeleteFileW(GetHistoryFilePath().c_str());
}

void CXiaomiIoTPlugin::OnInitialize(ITrafficMonitor* pApp)
{
    m_app = pApp;
    if (!m_loaded_app_config)
    {
        LoadConfigFromFile();
        RebuildPlugItems();
        LoadPowerHistoryFromFile();
        m_loaded_app_config = true;
    }
    if (!m_running)
    {
        m_running = true;
        StartBackgroundThreads();
    }
}

void CXiaomiIoTPlugin::StartBackgroundThreads()
{
    m_ble_thread = std::thread([this]() { BleWorkerThread(); });
    m_plug_thread = std::thread([this]() { PlugWorkerThread(); });
}

PluginConfig CXiaomiIoTPlugin::GetConfig()
{
    std::lock_guard<std::mutex> lock(m_data_mutex);
    return m_config;
}

void CXiaomiIoTPlugin::SaveConfig(const PluginConfig& config)
{
    PluginConfig normalized = config;
    EnsureConfigDefaults(normalized);
    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        m_config = normalized;
        RebuildPlugItems();
    }
    SaveConfigToFile();
}

void CXiaomiIoTPlugin::ResetDefaultConfig()
{
    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        m_config = PluginConfig();
    PlugDeviceConfig dev1; dev1.ip = L"192.168.2.6"; dev1.token.clear(); dev1.label = L"插座 [192.168.2.6]"; dev1.is_chuangmi = true;
    PlugDeviceConfig dev2; dev2.ip = L"192.168.2.3"; dev2.token.clear(); dev2.label = L"插座 [192.168.2.3]"; dev2.is_chuangmi = false;
        m_config.devices = { dev1, dev2 };
        EnsureConfigDefaults(m_config);
        RebuildPlugItems();
    }
    SaveConfigToFile();
}

void CXiaomiIoTPlugin::ResetCategoryDefaults(int category)
{
    PluginConfig config = GetConfig();
    if (category == 0) { config.refresh_interval_ms = 1000; config.precision = 2; for (auto& d : config.devices) { d.refresh_interval_ms = 0; d.precision = -1; } }
    if (category == 1) { config.thermometer_refresh_interval_ms = 10000; config.thermometer_precision = 2; }
    if (category == 2) { config.alarm_power_enabled = false; config.alarm_power_limit = 2000.0; config.alarm_cooldown_minutes = 10; config.offline_failures = 3; config.temp_alarm_enabled = config.humidity_alarm_enabled = false; }
    SaveConfig(config);
}

bool CXiaomiIoTPlugin::ExportConfig(const std::wstring& path, std::wstring& error)
{
    PluginConfig config = GetConfig();
    std::ofstream file(WStringToString(path), std::ios::trunc | std::ios::binary);
    if (!file.is_open()) { error = L"无法创建导出文件。"; return false; }
    auto quoted = [](const std::wstring& value) { return std::string("\"") + JsonEscape(value) + "\""; };
    file << "{\n  \"format\":2,\n";
    file << "  \"total_label\":" << quoted(config.total_label) << ",\n";
    file << "  \"ble_mac\":" << quoted(config.ble_mac) << ",\n";
    file << "  \"temp_label\":" << quoted(config.temp_label) << ",\n";
    file << "  \"hum_label\":" << quoted(config.hum_label) << ",\n";
    file << "  \"refresh_interval_ms\":" << config.refresh_interval_ms << ",\"precision\":" << config.precision << ",\n";
    file << "  \"thermometer_refresh_interval_ms\":" << config.thermometer_refresh_interval_ms << ",\"thermometer_precision\":" << config.thermometer_precision << ",\n";
    file << "  \"electricity_price\":" << config.electricity_price << ",\"alarm_power_enabled\":" << (config.alarm_power_enabled ? "true" : "false") << ",\"alarm_power_limit\":" << config.alarm_power_limit << ",\n";
    file << "  \"alarm_cooldown_minutes\":" << config.alarm_cooldown_minutes << ",\"offline_failures\":" << config.offline_failures << ",\n";
    file << "  \"temp_alarm_enabled\":" << (config.temp_alarm_enabled ? "true" : "false") << ",\"temp_low\":" << config.temp_low << ",\"temp_high\":" << config.temp_high << ",\n";
    file << "  \"humidity_alarm_enabled\":" << (config.humidity_alarm_enabled ? "true" : "false") << ",\"humidity_low\":" << config.humidity_low << ",\"humidity_high\":" << config.humidity_high << ",\n";
    file << "  \"groups\":[\n";
    for (size_t i = 0; i < config.groups.size(); ++i) { const auto& g = config.groups[i];
        file << "    {\"id\":" << quoted(g.id) << ",\"name\":" << quoted(g.name) << ",\"enabled\":" << (g.enabled ? "true" : "false") << ",\"refresh_ms\":" << g.refresh_interval_ms << ",\"precision\":" << g.precision << ",\"alarm_enabled\":" << (g.alarm_enabled ? "true" : "false") << ",\"power_limit\":" << g.power_limit << "}" << (i + 1 == config.groups.size() ? "\n" : ",\n"); }
    file << "  ],\n  \"devices\":[\n";
    for (size_t i = 0; i < config.devices.size(); ++i) { const auto& d = config.devices[i];
        file << "    {\"id\":" << quoted(d.id) << ",\"ip\":" << quoted(d.ip) << ",\"token\":" << quoted(d.token) << ",\"label\":" << quoted(d.label) << ",\"model\":" << quoted(d.model) << ",\"group\":" << quoted(d.group_id) << ",\"is_chuangmi\":" << (d.is_chuangmi ? "true" : "false") << ",\"enabled\":" << (d.enabled ? "true" : "false") << ",\"refresh_ms\":" << d.refresh_interval_ms << ",\"precision\":" << d.precision << ",\"alarm_enabled\":" << (d.alarm_enabled ? "true" : "false") << ",\"power_limit\":" << d.power_limit << "}" << (i + 1 == config.devices.size() ? "\n" : ",\n"); }
    file << "  ]\n}\n";
    if (!file.good()) { error = L"写入导出文件失败。"; return false; }
    return true;
}

bool CXiaomiIoTPlugin::ImportConfig(const std::wstring& path, std::wstring& error)
{
    std::ifstream file(WStringToString(path), std::ios::binary);
    if (!file.is_open()) { error = L"无法打开导入文件。"; return false; }
    std::string json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (JsonReadNumber(json, "format", 0) != 2) { error = L"不是受支持的插件导出文件。"; return false; }
    PluginConfig baseline = GetConfig();
    PluginConfig imported = baseline;
    JsonReadString(json, "total_label", imported.total_label); JsonReadString(json, "ble_mac", imported.ble_mac);
    JsonReadString(json, "temp_label", imported.temp_label); JsonReadString(json, "hum_label", imported.hum_label);
    imported.refresh_interval_ms = (int)JsonReadNumber(json, "refresh_interval_ms", imported.refresh_interval_ms);
    imported.precision = (int)JsonReadNumber(json, "precision", imported.precision);
    imported.thermometer_refresh_interval_ms = (int)JsonReadNumber(json, "thermometer_refresh_interval_ms", imported.thermometer_refresh_interval_ms);
    imported.thermometer_precision = (int)JsonReadNumber(json, "thermometer_precision", imported.thermometer_precision);
    imported.electricity_price = JsonReadNumber(json, "electricity_price", imported.electricity_price);
    imported.alarm_power_enabled = JsonReadBool(json, "alarm_power_enabled", imported.alarm_power_enabled);
    imported.alarm_power_limit = JsonReadNumber(json, "alarm_power_limit", imported.alarm_power_limit);
    imported.alarm_cooldown_minutes = (int)JsonReadNumber(json, "alarm_cooldown_minutes", imported.alarm_cooldown_minutes);
    imported.offline_failures = (int)JsonReadNumber(json, "offline_failures", imported.offline_failures);
    imported.temp_alarm_enabled = JsonReadBool(json, "temp_alarm_enabled", imported.temp_alarm_enabled);
    imported.temp_low = JsonReadNumber(json, "temp_low", imported.temp_low); imported.temp_high = JsonReadNumber(json, "temp_high", imported.temp_high);
    imported.humidity_alarm_enabled = JsonReadBool(json, "humidity_alarm_enabled", imported.humidity_alarm_enabled);
    imported.humidity_low = JsonReadNumber(json, "humidity_low", imported.humidity_low); imported.humidity_high = JsonReadNumber(json, "humidity_high", imported.humidity_high);
    imported.groups.clear();
    for (const auto& object : JsonReadObjectArray(json, "groups")) { GroupConfig g; JsonReadString(object, "id", g.id); JsonReadString(object, "name", g.name); g.enabled = JsonReadBool(object, "enabled", true); g.refresh_interval_ms = (int)JsonReadNumber(object, "refresh_ms", 0); g.precision = (int)JsonReadNumber(object, "precision", -1); g.alarm_enabled = JsonReadBool(object, "alarm_enabled", false); g.power_limit = JsonReadNumber(object, "power_limit", 0); if (!g.id.empty()) imported.groups.push_back(g); }
    imported.devices.clear();
    for (const auto& object : JsonReadObjectArray(json, "devices")) { PlugDeviceConfig d; JsonReadString(object, "id", d.id); JsonReadString(object, "ip", d.ip); JsonReadString(object, "token", d.token); JsonReadString(object, "label", d.label); JsonReadString(object, "model", d.model); JsonReadString(object, "group", d.group_id); d.is_chuangmi = JsonReadBool(object, "is_chuangmi", true); d.enabled = JsonReadBool(object, "enabled", true); d.refresh_interval_ms = (int)JsonReadNumber(object, "refresh_ms", 0); d.precision = (int)JsonReadNumber(object, "precision", -1); d.alarm_enabled = JsonReadBool(object, "alarm_enabled", false); d.power_limit = JsonReadNumber(object, "power_limit", 0); if (!d.ip.empty()) imported.devices.push_back(d); }
    if (imported.devices.empty()) { error = L"导入文件中没有设备。"; return false; }
    PluginConfig merged = baseline;
    merged.total_label = imported.total_label; merged.ble_mac = imported.ble_mac; merged.temp_label = imported.temp_label; merged.hum_label = imported.hum_label;
    merged.refresh_interval_ms = imported.refresh_interval_ms; merged.precision = imported.precision;
    merged.thermometer_refresh_interval_ms = imported.thermometer_refresh_interval_ms; merged.thermometer_precision = imported.thermometer_precision;
    merged.electricity_price = imported.electricity_price; merged.alarm_power_enabled = imported.alarm_power_enabled; merged.alarm_power_limit = imported.alarm_power_limit;
    merged.alarm_cooldown_minutes = imported.alarm_cooldown_minutes; merged.offline_failures = imported.offline_failures;
    merged.temp_alarm_enabled = imported.temp_alarm_enabled; merged.temp_low = imported.temp_low; merged.temp_high = imported.temp_high;
    merged.humidity_alarm_enabled = imported.humidity_alarm_enabled; merged.humidity_low = imported.humidity_low; merged.humidity_high = imported.humidity_high;
    for (const auto& group : imported.groups) {
        auto target = std::find_if(merged.groups.begin(), merged.groups.end(), [&](const GroupConfig& current) { return current.id == group.id; });
        if (target == merged.groups.end()) merged.groups.push_back(group); else *target = group;
    }
    for (const auto& device : imported.devices) {
        auto target = std::find_if(merged.devices.begin(), merged.devices.end(), [&](const PlugDeviceConfig& current) { return current.id == device.id || current.ip == device.ip; });
        if (target == merged.devices.end()) merged.devices.push_back(device);
        else { const double power = target->power; const bool online = target->online; const std::time_t seen = target->last_seen; *target = device; target->power = power; target->online = online; target->last_seen = seen; }
    }
    SaveConfig(merged);
    return true;
}

std::wstring CXiaomiIoTPlugin::GetConfigFilePath() const
{
    if (m_app)
    {
        const wchar_t* dir = m_app->GetPluginConfigDir();
        if (dir && *dir)
        {
            std::wstring path(dir);
            if (path.back() != L'\\' && path.back() != L'/') path += L'\\';
            return path + L"XiaomiIoTPlugin.ini";
        }
    }

    wchar_t path[MAX_PATH] = { 0 };
    GetModuleFileNameW(NULL, path, MAX_PATH);
    std::wstring exe_path(path);
    size_t pos = exe_path.find_last_of(L"\\/");
    if (pos != std::wstring::npos) exe_path.resize(pos + 1);
    return exe_path + L"XiaomiIoTPlugin.ini";
}

void CXiaomiIoTPlugin::EnsureConfigDefaults(PluginConfig& config)
{
    std::vector<GroupConfig> uniqueGroups;
    uniqueGroups.reserve(config.groups.size() + 1);
    for (auto group : config.groups) {
        if (group.id.empty()) group.id = MakeStableId(group.name.empty() ? L"group" : group.name, L"group");
        if (group.name.empty()) group.name = group.id;
        group.refresh_interval_ms = std::clamp(group.refresh_interval_ms, 0, 60000);
        group.precision = std::clamp(group.precision, -1, 4);
        const bool alreadyAdded = std::any_of(uniqueGroups.begin(), uniqueGroups.end(), [&](const GroupConfig& existing) { return existing.id == group.id; });
        if (!alreadyAdded) uniqueGroups.push_back(std::move(group));
    }
    if (std::none_of(uniqueGroups.begin(), uniqueGroups.end(), [](const GroupConfig& group) { return group.id == L"default"; }))
        uniqueGroups.insert(uniqueGroups.begin(), GroupConfig{});
    config.groups = std::move(uniqueGroups);
    for (auto& device : config.devices) {
        if (device.id.empty()) device.id = MakeStableId(device.ip + L"|" + device.token, L"plug");
        if (device.label.empty()) device.label = L"插座 [" + device.ip + L"]";
        if (device.group_id.empty()) device.group_id = L"default";
        if (std::none_of(config.groups.begin(), config.groups.end(), [&](const GroupConfig& g) { return g.id == device.group_id; })) {
            GroupConfig group;
            group.id = device.group_id;
            group.name = device.group_id;
            config.groups.push_back(group);
        }
        device.refresh_interval_ms = std::clamp(device.refresh_interval_ms, 0, 60000);
        device.precision = std::clamp(device.precision, -1, 4);
    }
    config.refresh_interval_ms = std::clamp(config.refresh_interval_ms, 500, 60000);
    config.thermometer_refresh_interval_ms = std::clamp(config.thermometer_refresh_interval_ms, 1000, 60000);
    config.alarm_cooldown_minutes = std::clamp(config.alarm_cooldown_minutes, 1, 1440);
    config.offline_failures = std::clamp(config.offline_failures, 1, 20);
    config.window_width = std::clamp(config.window_width, 500, 1200);
    config.window_height = std::clamp(config.window_height, 640, 1000);
}

void CXiaomiIoTPlugin::LoadConfigFromFile()
{
    std::wstring ini_path = GetConfigFilePath();
    if (m_app && GetFileAttributesW(ini_path.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        // 兼容旧版本：旧配置可能被写到了 TrafficMonitor.exe 所在目录。
        wchar_t path[MAX_PATH] = { 0 };
        GetModuleFileNameW(NULL, path, MAX_PATH);
        std::wstring legacy_path(path);
        size_t pos = legacy_path.find_last_of(L"\\/");
        if (pos != std::wstring::npos)
        {
            legacy_path.resize(pos + 1);
            legacy_path += L"XiaomiIoTPlugin.ini";
            if (GetFileAttributesW(legacy_path.c_str()) != INVALID_FILE_ATTRIBUTES)
                ini_path = legacy_path;
        }
    }

    const std::wstring env_path = GetEnvFilePath(ini_path);
    wchar_t buf[256];
    int count = GetPrivateProfileIntW(L"Plugs", L"Count", 0, ini_path.c_str());

    m_config.devices.clear();
    if (count > 0)
    {
        for (int i = 1; i <= count; ++i)
        {
            PlugDeviceConfig dev;
            std::wstring prefixKey = L"Plug" + std::to_wstring(i);

            GetPrivateProfileStringW(L"Plugs", (prefixKey + L"_IP").c_str(), L"192.168.2.6", buf, 256, ini_path.c_str()); dev.ip = buf;
            GetPrivateProfileStringW(L"Plugs", (prefixKey + L"_Token").c_str(), L"", buf, 256, ini_path.c_str()); dev.token = buf;
            if (dev.token.empty()) dev.token = ReadDotEnvValue(env_path, L"XIAOMI_PLUG_" + std::to_wstring(i) + L"_TOKEN");

            std::wstring defLabel = L"插座 [" + dev.ip + L"]";
            GetPrivateProfileStringW(L"Plugs", (prefixKey + L"_Label").c_str(), defLabel.c_str(), buf, 256, ini_path.c_str()); dev.label = buf;
            dev.is_chuangmi = (GetPrivateProfileIntW(L"Plugs", (prefixKey + L"_IsChuangmi").c_str(), 1, ini_path.c_str()) != 0);
            GetPrivateProfileStringW(L"Plugs", (prefixKey + L"_Id").c_str(), L"", buf, 256, ini_path.c_str()); dev.id = buf;
            GetPrivateProfileStringW(L"Plugs", (prefixKey + L"_Model").c_str(), L"", buf, 256, ini_path.c_str()); dev.model = buf;
            GetPrivateProfileStringW(L"Plugs", (prefixKey + L"_Group").c_str(), L"default", buf, 256, ini_path.c_str()); dev.group_id = buf;
            dev.enabled = GetPrivateProfileIntW(L"Plugs", (prefixKey + L"_Enabled").c_str(), 1, ini_path.c_str()) != 0;
            dev.refresh_interval_ms = GetPrivateProfileIntW(L"Plugs", (prefixKey + L"_RefreshMs").c_str(), 0, ini_path.c_str());
            dev.precision = GetPrivateProfileIntW(L"Plugs", (prefixKey + L"_Precision").c_str(), -1, ini_path.c_str());
            dev.alarm_enabled = GetPrivateProfileIntW(L"Plugs", (prefixKey + L"_AlarmEnabled").c_str(), 0, ini_path.c_str()) != 0;
            GetPrivateProfileStringW(L"Plugs", (prefixKey + L"_PowerLimit").c_str(), L"0", buf, 256, ini_path.c_str()); dev.power_limit = _wtof(buf);

            m_config.devices.push_back(dev);
        }
    }
    else
    {
        // 兼容旧配置
        PlugDeviceConfig dev1;
        GetPrivateProfileStringW(L"Chuangmi", L"IP", L"192.168.2.6", buf, 256, ini_path.c_str()); dev1.ip = buf;
        GetPrivateProfileStringW(L"Chuangmi", L"Token", L"", buf, 256, ini_path.c_str()); dev1.token = buf;
        if (dev1.token.empty()) dev1.token = ReadDotEnvValue(env_path, L"XIAOMI_CHUANGMI_TOKEN");
        GetPrivateProfileStringW(L"Chuangmi", L"Label", (L"插座 [" + dev1.ip + L"]").c_str(), buf, 256, ini_path.c_str()); dev1.label = buf;
        dev1.is_chuangmi = true;

        PlugDeviceConfig dev2;
        GetPrivateProfileStringW(L"Cuco", L"IP", L"192.168.2.3", buf, 256, ini_path.c_str()); dev2.ip = buf;
        GetPrivateProfileStringW(L"Cuco", L"Token", L"", buf, 256, ini_path.c_str()); dev2.token = buf;
        if (dev2.token.empty()) dev2.token = ReadDotEnvValue(env_path, L"XIAOMI_CUCO_TOKEN");
        GetPrivateProfileStringW(L"Cuco", L"Label", (L"插座 [" + dev2.ip + L"]").c_str(), buf, 256, ini_path.c_str()); dev2.label = buf;
        dev2.is_chuangmi = false;

        m_config.devices = { dev1, dev2 };
    }

    GetPrivateProfileStringW(L"Display", L"TotalLabel", L"总功率", buf, 256, ini_path.c_str()); m_config.total_label = buf;
    GetPrivateProfileStringW(L"Display", L"TempLabel", L"温度", buf, 256, ini_path.c_str()); m_config.temp_label = buf;
    GetPrivateProfileStringW(L"Display", L"HumLabel", L"湿度", buf, 256, ini_path.c_str()); m_config.hum_label = buf;

    GetPrivateProfileStringW(L"BLE", L"MAC", L"A4:C1:38:D5:C2:20", buf, 256, ini_path.c_str()); m_config.ble_mac = buf;

    m_config.refresh_interval_ms = GetPrivateProfileIntW(L"Preferences", L"RefreshIntervalMs", 1000, ini_path.c_str());
    m_config.precision = GetPrivateProfileIntW(L"Preferences", L"Precision", 2, ini_path.c_str());
    m_config.thermometer_refresh_interval_ms = GetPrivateProfileIntW(L"Preferences", L"ThermometerRefreshIntervalMs", 10000, ini_path.c_str());
    m_config.thermometer_precision = GetPrivateProfileIntW(L"Preferences", L"ThermometerPrecision", 2, ini_path.c_str());
    if (m_config.refresh_interval_ms < 500) m_config.refresh_interval_ms = 500;
    if (m_config.thermometer_refresh_interval_ms < 1000) m_config.thermometer_refresh_interval_ms = 1000;

    GetPrivateProfileStringW(L"Preferences", L"ElectricityPrice", L"0.6", buf, 256, ini_path.c_str()); m_config.electricity_price = _wtof(buf);

    m_config.alarm_power_enabled = GetPrivateProfileIntW(L"Alarm", L"PowerAlarmEnabled", 0, ini_path.c_str()) != 0;
    GetPrivateProfileStringW(L"Alarm", L"PowerLimit", L"2000.0", buf, 256, ini_path.c_str()); m_config.alarm_power_limit = _wtof(buf);
    m_config.alarm_cooldown_minutes = GetPrivateProfileIntW(L"Alarm", L"CooldownMinutes", 10, ini_path.c_str());
    m_config.offline_failures = GetPrivateProfileIntW(L"Alarm", L"OfflineFailures", 3, ini_path.c_str());
    m_config.temp_alarm_enabled = GetPrivateProfileIntW(L"Alarm", L"TempEnabled", 0, ini_path.c_str()) != 0;
    GetPrivateProfileStringW(L"Alarm", L"TempLow", L"0", buf, 256, ini_path.c_str()); m_config.temp_low = _wtof(buf);
    GetPrivateProfileStringW(L"Alarm", L"TempHigh", L"40", buf, 256, ini_path.c_str()); m_config.temp_high = _wtof(buf);
    m_config.humidity_alarm_enabled = GetPrivateProfileIntW(L"Alarm", L"HumidityEnabled", 0, ini_path.c_str()) != 0;
    GetPrivateProfileStringW(L"Alarm", L"HumidityLow", L"0", buf, 256, ini_path.c_str()); m_config.humidity_low = _wtof(buf);
    GetPrivateProfileStringW(L"Alarm", L"HumidityHigh", L"90", buf, 256, ini_path.c_str()); m_config.humidity_high = _wtof(buf);
    m_config.window_x = GetPrivateProfileIntW(L"Window", L"X", INT_MIN, ini_path.c_str());
    m_config.window_y = GetPrivateProfileIntW(L"Window", L"Y", INT_MIN, ini_path.c_str());
    m_config.window_width = GetPrivateProfileIntW(L"Window", L"Width", 650, ini_path.c_str());
    m_config.window_height = GetPrivateProfileIntW(L"Window", L"Height", 720, ini_path.c_str());

    int groupCount = GetPrivateProfileIntW(L"Groups", L"Count", 0, ini_path.c_str());
    for (int i = 1; i <= groupCount; ++i) {
        GroupConfig group;
        const std::wstring key = L"Group" + std::to_wstring(i);
        GetPrivateProfileStringW(L"Groups", (key + L"_Id").c_str(), L"", buf, 256, ini_path.c_str()); group.id = buf;
        GetPrivateProfileStringW(L"Groups", (key + L"_Name").c_str(), L"", buf, 256, ini_path.c_str()); group.name = buf;
        group.enabled = GetPrivateProfileIntW(L"Groups", (key + L"_Enabled").c_str(), 1, ini_path.c_str()) != 0;
        group.refresh_interval_ms = GetPrivateProfileIntW(L"Groups", (key + L"_RefreshMs").c_str(), 0, ini_path.c_str());
        group.precision = GetPrivateProfileIntW(L"Groups", (key + L"_Precision").c_str(), -1, ini_path.c_str());
        group.alarm_enabled = GetPrivateProfileIntW(L"Groups", (key + L"_AlarmEnabled").c_str(), 0, ini_path.c_str()) != 0;
        GetPrivateProfileStringW(L"Groups", (key + L"_PowerLimit").c_str(), L"0", buf, 256, ini_path.c_str()); group.power_limit = _wtof(buf);
        if (!group.id.empty()) m_config.groups.push_back(group);
    }
    EnsureConfigDefaults(m_config);
    const int alarmStateCount = GetPrivateProfileIntW(L"AlarmState", L"Count", 0, ini_path.c_str());
    for (int i = 1; i <= alarmStateCount; ++i) {
        const std::wstring key = L"State" + std::to_wstring(i);
        std::wstring stateKey;
        GetPrivateProfileStringW(L"AlarmState", (key + L"_Key").c_str(), L"", buf, 256, ini_path.c_str()); stateKey = buf;
        if (stateKey.empty()) continue;
        AlarmRuntime state;
        state.active = GetPrivateProfileIntW(L"AlarmState", (key + L"_Active").c_str(), 0, ini_path.c_str()) != 0;
        GetPrivateProfileStringW(L"AlarmState", (key + L"_Last").c_str(), L"0", buf, 256, ini_path.c_str()); state.last_notice = (std::time_t)_wtoi64(buf);
        m_alarm_runtime[stateKey] = state;
    }
}

void CXiaomiIoTPlugin::SaveConfigToFile()
{
    std::wstring ini_path = GetConfigFilePath();
    PluginConfig config;
    std::unordered_map<std::wstring, AlarmRuntime> alarmStates;
    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        config = m_config;
        alarmStates = m_alarm_runtime;
    }

    WritePrivateProfileStringW(L"Plugs", L"Count", std::to_wstring(config.devices.size()).c_str(), ini_path.c_str());
    for (size_t i = 0; i < config.devices.size(); ++i)
    {
        std::wstring prefixKey = L"Plug" + std::to_wstring(i + 1);
        const auto& dev = config.devices[i];

        WritePrivateProfileStringW(L"Plugs", (prefixKey + L"_IP").c_str(), dev.ip.c_str(), ini_path.c_str());
        WritePrivateProfileStringW(L"Plugs", (prefixKey + L"_Token").c_str(), dev.token.c_str(), ini_path.c_str());
        WritePrivateProfileStringW(L"Plugs", (prefixKey + L"_Label").c_str(), dev.label.c_str(), ini_path.c_str());
        WritePrivateProfileStringW(L"Plugs", (prefixKey + L"_IsChuangmi").c_str(), dev.is_chuangmi ? L"1" : L"0", ini_path.c_str());
        WritePrivateProfileStringW(L"Plugs", (prefixKey + L"_Id").c_str(), dev.id.c_str(), ini_path.c_str());
        WritePrivateProfileStringW(L"Plugs", (prefixKey + L"_Model").c_str(), dev.model.c_str(), ini_path.c_str());
        WritePrivateProfileStringW(L"Plugs", (prefixKey + L"_Group").c_str(), dev.group_id.c_str(), ini_path.c_str());
        WritePrivateProfileStringW(L"Plugs", (prefixKey + L"_Enabled").c_str(), dev.enabled ? L"1" : L"0", ini_path.c_str());
        WritePrivateProfileStringW(L"Plugs", (prefixKey + L"_RefreshMs").c_str(), std::to_wstring(dev.refresh_interval_ms).c_str(), ini_path.c_str());
        WritePrivateProfileStringW(L"Plugs", (prefixKey + L"_Precision").c_str(), std::to_wstring(dev.precision).c_str(), ini_path.c_str());
        WritePrivateProfileStringW(L"Plugs", (prefixKey + L"_AlarmEnabled").c_str(), dev.alarm_enabled ? L"1" : L"0", ini_path.c_str());
        WritePrivateProfileStringW(L"Plugs", (prefixKey + L"_PowerLimit").c_str(), std::to_wstring(dev.power_limit).c_str(), ini_path.c_str());
    }

    WritePrivateProfileStringW(L"Display", L"TotalLabel", config.total_label.c_str(), ini_path.c_str());
    WritePrivateProfileStringW(L"Display", L"TempLabel", config.temp_label.c_str(), ini_path.c_str());
    WritePrivateProfileStringW(L"Display", L"HumLabel", config.hum_label.c_str(), ini_path.c_str());

    WritePrivateProfileStringW(L"BLE", L"MAC", config.ble_mac.c_str(), ini_path.c_str());

    WritePrivateProfileStringW(L"Preferences", L"RefreshIntervalMs", std::to_wstring(config.refresh_interval_ms).c_str(), ini_path.c_str());
    WritePrivateProfileStringW(L"Preferences", L"Precision", std::to_wstring(config.precision).c_str(), ini_path.c_str());
    WritePrivateProfileStringW(L"Preferences", L"ThermometerRefreshIntervalMs", std::to_wstring(config.thermometer_refresh_interval_ms).c_str(), ini_path.c_str());
    WritePrivateProfileStringW(L"Preferences", L"ThermometerPrecision", std::to_wstring(config.thermometer_precision).c_str(), ini_path.c_str());
    WritePrivateProfileStringW(L"Preferences", L"ElectricityPrice", std::to_wstring(config.electricity_price).c_str(), ini_path.c_str());

    WritePrivateProfileStringW(L"Alarm", L"PowerAlarmEnabled", config.alarm_power_enabled ? L"1" : L"0", ini_path.c_str());
    WritePrivateProfileStringW(L"Alarm", L"PowerLimit", std::to_wstring(config.alarm_power_limit).c_str(), ini_path.c_str());
    WritePrivateProfileStringW(L"Alarm", L"CooldownMinutes", std::to_wstring(config.alarm_cooldown_minutes).c_str(), ini_path.c_str());
    WritePrivateProfileStringW(L"Alarm", L"OfflineFailures", std::to_wstring(config.offline_failures).c_str(), ini_path.c_str());
    WritePrivateProfileStringW(L"Alarm", L"TempEnabled", config.temp_alarm_enabled ? L"1" : L"0", ini_path.c_str());
    WritePrivateProfileStringW(L"Alarm", L"TempLow", std::to_wstring(config.temp_low).c_str(), ini_path.c_str());
    WritePrivateProfileStringW(L"Alarm", L"TempHigh", std::to_wstring(config.temp_high).c_str(), ini_path.c_str());
    WritePrivateProfileStringW(L"Alarm", L"HumidityEnabled", config.humidity_alarm_enabled ? L"1" : L"0", ini_path.c_str());
    WritePrivateProfileStringW(L"Alarm", L"HumidityLow", std::to_wstring(config.humidity_low).c_str(), ini_path.c_str());
    WritePrivateProfileStringW(L"Alarm", L"HumidityHigh", std::to_wstring(config.humidity_high).c_str(), ini_path.c_str());
    WritePrivateProfileStringW(L"Window", L"X", std::to_wstring(config.window_x).c_str(), ini_path.c_str());
    WritePrivateProfileStringW(L"Window", L"Y", std::to_wstring(config.window_y).c_str(), ini_path.c_str());
    WritePrivateProfileStringW(L"Window", L"Width", std::to_wstring(config.window_width).c_str(), ini_path.c_str());
    WritePrivateProfileStringW(L"Window", L"Height", std::to_wstring(config.window_height).c_str(), ini_path.c_str());
    WritePrivateProfileStringW(L"Groups", L"Count", std::to_wstring(config.groups.size()).c_str(), ini_path.c_str());
    for (size_t i = 0; i < config.groups.size(); ++i) {
        const GroupConfig& group = config.groups[i];
        const std::wstring key = L"Group" + std::to_wstring(i + 1);
        WritePrivateProfileStringW(L"Groups", (key + L"_Id").c_str(), group.id.c_str(), ini_path.c_str());
        WritePrivateProfileStringW(L"Groups", (key + L"_Name").c_str(), group.name.c_str(), ini_path.c_str());
        WritePrivateProfileStringW(L"Groups", (key + L"_Enabled").c_str(), group.enabled ? L"1" : L"0", ini_path.c_str());
        WritePrivateProfileStringW(L"Groups", (key + L"_RefreshMs").c_str(), std::to_wstring(group.refresh_interval_ms).c_str(), ini_path.c_str());
        WritePrivateProfileStringW(L"Groups", (key + L"_Precision").c_str(), std::to_wstring(group.precision).c_str(), ini_path.c_str());
        WritePrivateProfileStringW(L"Groups", (key + L"_AlarmEnabled").c_str(), group.alarm_enabled ? L"1" : L"0", ini_path.c_str());
        WritePrivateProfileStringW(L"Groups", (key + L"_PowerLimit").c_str(), std::to_wstring(group.power_limit).c_str(), ini_path.c_str());
    }
    WritePrivateProfileStringW(L"AlarmState", L"Count", std::to_wstring(alarmStates.size()).c_str(), ini_path.c_str());
    size_t alarmIndex = 0;
    for (const auto& item : alarmStates) {
        const std::wstring key = L"State" + std::to_wstring(++alarmIndex);
        WritePrivateProfileStringW(L"AlarmState", (key + L"_Key").c_str(), item.first.c_str(), ini_path.c_str());
        WritePrivateProfileStringW(L"AlarmState", (key + L"_Active").c_str(), item.second.active ? L"1" : L"0", ini_path.c_str());
        WritePrivateProfileStringW(L"AlarmState", (key + L"_Last").c_str(), std::to_wstring((long long)item.second.last_notice).c_str(), ini_path.c_str());
    }
}

// --- 动态 N 设备 Win32 GUI 选项对话框 ---
struct FullDlgData {
    HWND hBtnTab1, hBtnTab2, hBtnTab3, hBtnTab4, hBtnTab5;

    // Panel 1 Controls
    HWND hTotalLabel;
    HWND hEditGroupRules;
    HWND hEditDeviceSearch;
    HWND hBtnAddDevice;
    std::vector<HWND> device_group_boxes;
    std::vector<HWND> device_ip_edits;
    std::vector<HWND> device_token_edits;
    std::vector<HWND> device_label_edits;
    std::vector<HWND> device_on_btns;
    std::vector<HWND> device_off_btns;
    std::vector<HWND> device_fetch_btns;
    std::vector<HWND> device_del_btns;

    // Panel 2 Controls
    HWND hBleMac, hTempLabel, hHumLabel;

    // Panel 3 Controls
    HWND hCboRefresh, hCboPrecision;
    HWND hCboThermoRefresh, hCboThermoPrecision;
    HWND hEditCustomRefresh, hEditThermoCustomRefresh;
    HWND hEditPrice, hChkAlarmPower, hAlarmPowerLimit;
    HWND hEditCooldown, hEditOfflineFailures;
    HWND hChkTempAlarm, hEditTempLow, hEditTempHigh;
    HWND hChkHumidityAlarm, hEditHumidityLow, hEditHumidityHigh;

    // Panel 4 Controls
    HWND hBtnTest, hBtnScan, hEditLog, hEditCidr, hBtnImportFound;
    HWND hHistoryChart, hBtnClearHistory, hCboHistorySeries, hHistoryStats;
    std::vector<std::wstring> history_series_ids;

    std::vector<HWND> panel1_controls;
    std::vector<HWND> panel2_controls;
    std::vector<HWND> panel3_controls;
    std::vector<HWND> panel4_controls;
    std::vector<HWND> panel5_controls;

    bool saved = false;
    CXiaomiIoTPlugin* plugin = nullptr;
    PluginConfig editing_config;
};

static LRESULT CALLBACK HistoryChartWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* plugin = reinterpret_cast<CXiaomiIoTPlugin*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE)
    {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        plugin = reinterpret_cast<CXiaomiIoTPlugin*>(create->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(plugin));
        SetTimer(hWnd, 1, 5000, nullptr);
    }

    switch (msg)
    {
    case WM_TIMER:
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc{};
        GetClientRect(hWnd, &rc);
        FillRect(hdc, &rc, GetSysColorBrush(COLOR_WINDOW));

        const int left = 48;
        const int top = 18;
        const int right = 18;
        const int bottom = 30;
        RECT plot{ rc.left + left, rc.top + top, rc.right - right, rc.bottom - bottom };
        HPEN axis = CreatePen(PS_SOLID, 1, RGB(150, 150, 150));
        HGDIOBJ oldPen = SelectObject(hdc, axis);
        MoveToEx(hdc, plot.left, plot.top, nullptr);
        LineTo(hdc, plot.left, plot.bottom);
        LineTo(hdc, plot.right, plot.bottom);
        SelectObject(hdc, oldPen);
        DeleteObject(axis);

        if (plugin)
        {
            auto points = plugin->GetPowerHistorySnapshot();
            const std::time_t since = std::time(nullptr) - 24 * 60 * 60;
            points.erase(std::remove_if(points.begin(), points.end(), [&](const PowerHistoryPoint& point) { return point.timestamp < since; }), points.end());
            if (points.size() >= 2 && plot.right > plot.left && plot.bottom > plot.top)
            {
                double maxPower = 0.0;
                for (const auto& point : points) maxPower = (maxPower > point.power) ? maxPower : point.power;
                if (maxPower <= 0.0) maxPower = 1.0;

                std::vector<POINT> chart;
                chart.reserve(points.size());
                for (size_t i = 0; i < points.size(); ++i)
                {
                    const double ratioX = (points.size() == 1) ? 0.0 :
                        (double)i / (double)(points.size() - 1);
                    const double ratioY = points[i].power / maxPower;
                    chart.push_back({
                        plot.left + (int)((plot.right - plot.left) * ratioX),
                        plot.bottom - (int)((plot.bottom - plot.top) * ratioY)
                    });
                }

                HPEN line = CreatePen(PS_SOLID, 2, RGB(30, 120, 220));
                oldPen = SelectObject(hdc, line);
                Polyline(hdc, chart.data(), (int)chart.size());
                SelectObject(hdc, oldPen);
                DeleteObject(line);

                wchar_t maxText[64];
                swprintf_s(maxText, L"最大 %.1f W", maxPower);
                TextOutW(hdc, 5, plot.top - 2, maxText, (int)wcslen(maxText));
                const auto& latest = points.back();
                wchar_t latestText[64];
                swprintf_s(latestText, L"最新 %.1f W", latest.power);
                TextOutW(hdc, plot.left, plot.bottom + 8, latestText, (int)wcslen(latestText));
            }
            else
            {
                const wchar_t* empty = L"正在收集历史数据，至少需要两个采样点。";
                TextOutW(hdc, plot.left, plot.top + 30, empty, (int)wcslen(empty));
            }
        }

        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_NCDESTROY:
        KillTimer(hWnd, 1);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void SwitchTabPanels(FullDlgData* pData, int tabIndex)
{
    for (HWND h : pData->panel1_controls) ShowWindow(h, (tabIndex == 0) ? SW_SHOW : SW_HIDE);
    for (HWND h : pData->panel2_controls) ShowWindow(h, (tabIndex == 1) ? SW_SHOW : SW_HIDE);
    for (HWND h : pData->panel3_controls) ShowWindow(h, (tabIndex == 2) ? SW_SHOW : SW_HIDE);
    for (HWND h : pData->panel4_controls) ShowWindow(h, (tabIndex == 3) ? SW_SHOW : SW_HIDE);
    for (HWND h : pData->panel5_controls) ShowWindow(h, (tabIndex == 4) ? SW_SHOW : SW_HIDE);
}

static LRESULT CALLBACK FullOptionsWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    FullDlgData* pData = (FullDlgData*)GetWindowLongPtr(hWnd, GWLP_USERDATA);

    switch (msg)
    {
    case WM_CTLCOLORSTATIC:
    {
        HDC hdcStatic = (HDC)wParam;
        SetBkMode(hdcStatic, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }
    case WM_CREATE:
    {
        CREATESTRUCT* cs = (CREATESTRUCT*)lParam;
        pData = (FullDlgData*)cs->lpCreateParams;
        SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)pData);

        float scale = GetDPIScale(hWnd);
        #define S(val) (int)((val) * scale)

        int fontHeight = -S(13);
        HFONT hFont = CreateFontW(fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

        pData->editing_config = pData->plugin->GetConfig();

        // 1. 顶部 Segmented PushTab Header Buttons
        int tabWidth = S(116);
        int tabHeight = S(32);
        int startX = S(25);
        int gap = S(4);

        pData->hBtnTab1 = CreateWindowW(L"BUTTON", L"🔌 智能插座", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | BS_PUSHLIKE, startX, S(15), tabWidth, tabHeight, hWnd, (HMENU)201, NULL, NULL);
        SendMessage(pData->hBtnTab1, WM_SETFONT, (WPARAM)hFont, TRUE);

        pData->hBtnTab2 = CreateWindowW(L"BUTTON", L"🌡️ 蓝牙温湿度", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | BS_PUSHLIKE, startX + (tabWidth + gap), S(15), tabWidth, tabHeight, hWnd, (HMENU)202, NULL, NULL);
        SendMessage(pData->hBtnTab2, WM_SETFONT, (WPARAM)hFont, TRUE);

        pData->hBtnTab3 = CreateWindowW(L"BUTTON", L"🔔 告警与电费", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | BS_PUSHLIKE, startX + (tabWidth + gap) * 2, S(15), tabWidth, tabHeight, hWnd, (HMENU)203, NULL, NULL);
        SendMessage(pData->hBtnTab3, WM_SETFONT, (WPARAM)hFont, TRUE);

        pData->hBtnTab4 = CreateWindowW(L"BUTTON", L"🧪 通信与全网发现", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | BS_PUSHLIKE, startX + (tabWidth + gap) * 3, S(15), tabWidth, tabHeight, hWnd, (HMENU)204, NULL, NULL);
        SendMessage(pData->hBtnTab4, WM_SETFONT, (WPARAM)hFont, TRUE);

        pData->hBtnTab5 = CreateWindowW(L"BUTTON", L"📈 历史曲线", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | BS_PUSHLIKE, startX + (tabWidth + gap) * 4, S(15), tabWidth, tabHeight, hWnd, (HMENU)205, NULL, NULL);
        SendMessage(pData->hBtnTab5, WM_SETFONT, (WPARAM)hFont, TRUE);

        SendMessage(pData->hBtnTab1, BM_SETCHECK, BST_CHECKED, 0);

        // --- Panel 1: N 个插座动态生成控件 ---
        int y = S(60);
        HWND deviceSearchLabel = CreateWindowW(L"STATIC", L"搜索设备:", WS_CHILD | SS_LEFT, S(25), y, S(70), S(22), hWnd, NULL, NULL, NULL);
        pData->hEditDeviceSearch = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, S(95), y, S(160), S(24), hWnd, (HMENU)130, NULL, NULL);
        HWND findButton = CreateWindowW(L"BUTTON", L"查找", WS_CHILD | BS_PUSHBUTTON, S(260), y, S(55), S(24), hWnd, (HMENU)131, NULL, NULL);
        HWND enableAllButton = CreateWindowW(L"BUTTON", L"全部启用", WS_CHILD | BS_PUSHBUTTON, S(325), y, S(80), S(24), hWnd, (HMENU)132, NULL, NULL);
        HWND disableAllButton = CreateWindowW(L"BUTTON", L"全部禁用", WS_CHILD | BS_PUSHBUTTON, S(410), y, S(80), S(24), hWnd, (HMENU)133, NULL, NULL);
        HWND deleteDisabledButton = CreateWindowW(L"BUTTON", L"删除已禁用", WS_CHILD | BS_PUSHBUTTON, S(495), y, S(95), S(24), hWnd, (HMENU)134, NULL, NULL);
        for (HWND control : { deviceSearchLabel, pData->hEditDeviceSearch, findButton, enableAllButton, disableAllButton, deleteDisabledButton }) { SendMessage(control, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel1_controls.push_back(control); }
        y += S(35);
        for (size_t i = 0; i < pData->editing_config.devices.size(); ++i)
        {
            const auto& dev = pData->editing_config.devices[i];
            std::wstring grpTitle = dev.label + L" 配置与控制";

            HWND hGrp = CreateWindowW(L"BUTTON", grpTitle.c_str(), WS_CHILD | BS_GROUPBOX, S(25), y, S(580), S(140), hWnd, NULL, NULL, NULL);
            SendMessage(hGrp, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel1_controls.push_back(hGrp);

            y += S(22);
            HWND hIpL = CreateWindowW(L"STATIC", L"IP 地址:", WS_CHILD | SS_LEFT, S(45), y, S(55), S(22), hWnd, NULL, NULL, NULL);
            SendMessage(hIpL, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel1_controls.push_back(hIpL);

            HWND hIpEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", dev.ip.c_str(), WS_CHILD | ES_AUTOHSCROLL, S(100), y, S(110), S(24), hWnd, (HMENU)(1000 + i * 10 + 1), NULL, NULL);
            SendMessage(hIpEdit, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel1_controls.push_back(hIpEdit);

            HWND hLblL = CreateWindowW(L"STATIC", L"显示名称:", WS_CHILD | SS_LEFT, S(215), y, S(65), S(22), hWnd, NULL, NULL, NULL);
            SendMessage(hLblL, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel1_controls.push_back(hLblL);

            HWND hLblEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", dev.label.c_str(), WS_CHILD | ES_AUTOHSCROLL, S(280), y, S(120), S(24), hWnd, (HMENU)(1000 + i * 10 + 2), NULL, NULL);
            SendMessage(hLblEdit, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel1_controls.push_back(hLblEdit);

            HWND hBtnFetch = CreateWindowW(L"BUTTON", L"🔄获取型号", WS_CHILD | BS_PUSHBUTTON, S(405), y, S(75), S(24), hWnd, (HMENU)(1000 + i * 10 + 3), NULL, NULL);
            SendMessage(hBtnFetch, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel1_controls.push_back(hBtnFetch);

            HWND hBtnOn = CreateWindowW(L"BUTTON", L"⚡开", WS_CHILD | BS_PUSHBUTTON, S(485), y, S(40), S(24), hWnd, (HMENU)(1000 + i * 10 + 4), NULL, NULL);
            SendMessage(hBtnOn, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel1_controls.push_back(hBtnOn);

            HWND hBtnOff = CreateWindowW(L"BUTTON", L"🔌关", WS_CHILD | BS_PUSHBUTTON, S(530), y, S(40), S(24), hWnd, (HMENU)(1000 + i * 10 + 5), NULL, NULL);
            SendMessage(hBtnOff, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel1_controls.push_back(hBtnOff);

            y += S(30);
            HWND hTokL = CreateWindowW(L"STATIC", L"Token 密钥:", WS_CHILD | SS_LEFT, S(45), y, S(80), S(22), hWnd, NULL, NULL, NULL);
            SendMessage(hTokL, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel1_controls.push_back(hTokL);

            HWND hTokEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", dev.token.c_str(), WS_CHILD | ES_AUTOHSCROLL, S(130), y, S(440), S(24), hWnd, (HMENU)(1000 + i * 10 + 6), NULL, NULL);
            SendMessage(hTokEdit, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel1_controls.push_back(hTokEdit);

            y += S(30);
            HWND groupLabel = CreateWindowW(L"STATIC", L"分组:", WS_CHILD | SS_LEFT, S(45), y, S(55), S(22), hWnd, NULL, NULL, NULL);
            HWND groupEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", dev.group_id.c_str(), WS_CHILD | ES_AUTOHSCROLL, S(100), y, S(180), S(24), hWnd, (HMENU)(1000 + i * 10 + 7), NULL, NULL);
            HWND enabledCheck = CreateWindowW(L"BUTTON", L"启用设备", WS_CHILD | BS_AUTOCHECKBOX, S(300), y, S(100), S(24), hWnd, (HMENU)(1000 + i * 10 + 8), NULL, NULL);
            SendMessage(enabledCheck, BM_SETCHECK, dev.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
            HWND statusText = CreateWindowW(L"STATIC", (dev.online ? L"在线" : L"离线/待配置 Token"), WS_CHILD | SS_LEFT, S(420), y, S(150), S(22), hWnd, NULL, NULL, NULL);
            HWND moveUp = CreateWindowW(L"BUTTON", L"↑", WS_CHILD | BS_PUSHBUTTON, S(510), y, S(28), S(24), hWnd, (HMENU)(1000 + i * 10 + 9), NULL, NULL);
            HWND moveDown = CreateWindowW(L"BUTTON", L"↓", WS_CHILD | BS_PUSHBUTTON, S(542), y, S(28), S(24), hWnd, (HMENU)(1000 + i * 10), NULL, NULL);
            for (HWND control : { groupLabel, groupEdit, enabledCheck, statusText, moveUp, moveDown }) { SendMessage(control, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel1_controls.push_back(control); }

            y += S(55);
        }

        HWND h9 = CreateWindowW(L"BUTTON", L"总功率汇总项", WS_CHILD | BS_GROUPBOX, S(25), y, S(580), S(65), hWnd, NULL, NULL, NULL);
        SendMessage(h9, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel1_controls.push_back(h9);
        y += S(22);
        HWND h10 = CreateWindowW(L"STATIC", L"总功率显示标签:", WS_CHILD | SS_LEFT, S(45), y, S(120), S(22), hWnd, NULL, NULL, NULL);
        SendMessage(h10, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel1_controls.push_back(h10);
        pData->hTotalLabel = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", pData->editing_config.total_label.c_str(), WS_CHILD | ES_AUTOHSCROLL, S(175), y, S(415), S(24), hWnd, (HMENU)107, NULL, NULL);
        SendMessage(pData->hTotalLabel, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel1_controls.push_back(pData->hTotalLabel);
        y += S(35);
        HWND groupRulesLabel = CreateWindowW(L"STATIC", L"分组规则：名称|刷新ms(0继承)|精度(-1继承)|阈值W|启用，每行一个", WS_CHILD | SS_LEFT, S(45), y, S(520), S(22), hWnd, NULL, NULL, NULL);
        SendMessage(groupRulesLabel, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel1_controls.push_back(groupRulesLabel);
        std::wstringstream groupRules;
        for (const auto& group : pData->editing_config.groups) groupRules << group.id << L"|" << group.refresh_interval_ms << L"|" << group.precision << L"|" << group.power_limit << L"|" << (group.enabled ? 1 : 0) << L"\r\n";
        pData->hEditGroupRules = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", groupRules.str().c_str(), WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL, S(45), y + S(25), S(545), S(70), hWnd, (HMENU)108, NULL, NULL);
        SendMessage(pData->hEditGroupRules, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel1_controls.push_back(pData->hEditGroupRules);

        // --- Panel 2: 蓝牙温湿度计控件 ---
        y = S(60);
        HWND b1 = CreateWindowW(L"BUTTON", L"秒秒测蓝牙 BLE 温湿度计", WS_CHILD | BS_GROUPBOX, S(25), y, S(580), S(160), hWnd, NULL, NULL, NULL);
        SendMessage(b1, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel2_controls.push_back(b1);
        y += S(30);
        HWND b2 = CreateWindowW(L"STATIC", L"蓝牙 MAC 地址:", WS_CHILD | SS_LEFT, S(45), y, S(110), S(22), hWnd, NULL, NULL, NULL);
        SendMessage(b2, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel2_controls.push_back(b2);
        pData->hBleMac = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", pData->editing_config.ble_mac.c_str(), WS_CHILD | ES_AUTOHSCROLL, S(165), y, S(430), S(24), hWnd, (HMENU)108, NULL, NULL);
        SendMessage(pData->hBleMac, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel2_controls.push_back(pData->hBleMac);
        y += S(35);
        HWND b3 = CreateWindowW(L"STATIC", L"温度显示标签:", WS_CHILD | SS_LEFT, S(45), y, S(110), S(22), hWnd, NULL, NULL, NULL);
        SendMessage(b3, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel2_controls.push_back(b3);
        pData->hTempLabel = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", pData->editing_config.temp_label.c_str(), WS_CHILD | ES_AUTOHSCROLL, S(165), y, S(430), S(24), hWnd, (HMENU)109, NULL, NULL);
        SendMessage(pData->hTempLabel, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel2_controls.push_back(pData->hTempLabel);
        y += S(35);
        HWND b4 = CreateWindowW(L"STATIC", L"湿度显示标签:", WS_CHILD | SS_LEFT, S(45), y, S(110), S(22), hWnd, NULL, NULL, NULL);
        SendMessage(b4, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel2_controls.push_back(b4);
        pData->hHumLabel = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", pData->editing_config.hum_label.c_str(), WS_CHILD | ES_AUTOHSCROLL, S(165), y, S(430), S(24), hWnd, (HMENU)110, NULL, NULL);
        SendMessage(pData->hHumLabel, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel2_controls.push_back(pData->hHumLabel);

        // --- Panel 3: 告警与偏好 ---
        y = S(60);
        HWND p1 = CreateWindowW(L"BUTTON", L"刷新与数值精度设置", WS_CHILD | BS_GROUPBOX, S(25), y, S(580), S(115), hWnd, NULL, NULL, NULL);
        SendMessage(p1, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel3_controls.push_back(p1);
        SetWindowTextW(p1, L"\u63D2\u5EA7\u7C7B\u8BBE\u7F6E");
        y += S(30);
        HWND p2 = CreateWindowW(L"STATIC", L"后台轮询频率:", WS_CHILD | SS_LEFT, S(45), y, S(100), S(22), hWnd, NULL, NULL, NULL);
        SendMessage(p2, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel3_controls.push_back(p2);
        pData->hCboRefresh = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, S(155), y, S(170), S(120), hWnd, (HMENU)111, NULL, NULL);
        SendMessage(pData->hCboRefresh, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel3_controls.push_back(pData->hCboRefresh);
        SendMessageW(pData->hCboRefresh, CB_ADDSTRING, 0, (LPARAM)L"500 ms (极速刷新)");
        SendMessageW(pData->hCboRefresh, CB_ADDSTRING, 0, (LPARAM)L"1000 ms (标准刷新)");
        SendMessageW(pData->hCboRefresh, CB_ADDSTRING, 0, (LPARAM)L"2000 ms (省电模式)");
        SendMessageW(pData->hCboRefresh, CB_ADDSTRING, 0, (LPARAM)L"5000 ms (低频监控)");
        SendMessageW(pData->hCboRefresh, CB_ADDSTRING, 0, (LPARAM)L"自定义...");

        HWND customPlugLabel = CreateWindowW(L"STATIC", L"自定义(ms):", WS_CHILD | SS_LEFT, S(45), y + S(35), S(95), S(22), hWnd, NULL, NULL, NULL);
        SendMessage(customPlugLabel, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel3_controls.push_back(customPlugLabel);
        pData->hEditCustomRefresh = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"1000", WS_CHILD | ES_AUTOHSCROLL | ES_NUMBER, S(145), y + S(32), S(180), S(24), hWnd, (HMENU)118, NULL, NULL);
        SendMessage(pData->hEditCustomRefresh, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel3_controls.push_back(pData->hEditCustomRefresh);

        HWND p3 = CreateWindowW(L"STATIC", L"小数精确度:", WS_CHILD | SS_LEFT, S(345), y, S(80), S(22), hWnd, NULL, NULL, NULL);
        SendMessage(p3, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel3_controls.push_back(p3);
        pData->hCboPrecision = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, S(430), y, S(160), S(100), hWnd, (HMENU)112, NULL, NULL);
        SendMessage(pData->hCboPrecision, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel3_controls.push_back(pData->hCboPrecision);

        y += S(55);
        HWND t1 = CreateWindowW(L"BUTTON", L"\u6E29\u5EA6\u8BA1\u7C7B\u8BBE\u7F6E", WS_CHILD | BS_GROUPBOX, S(25), y, S(580), S(115), hWnd, NULL, NULL, NULL);
        SendMessage(t1, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel3_controls.push_back(t1);
        y += S(30);
        HWND t2 = CreateWindowW(L"STATIC", L"\u540E\u53F0\u8F6E\u8BE2\u9891\u7387:", WS_CHILD | SS_LEFT, S(45), y, S(100), S(22), hWnd, NULL, NULL, NULL);
        SendMessage(t2, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel3_controls.push_back(t2);
        pData->hCboThermoRefresh = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, S(155), y, S(170), S(120), hWnd, (HMENU)116, NULL, NULL);
        SendMessage(pData->hCboThermoRefresh, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel3_controls.push_back(pData->hCboThermoRefresh);
        SendMessageW(pData->hCboThermoRefresh, CB_ADDSTRING, 0, (LPARAM)L"2000 ms (\u5FEB\u901F)");
        SendMessageW(pData->hCboThermoRefresh, CB_ADDSTRING, 0, (LPARAM)L"5000 ms (\u6807\u51C6)");
        SendMessageW(pData->hCboThermoRefresh, CB_ADDSTRING, 0, (LPARAM)L"10000 ms (\u7701\u7535)");
        SendMessageW(pData->hCboThermoRefresh, CB_ADDSTRING, 0, (LPARAM)L"30000 ms (\u4F4E\u9891)");
        SendMessageW(pData->hCboThermoRefresh, CB_ADDSTRING, 0, (LPARAM)L"\u81EA\u5B9A\u4E49...");

        HWND customThermoLabel = CreateWindowW(L"STATIC", L"\u81EA\u5B9A\u4E49(ms):", WS_CHILD | SS_LEFT, S(345), y + S(35), S(95), S(22), hWnd, NULL, NULL, NULL);
        SendMessage(customThermoLabel, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel3_controls.push_back(customThermoLabel);
        pData->hEditThermoCustomRefresh = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"10000", WS_CHILD | ES_AUTOHSCROLL | ES_NUMBER, S(440), y + S(32), S(150), S(24), hWnd, (HMENU)119, NULL, NULL);
        SendMessage(pData->hEditThermoCustomRefresh, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel3_controls.push_back(pData->hEditThermoCustomRefresh);

        HWND t3 = CreateWindowW(L"STATIC", L"\u5C0F\u6570\u7CBE\u5EA6:", WS_CHILD | SS_LEFT, S(345), y, S(80), S(22), hWnd, NULL, NULL, NULL);
        SendMessage(t3, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel3_controls.push_back(t3);
        pData->hCboThermoPrecision = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, S(430), y, S(160), S(100), hWnd, (HMENU)117, NULL, NULL);
        SendMessage(pData->hCboThermoPrecision, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel3_controls.push_back(pData->hCboThermoPrecision);
        SendMessageW(pData->hCboThermoPrecision, CB_ADDSTRING, 0, (LPARAM)L"\u4FDD\u7559 1 \u4F4D\u5C0F\u6570");
        SendMessageW(pData->hCboThermoPrecision, CB_ADDSTRING, 0, (LPARAM)L"\u4FDD\u7559 2 \u4F4D\u5C0F\u6570");
        SendMessageW(pData->hCboThermoPrecision, CB_ADDSTRING, 0, (LPARAM)L"\u4EC5\u4FDD\u7559\u6574\u6570");
        y += S(85);
        SendMessageW(pData->hCboPrecision, CB_ADDSTRING, 0, (LPARAM)L"保留 1 位小数");
        SendMessageW(pData->hCboPrecision, CB_ADDSTRING, 0, (LPARAM)L"保留 2 位小数");
        SendMessageW(pData->hCboPrecision, CB_ADDSTRING, 0, (LPARAM)L"仅保留整数");

        y += S(55);
        HWND p4 = CreateWindowW(L"BUTTON", L"告警规则与电费预估", WS_CHILD | BS_GROUPBOX, S(25), y, S(580), S(205), hWnd, NULL, NULL, NULL);
        SendMessage(p4, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel3_controls.push_back(p4);
        y += S(30);
        pData->hChkAlarmPower = CreateWindowW(L"BUTTON", L"启用高功率过载报警阈值", WS_CHILD | BS_AUTOCHECKBOX, S(45), y, S(200), S(22), hWnd, (HMENU)113, NULL, NULL);
        SendMessage(pData->hChkAlarmPower, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel3_controls.push_back(pData->hChkAlarmPower);
        HWND p5 = CreateWindowW(L"STATIC", L"报警功率 (W):", WS_CHILD | SS_LEFT, S(260), y, S(100), S(22), hWnd, NULL, NULL, NULL);
        SendMessage(p5, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel3_controls.push_back(p5);
        pData->hAlarmPowerLimit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"2000.0", WS_CHILD | ES_AUTOHSCROLL, S(365), y, S(225), S(24), hWnd, (HMENU)114, NULL, NULL);
        SendMessage(pData->hAlarmPowerLimit, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel3_controls.push_back(pData->hAlarmPowerLimit);

        y += S(35);
        HWND p6 = CreateWindowW(L"STATIC", L"电费单价 (元/度):", WS_CHILD | SS_LEFT, S(45), y, S(120), S(22), hWnd, NULL, NULL, NULL);
        SendMessage(p6, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel3_controls.push_back(p6);
        pData->hEditPrice = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0.6", WS_CHILD | ES_AUTOHSCROLL, S(175), y, S(415), S(24), hWnd, (HMENU)115, NULL, NULL);
        SendMessage(pData->hEditPrice, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel3_controls.push_back(pData->hEditPrice);

        y += S(35);
        HWND cooldownLabel = CreateWindowW(L"STATIC", L"告警冷却时间（分钟）:", WS_CHILD | SS_LEFT, S(45), y, S(145), S(22), hWnd, NULL, NULL, NULL);
        pData->hEditCooldown = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"10", WS_CHILD | ES_AUTOHSCROLL | ES_NUMBER, S(195), y, S(75), S(24), hWnd, (HMENU)116, NULL, NULL);
        HWND offlineLabel = CreateWindowW(L"STATIC", L"判定离线的连续失败次数:", WS_CHILD | SS_LEFT, S(310), y, S(165), S(22), hWnd, NULL, NULL, NULL);
        pData->hEditOfflineFailures = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"3", WS_CHILD | ES_AUTOHSCROLL | ES_NUMBER, S(480), y, S(70), S(24), hWnd, (HMENU)117, NULL, NULL);
        for (HWND control : { cooldownLabel, pData->hEditCooldown, offlineLabel, pData->hEditOfflineFailures }) { SendMessage(control, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel3_controls.push_back(control); }

        y += S(35);
        pData->hChkTempAlarm = CreateWindowW(L"BUTTON", L"温度报警", WS_CHILD | BS_AUTOCHECKBOX, S(45), y, S(90), S(22), hWnd, (HMENU)120, NULL, NULL);
        HWND tempLowLabel = CreateWindowW(L"STATIC", L"最低（℃）:", WS_CHILD | SS_LEFT, S(150), y, S(70), S(22), hWnd, NULL, NULL, NULL);
        pData->hEditTempLow = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0", WS_CHILD | ES_AUTOHSCROLL, S(220), y, S(85), S(24), hWnd, (HMENU)121, NULL, NULL);
        HWND tempHighLabel = CreateWindowW(L"STATIC", L"最高（℃）:", WS_CHILD | SS_LEFT, S(320), y, S(70), S(22), hWnd, NULL, NULL, NULL);
        pData->hEditTempHigh = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"40", WS_CHILD | ES_AUTOHSCROLL, S(390), y, S(85), S(24), hWnd, (HMENU)122, NULL, NULL);

        y += S(35);
        pData->hChkHumidityAlarm = CreateWindowW(L"BUTTON", L"湿度报警", WS_CHILD | BS_AUTOCHECKBOX, S(45), y, S(90), S(22), hWnd, (HMENU)123, NULL, NULL);
        HWND humLowLabel = CreateWindowW(L"STATIC", L"最低（%）:", WS_CHILD | SS_LEFT, S(150), y, S(70), S(22), hWnd, NULL, NULL, NULL);
        pData->hEditHumidityLow = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0", WS_CHILD | ES_AUTOHSCROLL, S(220), y, S(85), S(24), hWnd, (HMENU)124, NULL, NULL);
        HWND humHighLabel = CreateWindowW(L"STATIC", L"最高（%）:", WS_CHILD | SS_LEFT, S(320), y, S(70), S(22), hWnd, NULL, NULL, NULL);
        pData->hEditHumidityHigh = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"90", WS_CHILD | ES_AUTOHSCROLL, S(390), y, S(85), S(24), hWnd, (HMENU)125, NULL, NULL);
        for (HWND control : { pData->hChkTempAlarm, tempLowLabel, pData->hEditTempLow, tempHighLabel, pData->hEditTempHigh, pData->hChkHumidityAlarm, humLowLabel, pData->hEditHumidityLow, humHighLabel, pData->hEditHumidityHigh }) { SendMessage(control, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel3_controls.push_back(control); }

        // --- Panel 4: 设备诊断与全网发现 ---
        y = S(60);
        pData->hBtnTest = CreateWindowW(L"BUTTON", L"⚡ 现场 UDP/BLE 单点加解密测试", WS_CHILD | BS_PUSHBUTTON, S(25), y, S(280), S(36), hWnd, (HMENU)301, NULL, NULL);
        SendMessage(pData->hBtnTest, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel4_controls.push_back(pData->hBtnTest);

        pData->hBtnScan = CreateWindowW(L"BUTTON", L"🔍 局域网 C++ 多线程极速扫描", WS_CHILD | BS_PUSHBUTTON, S(315), y, S(290), S(36), hWnd, (HMENU)303, NULL, NULL);
        SendMessage(pData->hBtnScan, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel4_controls.push_back(pData->hBtnScan);

        y += S(45);
        HWND cidrLabel = CreateWindowW(L"STATIC", L"手动网段 (CIDR):", WS_CHILD | SS_LEFT, S(25), y, S(110), S(22), hWnd, NULL, NULL, NULL);
        pData->hEditCidr = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, S(140), y, S(190), S(24), hWnd, (HMENU)304, NULL, NULL);
        pData->hBtnImportFound = CreateWindowW(L"BUTTON", L"批量导入扫描结果", WS_CHILD | BS_PUSHBUTTON, S(350), y, S(170), S(28), hWnd, (HMENU)305, NULL, NULL);
        for (HWND control : { cidrLabel, pData->hEditCidr, pData->hBtnImportFound }) { SendMessage(control, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel4_controls.push_back(control); }

        y += S(36);
        pData->hEditLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"点击上方按钮开始测试或扫描局域网全量设备。可填写 192.168.2.0/24；发现设备导入后需补充真实 Token。", WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_READONLY, S(25), y, S(580), S(245), hWnd, (HMENU)302, NULL, NULL);
        SendMessage(pData->hEditLog, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel4_controls.push_back(pData->hEditLog);

        // --- Panel 5: 历史功率曲线 ---
        y = S(60);
        HWND historyGroup = CreateWindowW(L"BUTTON", L"功率历史（默认最近 24 小时，保存 30 天）", WS_CHILD | BS_GROUPBOX, S(25), y, S(580), S(380), hWnd, NULL, NULL, NULL);
        SendMessage(historyGroup, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel5_controls.push_back(historyGroup);
        pData->hCboHistorySeries = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, S(40), y + S(25), S(250), S(160), hWnd, (HMENU)402, NULL, NULL);
        pData->hHistoryStats = CreateWindowW(L"STATIC", L"正在读取今日统计...", WS_CHILD | SS_LEFT, S(300), y + S(30), S(285), S(22), hWnd, NULL, NULL, NULL);
        for (HWND control : { pData->hCboHistorySeries, pData->hHistoryStats }) { SendMessage(control, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel5_controls.push_back(control); }
        for (const auto& series : pData->plugin->GetHistorySeries()) { SendMessageW(pData->hCboHistorySeries, CB_ADDSTRING, 0, (LPARAM)series.second.c_str()); pData->history_series_ids.push_back(series.first); }
        SendMessage(pData->hCboHistorySeries, CB_SETCURSEL, 0, 0);
        SetWindowTextW(pData->hHistoryStats, pData->plugin->GetHistoryStatisticsText().c_str());
        pData->hHistoryChart = CreateWindowExW(WS_EX_CLIENTEDGE, L"XiaomiIoTHistoryChartClass", L"", WS_CHILD | WS_VISIBLE, S(40), y + S(60), S(550), S(270), hWnd, NULL, GetModuleHandle(NULL), pData->plugin);
        SendMessage(pData->hHistoryChart, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel5_controls.push_back(pData->hHistoryChart);
        pData->hBtnClearHistory = CreateWindowW(L"BUTTON", L"清空历史数据", WS_CHILD | BS_PUSHBUTTON, S(40), y + S(335), S(120), S(30), hWnd, (HMENU)401, NULL, NULL);
        SendMessage(pData->hBtnClearHistory, WM_SETFONT, (WPARAM)hFont, TRUE); pData->panel5_controls.push_back(pData->hBtnClearHistory);

        // 底栏 Save / Reset / Cancel 按钮
        HWND hBtnReset = CreateWindowW(L"BUTTON", L"插座类默认", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, S(25), S(540), S(105), S(32), hWnd, (HMENU)501, NULL, NULL);
        SendMessage(hBtnReset, WM_SETFONT, (WPARAM)hFont, TRUE);
        HWND hBtnResetThermo = CreateWindowW(L"BUTTON", L"温湿度类默认", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, S(140), S(540), S(105), S(32), hWnd, (HMENU)504, NULL, NULL);
        HWND hBtnResetAlarm = CreateWindowW(L"BUTTON", L"报警类默认", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, S(255), S(540), S(105), S(32), hWnd, (HMENU)505, NULL, NULL);
        SendMessage(hBtnResetThermo, WM_SETFONT, (WPARAM)hFont, TRUE); SendMessage(hBtnResetAlarm, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hBtnExport = CreateWindowW(L"BUTTON", L"导出配置", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, S(370), S(540), S(100), S(32), hWnd, (HMENU)502, NULL, NULL);
        HWND hBtnImport = CreateWindowW(L"BUTTON", L"导入配置", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, S(480), S(540), S(100), S(32), hWnd, (HMENU)503, NULL, NULL);
        SendMessage(hBtnExport, WM_SETFONT, (WPARAM)hFont, TRUE); SendMessage(hBtnImport, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hBtnSave = CreateWindowW(L"BUTTON", L"保存并生效", WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, S(370), S(585), S(100), S(34), hWnd, (HMENU)IDOK, NULL, NULL);
        SendMessage(hBtnSave, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hBtnCancel = CreateWindowW(L"BUTTON", L"取消", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, S(480), S(585), S(100), S(34), hWnd, (HMENU)IDCANCEL, NULL, NULL);
        SendMessage(hBtnCancel, WM_SETFONT, (WPARAM)hFont, TRUE);

        SendMessage(pData->hCboRefresh, CB_SETCURSEL, (pData->editing_config.refresh_interval_ms == 500) ? 0 : (pData->editing_config.refresh_interval_ms == 2000) ? 2 : (pData->editing_config.refresh_interval_ms == 5000) ? 3 : 1, 0);
        SendMessage(pData->hCboPrecision, CB_SETCURSEL, (pData->editing_config.precision == 1) ? 0 : (pData->editing_config.precision == 0) ? 2 : 1, 0);
        const int plugRefresh = pData->editing_config.refresh_interval_ms;
        SendMessage(pData->hCboRefresh, CB_SETCURSEL,
            (plugRefresh == 500) ? 0 : (plugRefresh == 1000) ? 1 :
            (plugRefresh == 2000) ? 2 : (plugRefresh == 5000) ? 3 : 4, 0);
        const int thermoRefresh = pData->editing_config.thermometer_refresh_interval_ms;
        SendMessage(pData->hCboThermoRefresh, CB_SETCURSEL,
            (thermoRefresh == 2000) ? 0 : (thermoRefresh == 5000) ? 1 :
            (thermoRefresh == 10000) ? 2 : (thermoRefresh == 30000) ? 3 : 4, 0);
        wchar_t customPlugBuf[32];
        swprintf_s(customPlugBuf, L"%d", plugRefresh);
        SetWindowTextW(pData->hEditCustomRefresh, customPlugBuf);
        wchar_t customThermoBuf[32];
        swprintf_s(customThermoBuf, L"%d", thermoRefresh);
        SetWindowTextW(pData->hEditThermoCustomRefresh, customThermoBuf);
        SendMessage(pData->hCboThermoPrecision, CB_SETCURSEL,
            (pData->editing_config.thermometer_precision == 1) ? 0 :
            (pData->editing_config.thermometer_precision == 0) ? 2 : 1, 0);

        wchar_t priceBuf[64]; swprintf_s(priceBuf, L"%.2f", pData->editing_config.electricity_price);
        SetWindowTextW(pData->hEditPrice, priceBuf);

        SendMessage(pData->hChkAlarmPower, BM_SETCHECK, pData->editing_config.alarm_power_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        wchar_t limitBuf[64]; swprintf_s(limitBuf, L"%.1f", pData->editing_config.alarm_power_limit);
        SetWindowTextW(pData->hAlarmPowerLimit, limitBuf);
        SetWindowTextW(pData->hEditCooldown, std::to_wstring(pData->editing_config.alarm_cooldown_minutes).c_str());
        SetWindowTextW(pData->hEditOfflineFailures, std::to_wstring(pData->editing_config.offline_failures).c_str());
        SendMessage(pData->hChkTempAlarm, BM_SETCHECK, pData->editing_config.temp_alarm_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessage(pData->hChkHumidityAlarm, BM_SETCHECK, pData->editing_config.humidity_alarm_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        SetWindowTextW(pData->hEditTempLow, std::to_wstring(pData->editing_config.temp_low).c_str());
        SetWindowTextW(pData->hEditTempHigh, std::to_wstring(pData->editing_config.temp_high).c_str());
        SetWindowTextW(pData->hEditHumidityLow, std::to_wstring(pData->editing_config.humidity_low).c_str());
        SetWindowTextW(pData->hEditHumidityHigh, std::to_wstring(pData->editing_config.humidity_high).c_str());

        // 默认显示第一个 Panel
        SwitchTabPanels(pData, 0);

        #undef S
        break;
    }
    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        if (id == 118 && HIWORD(wParam) == EN_CHANGE)
        {
            SendMessage(pData->hCboRefresh, CB_SETCURSEL, 4, 0);
        }
        else if (id == 119 && HIWORD(wParam) == EN_CHANGE)
        {
            SendMessage(pData->hCboThermoRefresh, CB_SETCURSEL, 4, 0);
        }
        else if (id >= 201 && id <= 205)
        {
            SwitchTabPanels(pData, id - 201);
        }
        else if (id == 402 && HIWORD(wParam) == CBN_SELCHANGE)
        {
            int selection = (int)SendMessage(pData->hCboHistorySeries, CB_GETCURSEL, 0, 0);
            if (selection >= 0 && (size_t)selection < pData->history_series_ids.size()) {
                pData->plugin->SetHistorySeries(pData->history_series_ids[selection]);
                SetWindowTextW(pData->hHistoryStats, pData->plugin->GetHistoryStatisticsText().c_str());
                InvalidateRect(pData->hHistoryChart, nullptr, TRUE);
            }
        }
        else if (id == 131)
        {
            wchar_t query[256]{}; GetWindowTextW(pData->hEditDeviceSearch, query, 256);
            std::wstring needle(query); std::transform(needle.begin(), needle.end(), needle.begin(), towlower);
            bool found = false;
            for (size_t i = 0; i < pData->editing_config.devices.size(); ++i) {
                std::wstring haystack = pData->editing_config.devices[i].label + L" " + pData->editing_config.devices[i].ip + L" " + pData->editing_config.devices[i].group_id;
                std::transform(haystack.begin(), haystack.end(), haystack.begin(), towlower);
                if (!needle.empty() && haystack.find(needle) != std::wstring::npos) {
                    SetFocus(GetDlgItem(hWnd, (int)(1000 + i * 10 + 2)));
                    MessageBoxW(hWnd, (L"已定位：" + pData->editing_config.devices[i].label).c_str(), L"设备搜索", MB_ICONINFORMATION);
                    found = true; break;
                }
            }
            if (!found) MessageBoxW(hWnd, L"未找到匹配设备。", L"设备搜索", MB_ICONINFORMATION);
        }
        else if (id == 132 || id == 133)
        {
            const LRESULT checked = id == 132 ? BST_CHECKED : BST_UNCHECKED;
            for (size_t i = 0; i < pData->editing_config.devices.size(); ++i)
                SendMessage(GetDlgItem(hWnd, (int)(1000 + i * 10 + 8)), BM_SETCHECK, checked, 0);
        }
        else if (id == 134)
        {
            std::vector<PlugDeviceConfig> kept;
            for (size_t i = 0; i < pData->editing_config.devices.size(); ++i) {
                HWND enabled = GetDlgItem(hWnd, (int)(1000 + i * 10 + 8));
                if (SendMessage(enabled, BM_GETCHECK, 0, 0) == BST_CHECKED) kept.push_back(pData->editing_config.devices[i]);
            }
            if (kept.size() == pData->editing_config.devices.size()) { MessageBoxW(hWnd, L"没有已禁用的设备可删除。", L"批量删除", MB_ICONINFORMATION); }
            else if (MessageBoxW(hWnd, L"将删除所有已禁用设备，确定继续？", L"批量删除", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                pData->editing_config.devices = std::move(kept); pData->plugin->SaveConfig(pData->editing_config); pData->saved = true; DestroyWindow(hWnd);
            }
        }
        else if (id >= 1000)
        {
            size_t devIdx = (id - 1000) / 10;
            int action = (id - 1000) % 10;
            if (devIdx < pData->editing_config.devices.size())
            {
                HWND hIpEdit = GetDlgItem(hWnd, (int)(1000 + devIdx * 10 + 1));
                HWND hTokEdit = GetDlgItem(hWnd, (int)(1000 + devIdx * 10 + 6));
                HWND hLblEdit = GetDlgItem(hWnd, (int)(1000 + devIdx * 10 + 2));

                wchar_t bufIp[256] = { 0 }, bufTok[256] = { 0 }, bufLbl[256] = { 0 };
                GetWindowTextW(hIpEdit, bufIp, 256);
                GetWindowTextW(hTokEdit, bufTok, 256);
                GetWindowTextW(hLblEdit, bufLbl, 256);

                if (action == 9 || action == 0)
                {
                    const int target = action == 9 ? (int)devIdx - 1 : (int)devIdx + 1;
                    if (target >= 0 && target < (int)pData->editing_config.devices.size()) {
                        pData->editing_config.devices[devIdx].ip = bufIp;
                        pData->editing_config.devices[devIdx].token = bufTok;
                        pData->editing_config.devices[devIdx].label = bufLbl;
                        std::swap(pData->editing_config.devices[devIdx], pData->editing_config.devices[target]);
                        pData->plugin->SaveConfig(pData->editing_config);
                        pData->saved = true;
                        DestroyWindow(hWnd);
                    }
                }
                else if (action == 3) // 动态获取型号
                {
                    std::thread([hWnd, hLblEdit, bufIp, bufTok]() {
                        std::wstring fetchedModel;
                        bool ok = QueryDeviceModelNativeUDP(bufIp, bufTok, fetchedModel);
                        if (ok)
                        {
                            SetWindowTextW(hLblEdit, fetchedModel.c_str());
                            MessageBoxW(hWnd, (L"动态从硬件设备获取型号成功: " + fetchedModel).c_str(), L"获取型号成功", MB_ICONINFORMATION);
                        }
                        else
                        {
                            MessageBoxW(hWnd, L"无法通过 UDP 获取设备型号，请检查 IP 与 Token！", L"获取失败", MB_ICONERROR);
                        }
                    }).detach();
                }
                else if (action == 4 || action == 5) // 通电开 / 断电关
                {
                    bool turnOn = (action == 4);
                    bool isChuangmi = pData->editing_config.devices[devIdx].is_chuangmi;
                    std::wstring labelStr = bufLbl;

                    std::thread([hWnd, bufIp, bufTok, isChuangmi, labelStr, turnOn]() {
                        bool ok = ControlPlugPowerUDP(bufIp, bufTok, isChuangmi, turnOn);
                        std::wstring msg = labelStr + (turnOn ? L" 已通电开启！" : L" 已断电关闭！");
                        MessageBoxW(hWnd, ok ? msg.c_str() : L"发送指令失败，请检查 IP 与 Token！",
                                    L"电源控制结果", ok ? MB_ICONINFORMATION : MB_ICONERROR);
                    }).detach();
                }
            }
        }
        else if (id == 301) // 单点现场诊断
        {
            SetWindowTextW(pData->hEditLog, L"==============================================\r\n"
                                             L"[即时诊断] 正在向所有配置的 N 个设备发起 UDP 测试...\r\n"
                                             L"==============================================\r\n\r\n");

            std::vector<PlugDeviceConfig> devs = pData->editing_config.devices;
            std::thread([hWnd, pData, devs]() {
                std::wstringstream ss;
                ss << L"==============================================\r\n"
                   << L"   小米 N 插座全量 UDP 握手诊断报告\r\n"
                   << L"==============================================\r\n\r\n";

                for (size_t i = 0; i < devs.size(); ++i)
                {
                    double p = 0.0;
                    bool ok = QueryPlugNativeUDP(devs[i].ip, devs[i].token, devs[i].is_chuangmi, p);
                    ss << L"[" << (i + 1) << L"] " << devs[i].label << L" (" << devs[i].ip << L"):\r\n"
                       << (ok ? L"   [✅ 成功] 握手完成！当前实时功率: " + std::to_wstring(p) + L" W\r\n"
                              : L"   [❌ 失败] 无法连通设备或 Token 错误！\r\n") << L"\r\n";
                }

                std::wstring resultText = ss.str();
                SetWindowTextW(pData->hEditLog, resultText.c_str());
            }).detach();
        }
        else if (id == 303) // 局域网 C++ 多线程 16 线程极速扫描子网 1~254
        {
            SetWindowTextW(pData->hEditLog, L"==============================================\r\n"
                                             L"[多线程扫描] 16 线程正在并发探索局域网 1~254 所有 IP 设备...\r\n"
                                             L"==============================================\r\n\r\n");

            std::vector<std::wstring> configured_ips;
            for (const auto& dev : pData->editing_config.devices) configured_ips.push_back(dev.ip);
            wchar_t cidr[64]{};
            GetWindowTextW(pData->hEditCidr, cidr, 64);
            std::thread([pData, configured_ips, cidr = std::wstring(cidr)]() {
                std::wstring scanLog = ScanLocalMiioDevicesUDP(configured_ips, cidr);
                SetWindowTextW(pData->hEditLog, scanLog.c_str());
            }).detach();
        }
        else if (id == 305)
        {
            int length = GetWindowTextLengthW(pData->hEditLog);
            std::wstring log(length + 1, L'\0');
            GetWindowTextW(pData->hEditLog, log.data(), length + 1);
            std::vector<std::wstring> ips;
            const std::wstring marker = L"IP 地址: ";
            size_t pos = 0;
            while ((pos = log.find(marker, pos)) != std::wstring::npos) {
                pos += marker.size(); size_t end = log.find_first_of(L"\r\n", pos);
                std::wstring ip = log.substr(pos, end == std::wstring::npos ? std::wstring::npos : end - pos);
                if (!ip.empty() && std::find(ips.begin(), ips.end(), ip) == ips.end()) ips.push_back(ip);
                if (end == std::wstring::npos) break; pos = end + 1;
            }
            if (ips.empty()) { MessageBoxW(hWnd, L"没有可导入的扫描结果，请先完成扫描。", L"批量导入", MB_ICONINFORMATION); break; }
            if (MessageBoxW(hWnd, (L"将导入 " + std::to_wstring(ips.size()) + L" 台设备为“待配置 Token”状态。确定继续？").c_str(), L"批量导入", MB_YESNO | MB_ICONQUESTION) != IDYES) break;
            for (const auto& ip : ips) {
                auto found = std::find_if(pData->editing_config.devices.begin(), pData->editing_config.devices.end(), [&](const PlugDeviceConfig& d) { return d.ip == ip; });
                if (found == pData->editing_config.devices.end()) {
                    PlugDeviceConfig device;
                    device.id = MakeStableId(ip, L"plug"); device.ip = ip; device.token.clear();
                    device.label = L"发现的设备 [" + ip + L"]"; device.enabled = false; device.auto_fetched = false;
                    pData->editing_config.devices.push_back(device);
                }
            }
            MessageBoxW(hWnd, L"扫描结果已加入待保存配置。请填写真实 Token 后保存，再重新打开设置查看新设备。", L"批量导入完成", MB_ICONINFORMATION);
        }
        else if (id == 501 || id == 504 || id == 505)
        {
            const int category = id == 501 ? 0 : (id == 504 ? 1 : 2);
            const wchar_t* categoryName = category == 0 ? L"插座类" : (category == 1 ? L"温湿度类" : L"报警类");
            if (MessageBoxW(hWnd, (std::wstring(L"确定恢复 ") + categoryName + L" 的默认设置吗？").c_str(), L"恢复默认提示", MB_ICONQUESTION | MB_YESNO) == IDYES)
            {
                pData->plugin->ResetCategoryDefaults(category);
                pData->editing_config = pData->plugin->GetConfig();
                pData->saved = true;
                DestroyWindow(hWnd);
            }
        }
        else if (id == 502 || id == 503)
        {
            wchar_t path[MAX_PATH]{};
            OPENFILENAMEW dialog{}; dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = hWnd;
            dialog.lpstrFilter = L"Xiaomi IoT 配置 (*.json)\0*.json\0所有文件\0*.*\0";
            dialog.lpstrFile = path; dialog.nMaxFile = MAX_PATH;
            dialog.lpstrDefExt = L"json";
            bool selected = id == 502 ? GetSaveFileNameW(&dialog) != FALSE : GetOpenFileNameW(&dialog) != FALSE;
            if (!selected) break;
            std::wstring error;
            const bool ok = id == 502 ? pData->plugin->ExportConfig(path, error) : pData->plugin->ImportConfig(path, error);
            if (ok) {
                MessageBoxW(hWnd, id == 502 ? L"配置已导出。文件包含明文 Token，请妥善保管。" : L"配置已导入。窗口将关闭并刷新设备列表。", L"配置管理", MB_ICONINFORMATION);
                if (id == 503) { pData->saved = true; DestroyWindow(hWnd); }
            } else MessageBoxW(hWnd, error.c_str(), L"配置管理失败", MB_ICONERROR);
        }
        else if (id == 401)
        {
            if (MessageBoxW(hWnd, L"确定清空最近 24 小时的功率历史吗？", L"清空历史数据", MB_ICONQUESTION | MB_YESNO) == IDYES)
            {
                pData->plugin->ClearPowerHistory();
                if (pData->hHistoryChart) InvalidateRect(pData->hHistoryChart, nullptr, TRUE);
            }
        }
        else if (id == IDOK)
        {
            wchar_t buf[256];
            PluginConfig cfg = pData->editing_config;

            for (size_t i = 0; i < cfg.devices.size(); ++i)
            {
                HWND hIpEdit = GetDlgItem(hWnd, (int)(1000 + i * 10 + 1));
                HWND hLblEdit = GetDlgItem(hWnd, (int)(1000 + i * 10 + 2));
                HWND hTokEdit = GetDlgItem(hWnd, (int)(1000 + i * 10 + 6));
                HWND hGroupEdit = GetDlgItem(hWnd, (int)(1000 + i * 10 + 7));
                HWND hEnabled = GetDlgItem(hWnd, (int)(1000 + i * 10 + 8));

                if (hIpEdit && GetWindowTextW(hIpEdit, buf, 256)) cfg.devices[i].ip = buf;
                if (hLblEdit && GetWindowTextW(hLblEdit, buf, 256)) cfg.devices[i].label = buf;
                if (hTokEdit && GetWindowTextW(hTokEdit, buf, 256)) cfg.devices[i].token = buf;
                if (hGroupEdit && GetWindowTextW(hGroupEdit, buf, 256)) cfg.devices[i].group_id = buf;
                if (hEnabled) cfg.devices[i].enabled = SendMessage(hEnabled, BM_GETCHECK, 0, 0) == BST_CHECKED;
            }

            if (pData->hEditGroupRules) {
                int length = GetWindowTextLengthW(pData->hEditGroupRules);
                std::wstring rules(length + 1, L'\0'); GetWindowTextW(pData->hEditGroupRules, rules.data(), length + 1);
                std::wstringstream lines(rules); std::wstring line; std::vector<GroupConfig> groups;
                while (std::getline(lines, line)) {
                    if (!line.empty() && line.back() == L'\r') line.pop_back();
                    std::vector<std::wstring> parts; std::wstringstream split(line); std::wstring part;
                    while (std::getline(split, part, L'|')) parts.push_back(part);
                    if (parts.empty() || parts[0].empty()) continue;
                    GroupConfig group; group.id = parts[0]; group.name = parts[0];
                    if (parts.size() > 1) group.refresh_interval_ms = _wtoi(parts[1].c_str());
                    if (parts.size() > 2) group.precision = _wtoi(parts[2].c_str());
                    if (parts.size() > 3) group.power_limit = _wtof(parts[3].c_str());
                    if (parts.size() > 4) group.enabled = _wtoi(parts[4].c_str()) != 0;
                    groups.push_back(group);
                }
                if (!groups.empty()) cfg.groups = std::move(groups);
            }

            if (pData->hTotalLabel && GetWindowTextW(pData->hTotalLabel, buf, 256)) cfg.total_label = buf;
            if (pData->hBleMac && GetWindowTextW(pData->hBleMac, buf, 256)) cfg.ble_mac = buf;
            if (pData->hTempLabel && GetWindowTextW(pData->hTempLabel, buf, 256)) cfg.temp_label = buf;
            if (pData->hHumLabel && GetWindowTextW(pData->hHumLabel, buf, 256)) cfg.hum_label = buf;

            int refSel = (int)SendMessage(pData->hCboRefresh, CB_GETCURSEL, 0, 0);
            cfg.refresh_interval_ms = (refSel == 0) ? 500 : (refSel == 1) ? 1000 :
                (refSel == 2) ? 2000 : (refSel == 3) ? 5000 : 1000;
            if (refSel == 4 && pData->hEditCustomRefresh && GetWindowTextW(pData->hEditCustomRefresh, buf, 256))
            {
                cfg.refresh_interval_ms = _wtoi(buf);
                if (cfg.refresh_interval_ms < 500) cfg.refresh_interval_ms = 500;
                if (cfg.refresh_interval_ms > 60000) cfg.refresh_interval_ms = 60000;
            }

            int precSel = (int)SendMessage(pData->hCboPrecision, CB_GETCURSEL, 0, 0);
            cfg.precision = (precSel == 0) ? 1 : (precSel == 2) ? 0 : 2;

            int thermoRefSel = (int)SendMessage(pData->hCboThermoRefresh, CB_GETCURSEL, 0, 0);
            cfg.thermometer_refresh_interval_ms = (thermoRefSel == 0) ? 2000 :
                (thermoRefSel == 1) ? 5000 : (thermoRefSel == 2) ? 10000 :
                (thermoRefSel == 3) ? 30000 : 10000;
            if (thermoRefSel == 4 && pData->hEditThermoCustomRefresh && GetWindowTextW(pData->hEditThermoCustomRefresh, buf, 256))
            {
                cfg.thermometer_refresh_interval_ms = _wtoi(buf);
                if (cfg.thermometer_refresh_interval_ms < 1000) cfg.thermometer_refresh_interval_ms = 1000;
                if (cfg.thermometer_refresh_interval_ms > 60000) cfg.thermometer_refresh_interval_ms = 60000;
            }

            int thermoPrecSel = (int)SendMessage(pData->hCboThermoPrecision, CB_GETCURSEL, 0, 0);
            cfg.thermometer_precision = (thermoPrecSel == 0) ? 1 : (thermoPrecSel == 2) ? 0 : 2;

            if (pData->hEditPrice && GetWindowTextW(pData->hEditPrice, buf, 256)) cfg.electricity_price = _wtof(buf);

            cfg.alarm_power_enabled = (SendMessage(pData->hChkAlarmPower, BM_GETCHECK, 0, 0) == BST_CHECKED);
            if (pData->hAlarmPowerLimit && GetWindowTextW(pData->hAlarmPowerLimit, buf, 256)) cfg.alarm_power_limit = _wtof(buf);
            if (pData->hEditCooldown && GetWindowTextW(pData->hEditCooldown, buf, 256)) cfg.alarm_cooldown_minutes = _wtoi(buf);
            if (pData->hEditOfflineFailures && GetWindowTextW(pData->hEditOfflineFailures, buf, 256)) cfg.offline_failures = _wtoi(buf);
            cfg.temp_alarm_enabled = SendMessage(pData->hChkTempAlarm, BM_GETCHECK, 0, 0) == BST_CHECKED;
            cfg.humidity_alarm_enabled = SendMessage(pData->hChkHumidityAlarm, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (GetWindowTextW(pData->hEditTempLow, buf, 256)) cfg.temp_low = _wtof(buf);
            if (GetWindowTextW(pData->hEditTempHigh, buf, 256)) cfg.temp_high = _wtof(buf);
            if (GetWindowTextW(pData->hEditHumidityLow, buf, 256)) cfg.humidity_low = _wtof(buf);
            if (GetWindowTextW(pData->hEditHumidityHigh, buf, 256)) cfg.humidity_high = _wtof(buf);

            pData->plugin->SaveConfig(cfg);
            pData->saved = true;
            DestroyWindow(hWnd);
        }
        else if (id == IDCANCEL)
        {
            DestroyWindow(hWnd);
        }
        break;
    }
    case WM_CLOSE:
        DestroyWindow(hWnd);
        break;
    case WM_DESTROY:
        if (pData && pData->plugin) {
            RECT rc{}; GetWindowRect(hWnd, &rc);
            PluginConfig placement = pData->plugin->GetConfig();
            placement.window_x = rc.left; placement.window_y = rc.top;
            placement.window_width = rc.right - rc.left; placement.window_height = rc.bottom - rc.top;
            pData->plugin->SaveConfig(placement);
        }
        break;
    case WM_NCDESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

ITMPlugin::OptionReturn CXiaomiIoTPlugin::ShowOptionsDialog(void* hParent)
{
    HWND hWndParent = (HWND)hParent;

    WNDCLASSW wc{};
    wc.lpfnWndProc = FullOptionsWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = L"XiaomiIoTPluginFullOptionsClass";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);

    WNDCLASSW chartWc{};
    chartWc.lpfnWndProc = HistoryChartWndProc;
    chartWc.hInstance = GetModuleHandle(NULL);
    chartWc.lpszClassName = L"XiaomiIoTHistoryChartClass";
    chartWc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&chartWc);

    FullDlgData data;
    data.plugin = this;

    float scale = GetDPIScale(hWndParent);
    PluginConfig placement = GetConfig();
    // Controls have a fixed 650 logical-pixel layout and the window is not
    // resizable, so restoring an old physical width creates unused space.
    // Keep the saved position/height, but always calculate width for DPI.
    const bool hasSavedSize = placement.window_x != INT_MIN && placement.window_y != INT_MIN;
    int dlgWidth = (int)(650 * scale);
    int dlgHeight = hasSavedSize ? std::clamp(placement.window_height, 640, 1000) : (int)(720 * scale);

    HWND hDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        L"XiaomiIoTPluginFullOptionsClass",
        L"小米智能设备插件 - 高级选项设置与全网设备诊断",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, dlgWidth, dlgHeight,
        hWndParent, NULL, GetModuleHandle(NULL), &data
    );

    if (hDlg)
    {
        RECT rcDlg{};
        GetWindowRect(hDlg, &rcDlg);
        int w = rcDlg.right - rcDlg.left;
        int h = rcDlg.bottom - rcDlg.top;

        HMONITOR monitor = MonitorFromWindow(
            hWndParent ? hWndParent : hDlg, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (monitor && GetMonitorInfoW(monitor, &monitorInfo))
        {
            const RECT& work = monitorInfo.rcWork;
            int x = (placement.window_x == INT_MIN) ? work.left + (work.right - work.left - w) / 2 : placement.window_x;
            int y = (placement.window_y == INT_MIN) ? work.top + (work.bottom - work.top - h) / 2 : placement.window_y;
            x = (x < work.left) ? work.left : ((x > work.right - w) ? work.right - w : x);
            y = (y < work.top) ? work.top : ((y > work.bottom - h) ? work.bottom - h : y);
            SetWindowPos(hDlg, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        }

        EnableWindow(hWndParent, FALSE);
        MSG msg;
        while (GetMessageW(&msg, NULL, 0, 0))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        EnableWindow(hWndParent, TRUE);
        SetForegroundWindow(hWndParent);
    }

    UnregisterClassW(L"XiaomiIoTPluginFullOptionsClass", GetModuleHandle(NULL));
    UnregisterClassW(L"XiaomiIoTHistoryChartClass", GetModuleHandle(NULL));

    return data.saved ? OR_OPTION_CHANGED : OR_OPTION_UNCHANGED;
}

// 1. C++ 原生 WinRT 蓝牙 BLE 线程
void CXiaomiIoTPlugin::BleWorkerThread()
{
    init_apartment(apartment_type::multi_threaded);

    guid service_uuid("ebe0ccb0-7a0a-4b0c-8a1a-6ff2997da3a6");
    guid char_uuid("ebe0ccc1-7a0a-4b0c-8a1a-6ff2997da3a6");

    while (m_running)
    {
        std::wstring mac_str;
        {
            std::lock_guard<std::mutex> lock(m_data_mutex);
            mac_str = m_config.ble_mac;
        }

        uint64_t target_mac = 0;
        for (wchar_t ch : mac_str)
        {
            if (ch >= L'0' && ch <= L'9') target_mac = (target_mac << 4) | (ch - L'0');
            else if (ch >= L'a' && ch <= L'f') target_mac = (target_mac << 4) | (ch - L'a' + 10);
            else if (ch >= L'A' && ch <= L'F') target_mac = (target_mac << 4) | (ch - L'A' + 10);
        }
        if (target_mac == 0) target_mac = 0xA4C138D5C220ULL;

        bool success = false;
        try
        {
            auto device = BluetoothLEDevice::FromBluetoothAddressAsync(target_mac).get();
            if (device)
            {
                auto services_result = device.GetGattServicesForUuidAsync(service_uuid).get();
                if (services_result.Status() == GattCommunicationStatus::Success)
                {
                    auto services = services_result.Services();
                    if (services.Size() > 0)
                    {
                        auto service = services.GetAt(0);
                        auto char_result = service.GetCharacteristicsForUuidAsync(char_uuid).get();
                        if (char_result.Status() == GattCommunicationStatus::Success)
                        {
                            auto chars = char_result.Characteristics();
                            if (chars.Size() > 0)
                            {
                                auto gatt_char = chars.GetAt(0);
                                auto read_result = gatt_char.ReadValueAsync().get();
                                if (read_result.Status() == GattCommunicationStatus::Success)
                                {
                                    IBuffer buffer = read_result.Value();
                                    DataReader reader = DataReader::FromBuffer(buffer);
                                    uint8_t b1 = reader.ReadByte();
                                    uint8_t b2 = reader.ReadByte();
                                    uint8_t b3 = reader.ReadByte();

                                    int16_t temp_raw = (int16_t)(b1 | (b2 << 8));
                                    double temp = temp_raw / 100.0;
                                    double hum = b3;

                                    {
                                        std::lock_guard<std::mutex> lock(m_data_mutex);
                                        m_ble_temperature = temp;
                                        m_ble_humidity = hum;
                                        m_ble_online = true;
                                        EvaluateAlarmsLocked(m_config);
                                    }
                                    success = true;
                                }
                            }
                        }
                    }
                }
            }
        }
        catch (...)
        {
            success = false;
        }

        if (!success)
        {
            std::lock_guard<std::mutex> lock(m_data_mutex);
            m_ble_online = false;
        }

        int sleep_ms = 2000;
        if (success)
        {
            sleep_ms = GetConfig().thermometer_refresh_interval_ms;
            if (sleep_ms < 2000) sleep_ms = 2000;
        }
        for (int i = 0; i < sleep_ms / 100 && m_running; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

// 3. 100% 纯 C++ 动态 N 插座并发轮询线程 (全自动基于 IP 与硬件型号解析)
void CXiaomiIoTPlugin::PlugWorkerThread()
{
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
    std::unordered_map<std::wstring, std::chrono::steady_clock::time_point> last_poll;

    while (m_running)
    {
        PluginConfig cfg = GetConfig();
        double sum_power = 0.0;
        bool config_updated = false;
        std::unordered_map<std::wstring, double> history_samples;
        std::unordered_map<std::wstring, double> group_sums;

        for (const auto& configured : cfg.devices)
        {
            if (!configured.enabled) continue;
            auto group_cfg = std::find_if(cfg.groups.begin(), cfg.groups.end(), [&](const GroupConfig& g) { return g.id == configured.group_id; });
            if (group_cfg != cfg.groups.end() && !group_cfg->enabled) continue;
            const auto now = std::chrono::steady_clock::now();
            const int refresh_ms = EffectiveRefreshMs(cfg, configured);
            const auto previous = last_poll.find(configured.id);
            if (previous != last_poll.end() && std::chrono::duration_cast<std::chrono::milliseconds>(now - previous->second).count() < refresh_ms)
                continue;
            last_poll[configured.id] = now;
            PlugQuerySnapshot snapshot;
            bool ok = QueryPlugSnapshotUDP(configured, snapshot);

            {
                std::lock_guard<std::mutex> lock(m_data_mutex);
                PlugDeviceConfig* current = FindDeviceLocked(configured.id);
                if (current)
                {
                    current->online = ok;
                    if (ok) {
                        current->power = snapshot.power;
                        current->switch_state = snapshot.state;
                        current->switch_supported = snapshot.state_supported;
                        current->failed_polls = 0;
                        current->last_seen = std::time(nullptr);
                    } else {
                        ++current->failed_polls;
                        if (current->failed_polls < cfg.offline_failures) current->online = true;
                        current->switch_state = PlugSwitchState::Unknown;
                    }
                }
                if (ok) {
                    sum_power += snapshot.power;
                    group_sums[configured.group_id] += snapshot.power;
                    history_samples[L"device:" + configured.id] = snapshot.power;
                }
            }

            // 后台自动向在线设备获取真实硬件型号 (miIO.info)
            if (ok && !configured.auto_fetched && !IsPlaceholderToken(configured.token))
            {
                std::wstring autoModel;
                if (QueryDeviceModelNativeUDP(configured.ip, configured.token, autoModel))
                {
                    std::lock_guard<std::mutex> lock(m_data_mutex);
                    PlugDeviceConfig* current = FindDeviceLocked(configured.id);
                    if (current)
                    {
                        current->model = autoModel;
                        if (autoModel.rfind(L"cuco.", 0) == 0) current->is_chuangmi = false;
                        else if (autoModel.rfind(L"chuangmi.", 0) == 0) current->is_chuangmi = true;
                        if (current->label.empty() || current->label.find(L"插座 [") == 0)
                            current->label = autoModel + L" (" + configured.ip + L")";
                        current->auto_fetched = true;
                        config_updated = true;
                    }
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_data_mutex);
            sum_power = 0.0;
            group_sums.clear();
            for (const auto& device : m_config.devices) {
                if (!device.enabled || !device.online) continue;
                sum_power += device.power;
                group_sums[device.group_id] += device.power;
                history_samples[L"device:" + device.id] = device.power;
            }
        }

        if (config_updated) {
            SaveConfigToFile();
        }

        {
            std::lock_guard<std::mutex> lock(m_data_mutex);
            m_total_power = sum_power;
            EvaluateAlarmsLocked(cfg);
            if (false && (!cfg.alarm_power_enabled || m_total_power < cfg.alarm_power_limit))
                m_alarm_latched = false;

            // 高功率气泡告警
            if (false && cfg.alarm_power_enabled && m_total_power >= cfg.alarm_power_limit && m_app)
            {
                std::wstringstream alarmSs;
                alarmSs << L"⚠️ 告警: 当前插座总功率 (" << std::fixed << std::setprecision(1) << m_total_power
                        << L" W) 已超出设定的安全阈值 (" << cfg.alarm_power_limit << L" W)！";
                if (m_app && cfg.alarm_power_enabled && m_total_power >= cfg.alarm_power_limit && !m_alarm_latched)
                {
                    m_app->ShowNotifyMessage(alarmSs.str().c_str());
                    m_alarm_latched = true;
                }
            }
        }

        history_samples[L"total"] = sum_power;
        for (const auto& group : cfg.groups) history_samples[L"group:" + group.id] = group_sums[group.id];
        RecordPowerHistory(history_samples);

        int interval = 200;

        for (int i = 0; i < (interval / 100) && m_running; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    WSACleanup();
}

void CXiaomiIoTPlugin::NotifyAlarmLocked(const std::wstring& key, bool active, const std::wstring& text)
{
    AlarmRuntime& state = m_alarm_runtime[key];
    const std::time_t now = std::time(nullptr);
    const int cooldown = std::max(1, m_config.alarm_cooldown_minutes) * 60;
    if (active == state.active && (active ? now - state.last_notice < cooldown : true)) return;
    if (m_app) {
        const std::wstring message = active ? L"⚠️ " + text : L"✅ 已恢复正常：" + text;
        m_app->ShowNotifyMessage(message.c_str());
        state.last_notice = now;
    }
    state.active = active;
}

void CXiaomiIoTPlugin::EvaluateAlarmsLocked(const PluginConfig& cfg)
{
    if (cfg.alarm_power_enabled)
        NotifyAlarmLocked(L"total-power", m_total_power >= cfg.alarm_power_limit,
            L"总功率 " + std::to_wstring((int)std::round(m_total_power)) + L" W，阈值 " + std::to_wstring((int)std::round(cfg.alarm_power_limit)) + L" W");

    for (const auto& device : m_config.devices) {
        const bool offline = device.enabled && device.failed_polls >= cfg.offline_failures;
        NotifyAlarmLocked(L"offline:" + device.id, offline, device.label + L" 已离线");
        const double limit = EffectivePowerLimit(cfg, device);
        const bool powerEnabled = device.alarm_enabled || limit > 0.0;
        if (powerEnabled && limit > 0.0)
            NotifyAlarmLocked(L"device-power:" + device.id, device.online && device.power >= limit,
                device.label + L" 功率 " + std::to_wstring((int)std::round(device.power)) + L" W，阈值 " + std::to_wstring((int)std::round(limit)) + L" W");
    }
    if (cfg.temp_alarm_enabled && m_ble_online) {
        NotifyAlarmLocked(L"temperature-low", m_ble_temperature < cfg.temp_low,
            L"温度 " + std::to_wstring(m_ble_temperature) + L" ℃，低于 " + std::to_wstring(cfg.temp_low) + L" ℃");
        NotifyAlarmLocked(L"temperature-high", m_ble_temperature > cfg.temp_high,
            L"温度 " + std::to_wstring(m_ble_temperature) + L" ℃，高于 " + std::to_wstring(cfg.temp_high) + L" ℃");
    }
    if (cfg.humidity_alarm_enabled && m_ble_online) {
        NotifyAlarmLocked(L"humidity-low", m_ble_humidity < cfg.humidity_low,
            L"湿度 " + std::to_wstring(m_ble_humidity) + L" %，低于 " + std::to_wstring(cfg.humidity_low) + L" %");
        NotifyAlarmLocked(L"humidity-high", m_ble_humidity > cfg.humidity_high,
            L"湿度 " + std::to_wstring(m_ble_humidity) + L" %，高于 " + std::to_wstring(cfg.humidity_high) + L" %");
    }
}

void CXiaomiIoTPlugin::TogglePlugAsync(const std::wstring& device_id)
{
    PlugDeviceConfig device;
    {
        std::lock_guard<std::mutex> lock(m_data_mutex);
        const PlugDeviceConfig* found = FindDeviceLocked(device_id);
        if (!found || !found->enabled || !found->online || !found->switch_supported) {
            if (m_app) m_app->ShowNotifyMessage(L"该插座没有可用的开关状态，无法直接切换。");
            return;
        }
        device = *found;
    }
    std::thread([this, device]() {
        const bool wanted = device.switch_state != PlugSwitchState::On;
        const bool sent = ControlPlugPowerUDP(device.ip, device.token, device.is_chuangmi, wanted);
        if (m_app) {
            const std::wstring message = sent
                ? device.label + (wanted ? L" 已发送开启指令，将在下一次刷新后确认。" : L" 已发送关闭指令，将在下一次刷新后确认。")
                : device.label + L" 开关指令发送失败，请检查 IP、Token 或型号。";
            m_app->ShowNotifyMessage(message.c_str());
        }
    }).detach();
}

const PlugDeviceConfig* CXiaomiIoTPlugin::FindDeviceLocked(const std::wstring& device_id) const
{
    auto it = std::find_if(m_config.devices.begin(), m_config.devices.end(), [&](const PlugDeviceConfig& d) { return d.id == device_id; });
    return it == m_config.devices.end() ? nullptr : &*it;
}

PlugDeviceConfig* CXiaomiIoTPlugin::FindDeviceLocked(const std::wstring& device_id)
{
    auto it = std::find_if(m_config.devices.begin(), m_config.devices.end(), [&](const PlugDeviceConfig& d) { return d.id == device_id; });
    return it == m_config.devices.end() ? nullptr : &*it;
}

int CXiaomiIoTPlugin::EffectiveRefreshMs(const PluginConfig& cfg, const PlugDeviceConfig& dev) const
{
    if (dev.refresh_interval_ms > 0) return dev.refresh_interval_ms;
    auto group = std::find_if(cfg.groups.begin(), cfg.groups.end(), [&](const GroupConfig& g) { return g.id == dev.group_id; });
    return group != cfg.groups.end() && group->refresh_interval_ms > 0 ? group->refresh_interval_ms : cfg.refresh_interval_ms;
}

int CXiaomiIoTPlugin::EffectivePrecision(const PluginConfig& cfg, const PlugDeviceConfig& dev) const
{
    if (dev.precision >= 0) return dev.precision;
    auto group = std::find_if(cfg.groups.begin(), cfg.groups.end(), [&](const GroupConfig& g) { return g.id == dev.group_id; });
    return group != cfg.groups.end() && group->precision >= 0 ? group->precision : cfg.precision;
}

double CXiaomiIoTPlugin::EffectivePowerLimit(const PluginConfig& cfg, const PlugDeviceConfig& dev) const
{
    if (dev.power_limit > 0.0) return dev.power_limit;
    auto group = std::find_if(cfg.groups.begin(), cfg.groups.end(), [&](const GroupConfig& g) { return g.id == dev.group_id; });
    return group != cfg.groups.end() && group->power_limit > 0.0 ? group->power_limit : 0.0;
}

std::wstring CXiaomiIoTPlugin::GetTotalPowerLabelText()
{
    std::lock_guard<std::mutex> lock(m_data_mutex);
    return m_config.total_label;
}

std::wstring CXiaomiIoTPlugin::GetPlugLabelText(size_t index)
{
    std::lock_guard<std::mutex> lock(m_data_mutex);
    if (index < m_config.devices.size()) {
        return m_config.devices[index].label;
    }
    return L"未知插座";
}

std::wstring CXiaomiIoTPlugin::GetPlugLabelText(const std::wstring& device_id)
{
    std::lock_guard<std::mutex> lock(m_data_mutex);
    const PlugDeviceConfig* dev = FindDeviceLocked(device_id);
    return dev ? dev->label : L"未知插座";
}

std::wstring CXiaomiIoTPlugin::GetGroupLabelText(const std::wstring& group_id)
{
    std::lock_guard<std::mutex> lock(m_data_mutex);
    auto it = std::find_if(m_config.groups.begin(), m_config.groups.end(), [&](const GroupConfig& g) { return g.id == group_id; });
    return it == m_config.groups.end() ? L"分组" : it->name;
}

std::wstring CXiaomiIoTPlugin::GetTemperatureLabelText()
{
    std::lock_guard<std::mutex> lock(m_data_mutex);
    return m_config.temp_label;
}

std::wstring CXiaomiIoTPlugin::GetHumidityLabelText()
{
    std::lock_guard<std::mutex> lock(m_data_mutex);
    return m_config.hum_label;
}

std::wstring CXiaomiIoTPlugin::GetTotalPowerValueText()
{
    std::lock_guard<std::mutex> lock(m_data_mutex);
    std::wstringstream ss;
    ss << std::fixed << std::setprecision(m_config.precision) << m_total_power << L" W";
    if (m_config.alarm_power_enabled && m_total_power >= m_config.alarm_power_limit) {
        ss << L" [⚠️过载]";
    }
    return ss.str();
}

std::wstring CXiaomiIoTPlugin::GetPlugPowerValueText(size_t index)
{
    std::lock_guard<std::mutex> lock(m_data_mutex);
    if (index < m_config.devices.size()) {
        std::wstringstream ss;
        ss << std::fixed << std::setprecision(m_config.precision) << m_config.devices[index].power << L" W";
        return ss.str();
    }
    return L"-- W";
}

std::wstring CXiaomiIoTPlugin::GetPlugPowerValueText(const std::wstring& device_id)
{
    std::lock_guard<std::mutex> lock(m_data_mutex);
    const PlugDeviceConfig* dev = FindDeviceLocked(device_id);
    if (!dev || !dev->online) return L"-- W";
    std::wstringstream ss;
    ss << std::fixed << std::setprecision(EffectivePrecision(m_config, *dev)) << dev->power << L" W";
    return ss.str();
}

std::wstring CXiaomiIoTPlugin::GetPlugStatusValueText(const std::wstring& device_id)
{
    std::lock_guard<std::mutex> lock(m_data_mutex);
    const PlugDeviceConfig* dev = FindDeviceLocked(device_id);
    if (!dev) return L"未知设备";
    if (!dev->online) return L"离线  最后更新 " + FormatLocalTime(dev->last_seen);
    const wchar_t* state = !dev->switch_supported ? L"状态未知" : (dev->switch_state == PlugSwitchState::On ? L"开启" : L"关闭");
    return std::wstring(state) + L"  在线  更新 " + FormatLocalTime(dev->last_seen);
}

std::wstring CXiaomiIoTPlugin::GetGroupPowerValueText(const std::wstring& group_id)
{
    std::lock_guard<std::mutex> lock(m_data_mutex);
    double sum = 0.0;
    for (const auto& device : m_config.devices)
        if (device.group_id == group_id && device.enabled && device.online) sum += device.power;
    int precision = m_config.precision;
    auto group = std::find_if(m_config.groups.begin(), m_config.groups.end(), [&](const GroupConfig& g) { return g.id == group_id; });
    if (group != m_config.groups.end() && group->precision >= 0) precision = group->precision;
    std::wstringstream ss; ss << std::fixed << std::setprecision(precision) << sum << L" W";
    return ss.str();
}

std::wstring CXiaomiIoTPlugin::GetTemperatureValueText()
{
    std::lock_guard<std::mutex> lock(m_data_mutex);
    if (!m_ble_online) return L"-- \u2103";
    std::wstringstream ss;
    ss << std::fixed << std::setprecision(m_config.thermometer_precision) << m_ble_temperature << L" \u2103";
    return ss.str();
}

std::wstring CXiaomiIoTPlugin::GetHumidityValueText()
{
    std::lock_guard<std::mutex> lock(m_data_mutex);
    if (!m_ble_online) return L"-- %";
    std::wstringstream ss;
    ss << std::fixed << std::setprecision(m_config.thermometer_precision) << m_ble_humidity << L" %";
    return ss.str();
}

// 导出函数
extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance()
{
    return &CXiaomiIoTPlugin::Instance();
}
