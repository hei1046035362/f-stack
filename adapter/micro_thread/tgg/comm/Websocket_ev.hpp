// #pragma once
enum WebSocketFrameType {
    ERROR_FRAME = 0xFF,
    INCOMPLETE_DATA = 0xFE,

    CLOSING_FRAME = 0x8,

    INCOMPLETE_FRAME = 0x81,

    TEXT_FRAME = 0x1,
    BINARY_FRAME = 0x2,

    PING_FRAME = 0x9,
    PONG_FRAME = 0xA
};

static const char basis_64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";


static std::string
ws_gen_accept_key(std::string& ws_key)
{
    return Encrypt::Base64Encode(Encrypt::sha1(ws_key));
}

void send_close(uint16_t reason)
{
    uint8_t fr[4] = {0x8 | 0x80, 2, 0};
    struct evbuffer *output;
    uint16_t *u16;

    if (evws->closed)
        return;
    evws->closed = true;

    u16 = (uint16_t *)&fr[2];
    *u16 = htons((int16_t)reason);
}