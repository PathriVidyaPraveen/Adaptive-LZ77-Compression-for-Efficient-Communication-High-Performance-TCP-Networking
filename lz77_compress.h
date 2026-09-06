#ifndef LZ77_COMPRESS_H
#define LZ77_COMPRESS_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define LZ77_WINDOW_SIZE 4096u
#define LZ77_MAX_MATCH_LEN 4096u
#define Q2_HEADER_SIZE 13

typedef struct {
    uint8_t *buf;
    size_t byte_cap;
    size_t bit_len;
} bitwriter_t;

static void bitwriter_init(bitwriter_t *bw, size_t initial_bytes){
    if(initial_bytes < 16){
        initial_bytes = 16;
    }
    bw->buf = (uint8_t *)calloc(initial_bytes,1);
    bw->byte_cap = initial_bytes;
    bw->bit_len = 0;
}

static void bitwriter_ensure(bitwriter_t *bw, size_t extra_bits){
    size_t needed_bytes = (bw->bit_len + extra_bits + 7)/8;
    if(needed_bytes <= bw->byte_cap){
        return;
    }
    size_t new_cap = bw->byte_cap*2;
    if(new_cap < needed_bytes){
        new_cap = needed_bytes;
    }
    bw->buf = (uint8_t *)realloc(bw->buf, new_cap);
    memset(bw->buf + bw->byte_cap,0,new_cap - bw->byte_cap);
    bw->byte_cap = new_cap;
}

static void bitwriter_put_bit(bitwriter_t *bw, int bit){
    bitwriter_ensure(bw,1);
    size_t byte_idx = bw->bit_len/8;

    int bit_idx= 7-(int)(bw->bit_len % 8);

    if(bit){
        bw->buf[byte_idx] |= (uint8_t)(1u << bit_idx);
    }
    bw->bit_len++;
}

static void bitwriter_put_bits(bitwriter_t *bw, uint32_t value, int width){
    for(int i = width - 1;i >= 0;i--){
        bitwriter_put_bit(bw,(int)((value >> i) & 1u));
    }



}

typedef struct{
    const uint8_t *buf;
    size_t bit_len;
    size_t bit_pos;
} bitreader_t;

static int bitreader_get_bit(bitreader_t *br){
    size_t byte_idx = br->bit_pos / 8;
    int bit_idx = 7 - (int)(br->bit_pos % 8);
    int bit = (br->buf[byte_idx] >> bit_idx) & 1;
    br->bit_pos++;
    return bit;


}

static uint32_t bitreader_get_bits(bitreader_t *br, int width){
    uint32_t value = 0;

    for(int i = 0; i < width; i++){
        value = (value << 1) | (uint32_t)bitreader_get_bit(br);
    }

    return value;
}

static int bit_length_u32(uint32_t x){
    int n = 0;
    while (x){
        n++;
        x >>= 1;
    }
    return n;
}

static void encode_int_prefix(bitwriter_t *bw, uint32_t a){
    int L = bit_length_u32(a);
    int Lb = bit_length_u32((uint32_t)L);
    for(int i = 0; i < Lb - 1; i++){
        bitwriter_put_bit(bw, 0);
    }
    bitwriter_put_bit(bw,1);
    bitwriter_put_bits(bw,(uint32_t)L, Lb);
    bitwriter_put_bits(bw,a,L);
}

static uint32_t decode_int_prefix(bitreader_t *br){
    int zeros = 0;
    while(bitreader_get_bit(br) == 0){
        zeros++;
    }
    int Lb = zeros+1;
    uint32_t L = bitreader_get_bits(br, Lb);
    uint32_t a = bitreader_get_bits(br, (int)L);
    return a;

}
typedef struct {
    uint8_t *bytes;
    size_t byte_len;
    size_t bit_len;
} lz77_result_t;

static lz77_result_t lz77_encode(const uint8_t *s, size_t n){
    bitwriter_t bw;
    bitwriter_init(&bw, n);

    size_t t = 0;
    while(t < n){
        size_t max_length = 0, max_pos = 0;
        size_t window_start = (t > LZ77_WINDOW_SIZE)?(t - LZ77_WINDOW_SIZE):0;

        size_t max_possible = n-t;
        if(max_possible > LZ77_MAX_MATCH_LEN){
            max_possible = LZ77_MAX_MATCH_LEN;
        }
        for(size_t j = window_start;j < t;j++){
            size_t length = 0;
            while(length < max_possible && s[j + length] == s[t + length]){
                length++;
            }

            if(length > max_length){
                max_length = length;
                max_pos = j;
            }
        }
        if(max_length > 0){
            bitwriter_put_bit(&bw, 1);
            encode_int_prefix(&bw,(uint32_t)(t-max_pos));
            encode_int_prefix(&bw, (uint32_t)max_length);
            t += max_length;
        }else{
            bitwriter_put_bit(&bw,0);
            bitwriter_put_bits(&bw,s[t],8);
            t++;
        }
    }

    lz77_result_t result;
    result.bit_len = bw.bit_len;
    result.byte_len = (bw.bit_len + 7)/8;
    result.bytes = bw.buf;
    return result;
}

static uint8_t *lz77_decode(const uint8_t *bits_bytes, size_t bit_len, size_t expected_len){

    bitreader_t br;
    br.buf = bits_bytes;
    br.bit_len = bit_len;
    br.bit_pos = 0;
    uint8_t *out = (uint8_t *)malloc(expected_len > 0 ?expected_len:1);
    size_t out_len = 0;
    while(br.bit_pos < br.bit_len){
        int flag = bitreader_get_bit(&br);
        if(flag == 0){
            uint32_t byte_val = bitreader_get_bits(&br,8);
            out[out_len++] = (uint8_t)byte_val;
        }else{
            uint32_t offset = decode_int_prefix(&br);
            uint32_t length = decode_int_prefix(&br);
            size_t start_pos = out_len - offset;
            for(uint32_t k = 0; k < length; k++){
                out[out_len] = out[start_pos + k];
                out_len++;
            }
        }
    }
    if(out_len != expected_len){
        fprintf(stderr,"warning: lz77_decode() produced %zu bytes, expected %zu (protocol/header mismatch)\n", out_len,expected_len);
    }
    return out;
}
static int send_all(int fd, const void *buf, size_t len){
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    while(sent < len){
        ssize_t n = send(fd,p + sent,len - sent,0);
        if(n < 0){
            perror("send() failed");
            return -1;
        }
        if(n == 0){
            fprintf(stderr,"send() returned 0 unexpectedly\n");
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

static int recv_all(int fd, void *buf, size_t len){
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while(got < len){
        ssize_t n = recv(fd,p+got,len-got,0);
        if(n < 0){
            perror("recv() failed");return -1;
        }


        if(n == 0){
            if(got > 0){
                fprintf(stderr,"connection closed prematurely (expected %zu more bytes)\n",len - got);
            }
            return -1;
        }
        got += (size_t)n;
    }
    return 0;
}

typedef struct {
    size_t original_len;
    size_t compressed_len;
    int compression_used;
    size_t bytes_sent;
} q2_send_stats_t;

static int q2_send_message(int fd,const uint8_t *msg, size_t msg_len, q2_send_stats_t *stats){
    lz77_result_t enc = lz77_encode(msg, msg_len);

    int use_compressed = (enc.byte_len < msg_len)?1:0;
    size_t payload_len = use_compressed?enc.byte_len:msg_len;
    const uint8_t *payload_ptr = use_compressed ? enc.bytes : msg;
    uint8_t header[Q2_HEADER_SIZE];
    header[0] = (uint8_t)use_compressed;
    uint32_t orig_n = htonl((uint32_t)msg_len);
    uint32_t pay_n = htonl((uint32_t)payload_len);
    uint32_t bits_n = htonl((uint32_t)(use_compressed ? enc.bit_len : 0));

    memcpy(header+1,&orig_n,4);
    memcpy(header+5,&pay_n,4);
    memcpy(header+9,&bits_n,4);

    int rc = 0;
    if(send_all(fd,header,Q2_HEADER_SIZE) < 0){
         rc = -1;
    }
    if(rc == 0 && send_all(fd,payload_ptr,payload_len) < 0) {
        rc = -1;
    }
    if(stats){
        stats->original_len = msg_len;
        stats->compressed_len = enc.byte_len;
        stats->compression_used = use_compressed;
        stats->bytes_sent = Q2_HEADER_SIZE + payload_len;
    }


    free(enc.bytes);
    return rc;
}


static uint8_t *q2_recv_message(int fd, size_t *out_len){
    uint8_t header[Q2_HEADER_SIZE];

    if(recv_all(fd, header, Q2_HEADER_SIZE) < 0){
        return NULL;
    }
    uint8_t compressed_flag = header[0];
    uint32_t orig_n, pay_n, bits_n;
    memcpy(&orig_n, header + 1, 4);
    memcpy(&pay_n, header + 5,4);
    memcpy(&bits_n, header + 9, 4);


    uint32_t original_len = ntohl(orig_n);
    uint32_t payload_len = ntohl(pay_n);
    uint32_t compressed_bits = ntohl(bits_n);
    uint8_t *payload_buf = (uint8_t *)malloc(payload_len > 0 ? payload_len : 1);

    if(recv_all(fd, payload_buf, payload_len) < 0){
        free(payload_buf);
        return NULL;
    }

    uint8_t *original;
    if(compressed_flag){
        original = lz77_decode(payload_buf,compressed_bits,original_len);
        free(payload_buf);
    }else{
        original = payload_buf;
    }

    if(out_len){
        *out_len = original_len;
    }
    return original;
}
#endif