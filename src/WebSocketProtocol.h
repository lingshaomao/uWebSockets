/*
 * Authored by Alex Hultman, 2018-2020.
 * Intellectual property of third-party.

 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at

 *     http://www.apache.org/licenses/LICENSE-2.0

 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef UWS_WEBSOCKETPROTOCOL_H
#define UWS_WEBSOCKETPROTOCOL_H

#include <libusockets.h>

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string_view>

namespace uWS {

/* We should not overcomplicate these */
const std::string_view ERR_TOO_BIG_MESSAGE("Received too big message");
const std::string_view ERR_WEBSOCKET_TIMEOUT("WebSocket timed out from inactivity");
const std::string_view ERR_INVALID_TEXT("Received invalid UTF-8");
const std::string_view ERR_TOO_BIG_MESSAGE_INFLATION("Received too big message, or other inflation error");
const std::string_view ERR_INVALID_CLOSE_PAYLOAD("Received invalid close payload");
const std::string_view ERR_PROTOCOL("Received invalid WebSocket frame");
const std::string_view ERR_TCP_FIN("Received TCP FIN before WebSocket close frame");

enum OpCode : unsigned char {
    CONTINUATION = 0,
    TEXT = 1,
    BINARY = 2,
    CLOSE = 8,
    PING = 9,
    PONG = 10
};

enum {
    CLIENT,
    SERVER
};

// 24 bytes perfectly
template <bool isServer>
struct WebSocketState {
public:
    static const unsigned int SHORT_MESSAGE_HEADER = isServer ? 6 : 2;
    static const unsigned int MEDIUM_MESSAGE_HEADER = isServer ? 8 : 4;
    static const unsigned int LONG_MESSAGE_HEADER = isServer ? 14 : 10;

    /*
    opStack = -1：
        表示当前没有正在进行的消息处理。
        初始状态下，opStack 通常设置为 -1，表示还没有接收到任何消息片段。
        如果在处理消息时发现 opStack 为 -1，但接收到的消息不是新的消息（即操作码不是 OP_TEXT 或 OP_BINARY），则表明协议错误，需要关闭连接。
    opStack = 0：
        表示当前正在处理一个新的消息，但还没有接收到消息的所有片段。
        当接收到一个带有 FIN 标志为 false 的消息时，opStack 会被设置为 0，表示这是一个消息片段的一部分。
        在这种状态下，后续接收到的消息片段的操作码应该是 OP_CONTINUATION，表示它们属于同一个消息。
    opStack = 1：
        表示当前正在处理的消息片段已经接收到一部分，但还没有完成。
        当接收到一个带有 FIN 标志为 false 的消息片段时，opStack 会被设置为 1，表示这是一个消息片段的一部分。
        在这种状态下，后续接收到的消息片段的操作码应该是 OP_CONTINUATION，表示它们属于同一个消息。

    具体操作:
    初始化：
        在初始化时，opStack 通常设置为 -1，表示没有正在进行的消息处理。
    接收新消息：
        当接收到一个新的消息（操作码为 OP_TEXT 或 OP_BINARY）时，opStack 会被设置为 0，表示开始处理一个新的消息。
    接收消息片段：
        当接收到一个消息片段（操作码为 OP_CONTINUATION）时，opStack 应该已经是 0 或 1，表示正在处理一个未完成的消息。
        如果 FIN 标志为 false，表示还有更多的消息片段，opStack 保持不变。
        如果 FIN 标志为 true，表示这是最后一个消息片段，opStack 会被递减 1，直到恢复到 -1，表示消息处理完成。
    */
    // 16 bytes
    struct State {
        unsigned int wantsHead : 1;
        unsigned int spillLength : 4;
        signed int opStack : 2; // -1, 0, 1
        unsigned int lastFin : 1;
        /*
        wantsHead : 1：占用 1 位，表示是否需要头部信息。
        spillLength : 4：占用 4 位，表示溢出长度。
        opStack : 2：占用 2 位，表示操作栈的状态（-1, 0, 1）。
        lastFin : 1：占用 1 位，表示最后一个帧是否是结束帧
        这些位字段总共占用 8 位，即 1 个字节。
        */

        // 15 bytes
        unsigned char spill[LONG_MESSAGE_HEADER - 1]; // 13 或 9 字节
        OpCode opCode[2]; // 占用 2 字节

        State() {
            wantsHead = true;
            spillLength = 0;
            opStack = -1;
            lastFin = true;
        }

    } state;

    // 8 bytes
    unsigned int remainingBytes = 0;
    char mask[isServer ? 4 : 1];
};

namespace protocol {

template <typename T>
T bit_cast(char *c) {
    T val;
    memcpy(&val, c, sizeof(T));
    return val;
}

/* Byte swap for little-endian systems */
template <typename T>
T cond_byte_swap(T value) {
    uint32_t endian_test = 1;
    if (*((char *)&endian_test)) { // 查看内存中的最低有效字节来检查系统的字节序。endian_test被赋值为1，然后将其强制转换为char* 类型，如果第一个字节是 1，则表示系统是小端。如果是 0，则是大端。
        union {
            T i;
            uint8_t b[sizeof(T)];
        } src = { value }, dst;

        for (unsigned int i = 0; i < sizeof(value); i++) {
            dst.b[i] = src.b[sizeof(value) - 1 - i]; // 将 src 中的字节顺序反转，然后存储到 dst 中
        }

        return dst.i;
    }
    return value;
    // 如果系统是小端，且传入的 value 是 0x12345678，字节交换后的结果是 0x78563412
    // 对于大端系统，字节顺序是天然一致的，因此函数会直接返回原始值。
    // 这个模板函数适用于各种类型的数据（例如 16 位、32 位、64 位整数），甚至可以用于浮点数，取决于你如何实例化这个模板。
}

/*
位掩码 0x8080808080808080：
    这是一个64位的掩码，其中每一位的最高位（第7位）都是1，其余位都是0。具体来说，它的二进制表示是 100000001000000010000000100000001000000010000000100000001000000010000000。
    这个掩码用于检查每个字节的最高位是否为1。
按位与运算 &：
    tmp[0] & 0x8080808080808080：将 tmp[0] 中每个字节的最高位提取出来。
    tmp[1] & 0x8080808080808080：将 tmp[1] 中每个字节的最高位提取出来。
按位或运算 |：
    (tmp[0] & 0x8080808080808080) | (tmp[1] & 0x8080808080808080)：将 tmp[0] 和 tmp[1] 中每个字节的最高位合并在一起。
条件检查 == 0：
    如果合并后的结果为0，说明 tmp[0] 和 tmp[1] 中没有任何字节的最高位为1
*/

// Based on utf8_check.c by Markus Kuhn, 2005
// https://www.cl.cam.ac.uk/~mgk25/ucs/utf8_check.c
// Optimized for predominantly 7-bit content by Alex Hultman, 2016
// Licensed as Zlib, like the rest of this project
// This runs about 40% faster than simdutf with g++ -mavx
static bool isValidUtf8(unsigned char *s, size_t length)
{
    for (unsigned char *e = s + length; s != e; ) {
        if (s + 16 <= e) {
            uint64_t tmp[2];
            memcpy(tmp, s, 16);
            if (((tmp[0] & 0x8080808080808080) | (tmp[1] & 0x8080808080808080)) == 0) {
                s += 16;
                continue;
            }
        }

        while (!(*s & 0x80)) {
            if (++s == e) {
                return true;
            }
        }

        if ((s[0] & 0x60) == 0x40) {
            if (s + 1 >= e || (s[1] & 0xc0) != 0x80 || (s[0] & 0xfe) == 0xc0) {
                return false;
            }
            s += 2;
        } else if ((s[0] & 0xf0) == 0xe0) {
            if (s + 2 >= e || (s[1] & 0xc0) != 0x80 || (s[2] & 0xc0) != 0x80 ||
                    (s[0] == 0xe0 && (s[1] & 0xe0) == 0x80) || (s[0] == 0xed && (s[1] & 0xe0) == 0xa0)) {
                return false;
            }
            s += 3;
        } else if ((s[0] & 0xf8) == 0xf0) {
            if (s + 3 >= e || (s[1] & 0xc0) != 0x80 || (s[2] & 0xc0) != 0x80 || (s[3] & 0xc0) != 0x80 ||
                    (s[0] == 0xf0 && (s[1] & 0xf0) == 0x80) || (s[0] == 0xf4 && s[1] > 0x8f) || s[0] > 0xf4) {
                return false;
            }
            s += 4;
        } else {
            return false;
        }
    }
    return true;
}

struct CloseFrame {
    uint16_t code;
    char *message;
    size_t length;
};

static inline CloseFrame parseClosePayload(char *src, size_t length) {
    /* If we get no code or message, default to reporting 1005 no status code present */
    CloseFrame cf = {1005, nullptr, 0};
    if (length >= 2) {              // 如果有效载荷长度至少为 2 字节，表示存在关闭代码
        memcpy(&cf.code, src, 2);   // 使用 memcpy 将前两个字节复制到 cf.code
        cf = {cond_byte_swap<uint16_t>(cf.code), src + 2, length - 2}; // 确保字节序正确（通常是网络字节序到主机字节序的转换
        /*
        检查关闭代码是否在合法范围内：
            关闭代码必须在 1000 到 4999 之间。
            关闭代码不能在 1012 到 2999 之间。
            关闭代码不能是 1004、1005 或 1006。
        检查关闭消息是否为有效的 UTF-8 编码。
        如果关闭代码或消息不合法，返回关闭代码 1006（表示关闭帧异常）和预定义的错误消息 ERR_INVALID_CLOSE_PAYLOAD。
        */
        if (cf.code < 1000 || cf.code > 4999 || (cf.code > 1011 && cf.code < 4000) ||
            (cf.code >= 1004 && cf.code <= 1006) || !isValidUtf8((unsigned char *) cf.message, cf.length)) {
            /* Even though we got a WebSocket close frame, it in itself is abnormal */
            return {1006, (char *) ERR_INVALID_CLOSE_PAYLOAD.data(), ERR_INVALID_CLOSE_PAYLOAD.length()};
        }
    }
    return cf;
}

static inline size_t formatClosePayload(char *dst, uint16_t code, const char *message, size_t length) {
    /* We could have more strict checks here, but never append code 0 or 1005 or 1006 */
    if (code && code != 1005 && code != 1006) {
        code = cond_byte_swap<uint16_t>(code);
        memcpy(dst, &code, 2);
        /* It is invalid to pass nullptr to memcpy, even though length is 0 */
        if (message) {
            memcpy(dst + 2, message, length);
        }
        return length + 2;
    }
    return 0;
}

static inline size_t messageFrameSize(size_t messageSize) {
    if (messageSize < 126) {
        return 2 + messageSize;
    } else if (messageSize <= UINT16_MAX) {
        return 4 + messageSize;
    }
    return 10 + messageSize;
}

enum {
    SND_CONTINUATION = 1,
    SND_NO_FIN = 2,
    SND_COMPRESSED = 64
};

template <bool isServer>
static inline size_t formatMessage(char *dst, const char *src, size_t length, OpCode opCode, size_t reportedLength, bool compressed, bool fin) {
    size_t messageLength;
    size_t headerLength;
    if (reportedLength < 126) {
        headerLength = 2;
        dst[1] = (char) reportedLength;
    } else if (reportedLength <= UINT16_MAX) {
        headerLength = 4;
        dst[1] = 126;
        uint16_t tmp = cond_byte_swap<uint16_t>((uint16_t) reportedLength);
        memcpy(&dst[2], &tmp, sizeof(uint16_t));
    } else {
        headerLength = 10;
        dst[1] = 127;
        uint64_t tmp = cond_byte_swap<uint64_t>((uint64_t) reportedLength);
        memcpy(&dst[2], &tmp, sizeof(uint64_t));
    }

    dst[0] = (char) ((fin ? 128 : 0) | ((compressed && opCode) ? SND_COMPRESSED : 0) | (char) opCode);

    //printf("%d\n", (int)dst[0]);

    char mask[4];
    if (!isServer) {
        // 添加掩码（仅客户端）
        dst[1] |= 0x80; // 设置 dst[1] 的最高位为 1，表示消息内容被掩码
        uint32_t random = (uint32_t) rand();    // 生成一个随机的 4 字节掩码
        memcpy(mask, &random, 4);               // 将掩码写入消息头中
        memcpy(dst + headerLength, &random, 4);
        headerLength += 4;
    }

    messageLength = headerLength + length;
    memcpy(dst + headerLength, src, length);

    if (!isServer) {
        // 掩码处理（仅客户端）
        // overwrites up to 3 bytes outside of the given buffer!
        //WebSocketProtocol<isServer>::unmaskInplace(dst + headerLength, dst + headerLength + length, mask);

        // this is not optimal
        char *start = dst + headerLength;
        char *stop = start + length;
        int i = 0;
        while (start != stop) {
            (*start++) ^= mask[i++ % 4]; // 使用掩码对消息内容的每个字节进行异或操作
        }
    }
    return messageLength;
}

}

// essentially this is only a parser
template <const bool isServer, typename Impl>
struct WebSocketProtocol {
public:
    static const unsigned int SHORT_MESSAGE_HEADER = isServer ? 6 : 2;
    static const unsigned int MEDIUM_MESSAGE_HEADER = isServer ? 8 : 4;
    static const unsigned int LONG_MESSAGE_HEADER = isServer ? 14 : 10;

protected:
    static inline bool isFin(char *frame) {return *((unsigned char *) frame) & 128;}
    static inline unsigned char getOpCode(char *frame) {return *((unsigned char *) frame) & 15;}
    static inline unsigned char payloadLength(char *frame) {return ((unsigned char *) frame)[1] & 127;}
    static inline bool rsv23(char *frame) {return *((unsigned char *) frame) & 48;}
    static inline bool rsv1(char *frame) {return *((unsigned char *) frame) & 64;}

    template <int N> // 编译器可以在编译时确定 N 的值，并将其传播到整个函数体内。这允许编译器进行更多的优化，例如内联展开和循环展开
    static inline void UnrolledXor(char * __restrict data, char * __restrict mask) {
        if constexpr (N != 1) {
            UnrolledXor<N - 1>(data, mask);
        }
        data[N - 1] ^= mask[(N - 1) % 4];
    }

    /*
    unmaskImprecise8 函数的作用是对给定的数据进行掩码解码操作，使用一个固定的 64 位掩码。这个函数通过一次处理 8 个字节的方式来提高效率，
    适用于需要高性能处理大量数据的场景。函数的模板参数 DESTINATION 用于控制解码后的数据写入的位置。
    */
    template <int DESTINATION>
    static inline void unmaskImprecise8(char *src, uint64_t mask, unsigned int length) {
        /*unsigned int n = (length >> 3) + 1：计算需要处理的 8 字节块的数量。length >> 3 相当于 length / 8，加 1 是为了确保处理所有数据，即使 length 不是 8 的倍数。*/
        for (unsigned int n = (length >> 3) + 1; n; n--) {
            uint64_t loaded;
            memcpy(&loaded, src, 8); // 从 src 中加载 8 个字节到 loaded
            loaded ^= mask;          // 对 loaded 进行异或操作，使用 64 位掩码 mask
            memcpy(src - DESTINATION, &loaded, 8); // 将解码后的 8 字节数据写回到 src - DESTINATION 的位置
            src += 8;   // 准备处理下一个 8 字节块
        }
    }

    /* DESTINATION = 6 makes this not SIMD, DESTINATION = 4 is with SIMD but we don't want that for short messages */
    template <int DESTINATION>
    static inline void unmaskImprecise4(char *src, uint32_t mask, unsigned int length) {
        for (unsigned int n = (length >> 2) + 1; n; n--) {
            uint32_t loaded;
            memcpy(&loaded, src, 4);
            loaded ^= mask;
            memcpy(src - DESTINATION, &loaded, 4);
            src += 4;
        }
    }

    /*
    unmaskImpreciseCopyMask 函数的作用是从数据头部提取掩码，并使用该掩码对数据进行解码操作。
    这个函数通过模板参数 HEADER_SIZE 来决定如何处理不同大小的消息头。
    具体来说，它会根据 HEADER_SIZE 的值选择合适的掩码长度，并调用相应的解码函数。

    HEADER_SIZE 是一个编译时常量，表示消息头的大小。这个参数决定了如何提取掩码和调用哪个解码函数。
    */
    template <int HEADER_SIZE>
    static inline void unmaskImpreciseCopyMask(char *src, unsigned int length) {
        if constexpr (HEADER_SIZE != 6) {
            // 从 src 的前 4 个字节提取掩码，并将其复制到 mask 数组中，重复两次以形成 8 个字节的掩码
            char mask[8] = {src[-4], src[-3], src[-2], src[-1], src[-4], src[-3], src[-2], src[-1]};
            uint64_t maskInt;
            memcpy(&maskInt, mask, 8);
            unmaskImprecise8<HEADER_SIZE>(src, maskInt, length);
        } else {
            char mask[4] = {src[-4], src[-3], src[-2], src[-1]};
            uint32_t maskInt;
            memcpy(&maskInt, mask, 4);
            unmaskImprecise4<HEADER_SIZE>(src, maskInt, length);
        }
    }

    /*
    将掩码数组中的字节按指定的偏移量进行旋转。用于在解码过程中调整掩码，以便正确地对数据进行异或操作
    */
    static inline void rotateMask(unsigned int offset, char *mask) {
        char originalMask[4] = {mask[0], mask[1], mask[2], mask[3]};
        mask[(0 + offset) % 4] = originalMask[0];
        mask[(1 + offset) % 4] = originalMask[1];
        mask[(2 + offset) % 4] = originalMask[2];
        mask[(3 + offset) % 4] = originalMask[3];
    }

    /*
    在原地对数据进行解码操作，使用一个固定的 4 字节掩码。
    这个函数通过逐字节地对数据进行异或操作来实现解码，并且在原地修改数据，不使用额外的缓冲区
    */
    static inline void unmaskInplace(char *data, char *stop, char *mask) {
        while (data < stop) {
            *(data++) ^= mask[0];
            *(data++) ^= mask[1];
            *(data++) ^= mask[2];
            *(data++) ^= mask[3];
        }
    }

    /*
    Opcode: 4个比特。
    操作代码，Opcode的值决定了应该如何解析后续的数据载荷（data payload）。如果操作代码是不认识的，那么接收端应该断开连接（fail the connection）。可选的操作代码如下：
    - %x0：表示一个延续帧。当Opcode为0时，表示本次数据传输采用了数据分片，当前收到的数据帧为其中一个数据分片。
    - %x1：表示这是一个文本帧（frame）
    - %x2：表示这是一个二进制帧（frame）
    - %x3-7：保留的操作代码，用于后续定义的非控制帧。
    - %x8：表示连接断开。
    - %x9：表示这是一个ping操作。
    - %xA：表示这是一个pong操作。
    - %xB-F：保留的操作代码，用于后续定义的控制帧。
    */
   /*
    unsigned int MESSAGE_HEADER：消息头的大小。
    typename T：消息长度的类型（通常是 uint64_t 或 uint32_t）。
    T payLength：消息的有效负载长度。
    char *&src：指向消息缓冲区的指针。
    unsigned int &length：剩余未处理的消息长度。
    WebSocketState<isServer> *wState：WebSocket 状态对象的指针。
    void *user：用户数据指针，用于传递给回调函数。
   */
    template <unsigned int MESSAGE_HEADER, typename T>
    static inline bool consumeMessage(T payLength, char *&src, unsigned int &length, WebSocketState<isServer> *wState, void *user) {
        if (getOpCode(src)) {
            // 如果操作码不为0，且 opStack 已经有一个未完成的操作或者上一个消息没有结束且操作码小于 2（即不是控制帧），则关闭连接并返回 true
            if (wState->state.opStack == 1 || (!wState->state.lastFin && getOpCode(src) < 2)) {
                Impl::forceClose(wState, user, ERR_PROTOCOL);
                return true;
            }
            // 否则，将操作码压入 opStack
            wState->state.opCode[++wState->state.opStack] = (OpCode) getOpCode(src);
        } else if (wState->state.opStack == -1) {
            // 如果 opStack 为空且操作码无效，则关闭连接并返回 true
            Impl::forceClose(wState, user, ERR_PROTOCOL);
            return true;
        }
        // 更新最后一个消息的 FIN 标志
        wState->state.lastFin = isFin(src);

        if (Impl::refusePayloadLength(payLength, wState, user)) {
            // 如果消息长度超过允许的最大值，则关闭连接并返回 true
            Impl::forceClose(wState, user, ERR_TOO_BIG_MESSAGE);
            return true;
        }

        if (payLength + MESSAGE_HEADER <= length) {
            // if 分支：处理完整消息，即消息的总长度（包括消息头）不超过剩余未处理的消息长度。
            // 在这种情况下，消息可以完全处理，更新状态并返回 false

            bool fin = isFin(src);
            if (isServer) {
                /* This guy can never be assumed to be perfectly aligned since we can get multiple messages in one read */
                /* 在处理 WebSocket 消息时，数据缓冲区中的消息可能不是完美对齐的。这是因为在一个读取操作中可能会接收到多个消息，而这些消息的边界可能不会恰好与缓冲区的边界对齐。
                这种情况下，处理消息时需要特别小心，以确保正确解析和处理每个消息。*/
                // 服务器端需要对消息进行解码，因为 WebSocket 协议要求客户端发送的消息必须经过掩码处理。服务器端在接收消息后需要去除掩码，以便正确解析消息内容
                unmaskImpreciseCopyMask<MESSAGE_HEADER>(src + MESSAGE_HEADER, (unsigned int) payLength);
                // 处理解码后的消息片段
                if (Impl::handleFragment(src, payLength, 0, wState->state.opCode[wState->state.opStack], fin, wState, user)) {
                    return true;
                }
            } else {
                // 客户端不需要对消息进行解码，因为服务器发送的消息已经去除了掩码。客户端直接调用 Impl::handleFragment 函数处理消息片段
                if (Impl::handleFragment(src + MESSAGE_HEADER, payLength, 0, wState->state.opCode[wState->state.opStack], isFin(src), wState, user)) {
                    return true;
                }
            }

            if (fin) {
                // 如果消息是最后一个片段（FIN 为真），则从 opStack 中弹出操作码
                wState->state.opStack--;
            }
            // 更新 src 和 length，指向下一个未处理的消息
            src += payLength + MESSAGE_HEADER;
            length -= (unsigned int) (payLength + MESSAGE_HEADER);
            // 将 spillLength 设置为 0，表示当前消息已完全处理
            wState->state.spillLength = 0;
            // 返回 false，表示消息处理成功
            return false;

            // spillLength 是一个用于管理消息片段边界不一致情况的变量:
            // 当在一个读取操作中接收到的消息片段不足以构成一个完整的消息时，spillLength 用于记录剩余未处理的数据长度。
            // 这样可以在下一次读取操作中继续处理这些数据，确保消息的完整性和正确性
        } else {
            // else 分支：处理不完整消息，即消息的总长度（包括消息头）大于剩余未处理的消息长度。
            // 在这种情况下，消息不能完全处理，记录剩余未处理的数据长度并返回 true

            wState->state.spillLength = 0;      // 将 spillLength 设置为 0，表示当前没有未处理的数据
            wState->state.wantsHead = false;    // 设置 wantsHead 为 false，表示不再需要消息头
            wState->remainingBytes = (unsigned int) (payLength - length + MESSAGE_HEADER); // 计算剩余未处理的数据长度
            bool fin = isFin(src);
            if constexpr (isServer) {
                // 如果是服务器端，对消息进行解码（去除掩码）
                memcpy(wState->mask, src + MESSAGE_HEADER - 4, 4);
                uint64_t mask;
                memcpy(&mask, src + MESSAGE_HEADER - 4, 4);
                memcpy(((char *)&mask) + 4, src + MESSAGE_HEADER - 4, 4);
                unmaskImprecise8<0>(src + MESSAGE_HEADER, mask, length);
                rotateMask(4 - (length - MESSAGE_HEADER) % 4, wState->mask);
            }
            Impl::handleFragment(src + MESSAGE_HEADER, length - MESSAGE_HEADER, wState->remainingBytes, wState->state.opCode[wState->state.opStack], fin, wState, user);
            return true;
        }
    }

    /* This one is nicely vectorized on both ARM64 and X64 - especially with -mavx */
    // restrict 是一种用于指针的限定符，主要作用是向编译器提示该指针是访问某个内存位置的唯一途径
    // 函数通过循环遍历整个数据缓冲区，并每次处理16个字节的数据块。对于每个数据块，调用UnrolledXor模板函数来进行解掩码操作。
    // 这里假设 LIBUS_RECV_BUFFER_LENGTH 是一个预定义的宏，代表接收缓冲区的长度。
    static inline void unmaskAll(char * __restrict data, char * __restrict mask) {
        for (int i = 0; i < LIBUS_RECV_BUFFER_LENGTH; i += 16) {
            UnrolledXor<16>(data + i, mask);
        }
    }

    /*
    char *&src: 指向当前数据帧的指针。
    unsigned int &length: 当前数据帧的长度。
    WebSocketState<isServer> *wState: 包含WebSocket状态信息的结构体指针，isServer 是一个布尔模板参数，表示当前是否为服务器端。
    void *user: 用户提供的上下文数据指针。
    */
    static inline bool consumeContinuation(char *&src, unsigned int &length, WebSocketState<isServer> *wState, void *user) {
        if (wState->remainingBytes <= length) {
            // 剩余要处理的字节数小于或等于当前数据帧的长度，则继续处理
            if (isServer) {
                /*
                如果是服务器端，首先对数据进行解码。这里使用了位运算来优化解码过程：
                    n = wState->remainingBytes >> 2 计算出需要解码的完整4字节块的数量。
                    unmaskInplace 函数对这些完整块进行解码。
                    对于剩余不足4字节的部分，使用循环逐个字节解码。
                */
                unsigned int n = wState->remainingBytes >> 2; // 右移2位相当于除以4
                unmaskInplace(src, src + n * 4, wState->mask);
                for (unsigned int i = 0, s = wState->remainingBytes % 4; i < s; i++) {
                    src[n * 4 + i] ^= wState->mask[i];
                }
            }

            // 处理解码后的数据帧。如果处理失败，返回 false
            if (Impl::handleFragment(src, wState->remainingBytes, 0, wState->state.opCode[wState->state.opStack], wState->state.lastFin, wState, user)) {
                return false;
            }

            // 如果这是最后一个数据帧（lastFin 为真），则更新操作栈。然后更新指针和长度，并设置 wantsHead 标志，表示需要读取下一个头部
            if (wState->state.lastFin) {
                wState->state.opStack--;
            }

            src += wState->remainingBytes;
            length -= wState->remainingBytes;
            wState->state.wantsHead = true;
            return true;
        } else {
            // 如果当前数据帧不足以处理完所有剩余字节
            if (isServer) {
                /* No need to unmask if mask is 0 */
                uint32_t nullmask = 0;
                if (memcmp(wState->mask, &nullmask, sizeof(uint32_t))) {
                    // 创建一个 uint32_t 类型的变量 nullmask 并初始化为0。然后使用 memcmp 函数比较 wState->mask 和 nullmask。
                    // 如果掩码不是全零，则继续执行解码逻辑
                    if /*constexpr*/ (LIBUS_RECV_BUFFER_LENGTH == length) {
                        // 快速路径：如果当前数据帧的长度等于预定义的缓冲区长度 LIBUS_RECV_BUFFER_LENGTH，则调用 unmaskAll 函数对整个数据帧进行解码
                        unmaskAll(src, wState->mask);
                    } else {
                        // 慢速路径：否则，计算需要解码的完整4字节块的数量，并调用 unmaskInplace 函数进行解码。
                        // 这里 (length >> 2) + 1 计算出需要解码的完整4字节块的数量加1，确保所有数据都被解码
                        // Slow path
                        unmaskInplace(src, src + ((length >> 2) + 1) * 4, wState->mask);
                    }
                }
            }

            // 减少剩余要处理的字节数
            wState->remainingBytes -= length;
            // 处理解码后的数据帧
            if (Impl::handleFragment(src, length, wState->remainingBytes, wState->state.opCode[wState->state.opStack], wState->state.lastFin, wState, user)) {
                return false;
            }

            if (isServer && length % 4) {
                // 如果是服务器端且当前数据帧的长度不是4的倍数，则调整掩码。
                // 具体来说，计算剩余未处理的字节数，并调用 rotateMask 函数旋转掩码
                rotateMask(4 - (length % 4), wState->mask);
            }
            return false;
        }
    }

public:
    WebSocketProtocol() {

    }

    static inline void consume(char *src, unsigned int length, WebSocketState<isServer> *wState, void *user) {
        if (wState->state.spillLength) {
            src -= wState->state.spillLength;
            length += wState->state.spillLength;
            // 如果有之前未处理完的数据（存储在 wState->state.spill 中），将这些数据复制回 src，并更新 src 和 length
            memcpy(src, wState->state.spill, wState->state.spillLength);
        }
        if (wState->state.wantsHead) { // 如果当前状态需要解析新的头部，则进入解析逻辑
            parseNext:
            while (length >= SHORT_MESSAGE_HEADER) {
                // 只要当前数据帧的长度大于等于短消息头部的长度（SHORT_MESSAGE_HEADER），就继续解析

                // 验证帧头
                // invalid reserved bits / invalid opcodes / invalid control frames / set compressed frame
                if ((rsv1(src) && !Impl::setCompressed(wState, user)) || rsv23(src) || (getOpCode(src) > 2 && getOpCode(src) < 8) ||
                    getOpCode(src) > 10 || (getOpCode(src) > 2 && (!isFin(src) || payloadLength(src) > 125))) {
                    Impl::forceClose(wState, user, ERR_PROTOCOL);
                    return;
                }

                if (payloadLength(src) < 126) {
                    if (consumeMessage<SHORT_MESSAGE_HEADER, uint8_t>(payloadLength(src), src, length, wState, user)) {
                        return;
                    }
                } else if (payloadLength(src) == 126) {
                    if (length < MEDIUM_MESSAGE_HEADER) {
                        break;
                    } else if(consumeMessage<MEDIUM_MESSAGE_HEADER, uint16_t>(protocol::cond_byte_swap<uint16_t>(protocol::bit_cast<uint16_t>(src + 2)), src, length, wState, user)) {
                        return;
                    }
                } else if (length < LONG_MESSAGE_HEADER) {
                    break;
                } else if (consumeMessage<LONG_MESSAGE_HEADER, uint64_t>(protocol::cond_byte_swap<uint64_t>(protocol::bit_cast<uint64_t>(src + 2)), src, length, wState, user)) {
                    return;
                }
            }
            if (length) {
                // 如果还有剩余数据未处理，将其复制到 wState->state.spill 中，并更新 spillLength
                memcpy(wState->state.spill, src, length);
                wState->state.spillLength = length & 0xf;
                /*
                0xf 是十六进制表示的15，其二进制表示为 00001111。
                length & 0xf 是按位与运算，它将 length 的低4位与 0xf 进行按位与操作。
                    如果 length 是 20，那么 20 & 0xf 的结果是 4（因为20的二进制表示是 10100，低4位是 0100，即4）。
                    如果 length 是 10，那么 10 & 0xf 的结果是 10（因为10的二进制表示是 1010，低4位是 1010，即10）。
                通过按位与运算，确保 spillLength 的值不会超过15。
                这样可以避免 spill 缓冲区溢出，因为 spill 缓冲区的大小通常是固定的，最多只能存储15个字节。
                */
            }
        } else if (consumeContinuation(src, length, wState, user)) { // 处理连续帧            
            // 如果成功处理了一个完整的连续帧，跳转到 parseNext 继续解析新的头部
            goto parseNext;
        }
    }

    static const int CONSUME_POST_PADDING = 4;
    static const int CONSUME_PRE_PADDING = LONG_MESSAGE_HEADER - 1;
};

}

#endif // UWS_WEBSOCKETPROTOCOL_H
