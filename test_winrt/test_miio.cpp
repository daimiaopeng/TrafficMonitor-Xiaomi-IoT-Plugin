#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wincrypt.h>
#include <vector>
#include <string>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")

// MD5 Helper
std::vector<uint8_t> MD5(const std::vector<uint8_t>& data)
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

// AES-128-CBC Encryption / Decryption Helper
std::vector<uint8_t> AES_CBC(const std::vector<uint8_t>& data, const std::vector<uint8_t>& key, const std::vector<uint8_t>& iv, bool encrypt)
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
                // PKCS7 padding
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

void QueryPlugIP(const char* ipStr, const std::vector<uint8_t>& token)
{
    std::cout << "\nTesting IP: " << ipStr << " ..." << std::endl;

    std::vector<uint8_t> key = MD5(token);
    std::vector<uint8_t> key_plus_token = key;
    key_plus_token.insert(key_plus_token.end(), token.begin(), token.end());
    std::vector<uint8_t> iv = MD5(key_plus_token);

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return;

    sockaddr_in localAddr{};
    localAddr.sin_family = AF_INET;
    localAddr.sin_addr.s_addr = INADDR_ANY;
    localAddr.sin_port = 0;
    bind(sock, (sockaddr*)&localAddr, sizeof(localAddr));

    int timeout = 2000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(54321);
    inet_pton(AF_INET, ipStr, &serverAddr.sin_addr);

    // 1. Handshake Packet (32 bytes)
    std::vector<uint8_t> handshake(32, 0xFF);
    handshake[0] = 0x21; handshake[1] = 0x31;
    handshake[2] = 0x00; handshake[3] = 0x20;

    sendto(sock, (const char*)handshake.data(), (int)handshake.size(), 0, (sockaddr*)&serverAddr, sizeof(serverAddr));

    std::vector<uint8_t> resp(1024, 0);
    int respLen = recv(sock, (char*)resp.data(), (int)resp.size(), 0);
    if (respLen >= 32 && resp[0] == 0x21 && resp[1] == 0x31)
    {
        std::cout << "  Handshake SUCCESS on " << ipStr << std::endl;
        uint32_t stamp = (resp[16] << 24) | (resp[17] << 16) | (resp[18] << 8) | resp[19];
        stamp += 1;

        std::string json_str = "{\"id\":1,\"method\":\"get_properties\",\"params\":[{\"did\":\"power\",\"siid\":5,\"piid\":6}]}";
        std::vector<uint8_t> plain_payload(json_str.begin(), json_str.end());
        std::vector<uint8_t> enc_payload = AES_CBC(plain_payload, key, iv, true);

        uint16_t packet_len = (uint16_t)(32 + enc_payload.size());

        std::vector<uint8_t> header_16(16, 0);
        header_16[0] = 0x21; header_16[1] = 0x31;
        header_16[2] = (packet_len >> 8) & 0xFF; header_16[3] = packet_len & 0xFF;
        header_16[4] = 0; header_16[5] = 0; header_16[6] = 0; header_16[7] = 0;
        header_16[8] = resp[12]; header_16[9] = resp[13]; header_16[10] = resp[14]; header_16[11] = resp[15];
        header_16[12] = (stamp >> 24) & 0xFF; header_16[13] = (stamp >> 16) & 0xFF;
        header_16[14] = (stamp >> 8) & 0xFF; header_16[15] = stamp & 0xFF;

        std::vector<uint8_t> check_buf;
        check_buf.insert(check_buf.end(), header_16.begin(), header_16.end());
        check_buf.insert(check_buf.end(), token.begin(), token.end());
        check_buf.insert(check_buf.end(), enc_payload.begin(), enc_payload.end());

        std::vector<uint8_t> checksum = MD5(check_buf);

        std::vector<uint8_t> packet;
        packet.insert(packet.end(), header_16.begin(), header_16.end());
        packet.insert(packet.end(), checksum.begin(), checksum.end());
        packet.insert(packet.end(), enc_payload.begin(), enc_payload.end());

        sendto(sock, (const char*)packet.data(), (int)packet.size(), 0, (sockaddr*)&serverAddr, sizeof(serverAddr));

        respLen = recv(sock, (char*)resp.data(), (int)resp.size(), 0);
        std::cout << "  recv payload bytes: " << respLen << std::endl;
        if (respLen > 32)
        {
            std::vector<uint8_t> resp_enc(resp.begin() + 32, resp.begin() + respLen);
            std::vector<uint8_t> resp_plain = AES_CBC(resp_enc, key, iv, false);
            std::string resp_json(resp_plain.begin(), resp_plain.end());
            std::cout << "  Plug Response: " << resp_json << std::endl;
        }
    }
    else
    {
        std::cout << "  - Timeout on " << ipStr << std::endl;
    }
    closesocket(sock);
}

int main()
{
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    // Chuangmi token
    std::vector<uint8_t> token1 = {
        0xb5, 0x7c, 0x87, 0xc4, 0x03, 0x21, 0xed, 0xe5,
        0x97, 0x1e, 0x1f, 0x59, 0xd7, 0xab, 0xdb, 0xc3
    };

    QueryPlugIP("192.168.2.3", token1);
    QueryPlugIP("192.168.2.5", token1);

    WSACleanup();
    return 0;
}
