/* 
 * File:   main.cpp
 * Author: sk
 * Created on April 24, 2020, 9:47 PM
 */

#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>      /* for TCP_xxx defines */
#include <sys/socket.h>       /* for select */
#include <arpa/inet.h>        /* inet(3) functions */
#include <fcntl.h>
#include <unistd.h>
#include <memory>
#include <string.h>
#include <math.h>
#include <time.h>             /* time */
#include <sys/time.h>         /* time */
#include <ctype.h>
#include <map>
#include <vector>
#include <atomic>
#include <sys/ioctl.h>        /* for ioctl */
#include <limits.h>
#include <signal.h>
#include <inttypes.h>
#include <stdint.h>
#include <netdb.h>

#include "include/util/util_string.h"
#include "include/util/util_filesys.h"
#include "include/rand_generate.h"

extern "C"
{
   #include "include/util/buf.h"
   #include "include/crypt/crc32/crc32.h"
   #include "include/textcolor.h"
}

#include "include/util/lock.h"

using namespace std;

#define VERSION  "1.0.1"

#define INTERVAL_SEND_DATA_MS      1000
#define LIMIT_COUNT_SEND           5
#define LIMIT_SIZE_INPUT_REGISTER  1000

#define COUNT_WORKERS              3

#define REMOVE_SESSION_USER_NO_ACTIVE_IN_SEC   30
#define IDLE_TIME_IN_SEC_AFTER_REMOVE_SESSION  25

#define TYPE_SEND_DEFAULT     0
#define TYPE_SEND_NO_WAIT     1

#define SEND_DATA_TYPE_CALBACK_DATA___CHECK_MTU       0x01
#define SEND_DATA_TYPE_CALBACK_DATA___DOWNLOAD_FILE   0x02

#define STATUS_DOWNLOAD_FILE__NONE                    0x00
#define STATUS_DOWNLOAD_FILE__CHECK_MTU               0x01
#define STATUS_DOWNLOAD_FILE__RUN_UPLOAD              0x02
#define STATUS_DOWNLOAD_FILE__ERROR_NEED_REMOVE_TASK  0x03
#define STATUS_DOWNLOAD_FILE__END_UPLOAD_REMOVE_TASK  0x04

#define SENDDATA_QUEUE_LIMIT_MEMORY                   1024 * 1024 * 10

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

const int UDP_BUFLEN       = 1024*64;
const int UDP_PORT         = 25698;

uint8_t default_xor_key[]  = { 0x10, 0xaf, 0x25, 0x36, 0xa8, 0xad, 0xd0, 0x13, 0x55, 0x77, 0x12, 0x08, 0x19, 0xdf, 0x3f, 0x3f };
uint8_t iv[]               = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f };
uint8_t iv2[]              = { 0x10, 0x51, 0x72, 0xb3, 0xf4, 0x80, 0xac, 0x53, 0x42, 0x35, 0x25, 0x2b, 0x7c, 0xbd, 0xbe, 0x18 };

char abs_exe_path[512]     = "";
//-----------------------------------------------------------------------------------------
struct UdpRcvSocket_READER
{
    // ++ UDP info ---------------
    int soket;
    struct sockaddr_in si_other;
    socklen_t slen;
    string address_and_port;
    // -- UDP info ---------------
    
    uint8_t *buf;
    uint16_t buf_len;

    UdpRcvSocket_READER()
    {
        buf = NULL;
        soket = 0;
    }
    
    ~UdpRcvSocket_READER()
    {
        if(buf != NULL)
        {
            free(buf);
            buf = NULL;
        }
    }
};
//-----------------------------------------------------------------------------------------
struct ReadCommand
{
    UdpRcvSocket_READER* udp_info;
    
    bool init_ok;
    uint64_t timestamp;
    long len_message;
    string cmd;
    vector<string> params;
    int len_buffer;
    unsigned char* buffer;

    ReadCommand()
    {
        cmd = "";
        len_message = 0;
        init_ok = false;

        len_buffer = 0;
        buffer = NULL;
    }

    ~ReadCommand()
    {
        if(buffer != NULL)
        {
            free(buffer);
            buffer = NULL;
        }
    }
};
//-----------------------------------------------------------------------------------------
struct CheckMTU_STEP
{
    uint8_t count_iteration, counter_test;
    uint32_t test, lask_k, diff;
    vector<string> params;
    
    CheckMTU_STEP(CheckMTU_STEP *data)
    {
        count_iteration = data->count_iteration;
        test            = data->test;
        lask_k          = data->lask_k;
        diff            = data->diff;
        counter_test    = data->counter_test;
    }
    
    CheckMTU_STEP()
    {
        count_iteration = 1;
        test            = 1024 * 56;
        lask_k          = 0;
        diff            = 0;
        counter_test    = 0;
    }
};
//-----------------------------------------------------------------------------------------
struct UploadFile
{
    // ++ UDP info ---------------
    int soket;
    struct sockaddr_in si_other;
    socklen_t slen;
    string address_and_port;
    // -- UDP info ---------------
    
    uint8_t status;             // статус работы 
    string fullFilePath;
    FILE *file;
    pthread_rwlock_t lock_item;
    vector<string> params;
    
    CheckMTU_STEP *mtu;
    bool need_remove;
    int counter_references;
    
    UploadFile()
    {
        file          = NULL;
        fullFilePath  = "";
        status        = STATUS_DOWNLOAD_FILE__NONE;
        mtu           = NULL;
        need_remove          = false;
        counter_references   = 0;
        
        pthread_rwlock_init(&lock_item, NULL);
    }
    
    ~UploadFile()
    {
        pthread_rwlock_destroy(&lock_item);
        
        if( file != NULL )
        {
            fclose(file);
        }
        
        if( mtu != NULL )
        {
            delete mtu;
            mtu = NULL;
        }
        
        printf("~UploadFile \n");
    }
    
    void copyNetInfo(UdpRcvSocket_READER* udp_info)
    {
        memcpy(&si_other, &udp_info->si_other, sizeof(struct sockaddr_in));
        
        soket            = udp_info->soket;
        slen             = udp_info->slen;
        address_and_port = udp_info->address_and_port;
    }
    
    void lockREAD(int line, const char* function_name)
    {
        __LOCK_READ(&lock_item, line, function_name);
    }
    
    void lockWRITE(int line, const char* function_name)
    {
        __LOCK_WRITE(&lock_item, line, function_name);
    }
    
    void lockFREE(int line, const char* function_name)
    {
        __LOCK_FREE(&lock_item, line, function_name);
    }
};
//-----------------------------------------------------------------------------------------
struct SendData
{
    // ++ UDP info ---------------
    int soket;
    struct sockaddr_in si_other;
    socklen_t slen;
    string address_and_port;
    // -- UDP info ---------------
    
    uint64_t timestamp;
    
    string cmd;
    vector<string> params;
    struct buf* add_output_byffer;
    
    int count_send;                 // попытки отправки данных - счётчик
    uint64_t interval_send;         // интервал отправки
    uint64_t timestamp_last_send;
    bool need_remove;               // метка удаления

    //bool OK_SEND;
    uint8_t type_send;

    pthread_rwlock_t lock_item;
    
    uint8_t callback_typedata;
    void* callback_data;
    void (*callback_okSend)(SendData *sd, uint8_t typedata, void *data);
    void (*callback_errorSend)(SendData *sd, uint8_t typedata, void *data);

    SendData()
    {
        cmd                  = "";
        params               = vector<string>();
        add_output_byffer    = NULL;
        //OK_SEND              = false;
        type_send            = TYPE_SEND_DEFAULT;
        
        callback_okSend      = NULL;
        callback_errorSend   = NULL;
        callback_data        = NULL;

        soket                = 0;
        slen                 = 0;
        count_send           = 0;
        timestamp_last_send  = 0;
        need_remove          = false;
        interval_send        = INTERVAL_SEND_DATA_MS;
        timestamp            = 0;

        pthread_rwlock_init(&lock_item, NULL);
    }

    ~SendData()
    {
        pthread_rwlock_destroy(&lock_item);

        if( add_output_byffer != NULL )
        {
            buf_free( add_output_byffer );
            add_output_byffer = NULL;
        }
        
        if( callback_data != NULL)
        {
            if(callback_typedata == SEND_DATA_TYPE_CALBACK_DATA___CHECK_MTU)
            {
                delete ((CheckMTU_STEP *) callback_data);
            }
            else if(callback_typedata == SEND_DATA_TYPE_CALBACK_DATA___DOWNLOAD_FILE)
            {
                UploadFile* df = (UploadFile *) callback_data;
                
                if(df != NULL)
                {
                    df->lockWRITE(__LINE__, __FUNCTION__ );
                
                    df->counter_references -= 1;
                    
                    df->lockFREE(__LINE__, __FUNCTION__ );
                }
            }
        }
    }
    
    void copyNetInfo(UploadFile *df)
    {
        memcpy(&si_other, &df->si_other, sizeof(struct sockaddr_in));
        soket            = df->soket;
        slen             = df->slen;
        address_and_port = df->address_and_port;
        
        /*printf("%s:%s:%d\n"
                      , address_and_port.c_str()
                      , inet_ntoa(si_other.sin_addr)
                      , ntohs(si_other.sin_port));*/
    }
    
    void copyNetInfo(SendData *sd)
    {
        memcpy(&si_other, &sd->si_other, sizeof(struct sockaddr_in));
        soket            = sd->soket;
        slen             = sd->slen;
        address_and_port = sd->address_and_port;
    }
    
    void copyNetInfo(UdpRcvSocket_READER* udp_info)
    {
        memcpy(&si_other, &udp_info->si_other, sizeof(struct sockaddr_in));
        
        soket            = udp_info->soket;
        slen             = udp_info->slen;
        address_and_port = udp_info->address_and_port;
    }
    
    void lockREAD(int line, const char* function_name)
    {
        __LOCK_READ(&lock_item, line, function_name);
    }
    
    void lockWRITE(int line, const char* function_name)
    {
        __LOCK_WRITE(&lock_item, line, function_name);
    }
    
    void lockFREE(int line, const char* function_name)
    {
        __LOCK_FREE(&lock_item, line, function_name);
    }
};
//-----------------------------------------------------------------------------------------
struct IOWorker
{
    pthread_t th;
    int index;
    vector<UdpRcvSocket_READER *> queue_udp_command;
    
    pthread_rwlock_t lock_item;
    
    IOWorker()
    {
        index = 0;
        pthread_rwlock_init(&lock_item, NULL);
    }
    
    ~IOWorker()
    {
        pthread_rwlock_destroy(&lock_item);
    }
    
    void lockREAD(int line, const char* function_name)
    {
        __LOCK_READ(&lock_item, line, function_name);
    }
    
    void lockWRITE(int line, const char* function_name)
    {
        __LOCK_WRITE(&lock_item, line, function_name);
    }
    
    void lockFREE(int line, const char* function_name)
    {
        __LOCK_FREE(&lock_item, line, function_name);
    }
};
//-----------------------------------------------------------------------------------------
struct InputCmdReg
{
    string address_and_port;
    uint64_t timestamp;
};
//-----------------------------------------------------------------------------------------
struct UserInfo
{
    
};
//-----------------------------------------------------------------------------------------
struct SessionUser
{
    time_t last_active_tms, last_mtu_check;
    uint16_t mtu_value;
    
    struct UserInfo *user_info;
    
    pthread_rwlock_t lock_item;
    
    SessionUser()
    {
        last_mtu_check = 0;
        mtu_value = 0;
        
        last_active_tms = time(NULL);
        pthread_rwlock_init(&lock_item, NULL);
        
        user_info = NULL;
    }
    
    ~SessionUser()
    {
        if( user_info != NULL )
        {
            delete user_info;
        }
        
        pthread_rwlock_destroy(&lock_item);
    }
    
    void lockREAD(int line, const char* function_name)
    {
        __LOCK_READ(&lock_item, line, function_name);
    }
    
    void lockWRITE(int line, const char* function_name)
    {
        __LOCK_WRITE(&lock_item, line, function_name);
    }
    
    void lockFREE(int line, const char* function_name)
    {
        __LOCK_FREE(&lock_item, line, function_name);
    }
};
//-----------------------------------------------------------------------------------------
//-----------------------------------------------------------------------------------------

vector<InputCmdReg*> register_input;
pthread_rwlock_t rwlock__register_input;

map<string, uint64_t> WAIT_SEND_SOCKET;
pthread_rwlock_t rwlock_wait_send_socket;

const int sizestructSendData = sizeof(struct SendData);

std::atomic<int_fast16_t> totalUseMemoryQueueSend(0);
vector<SendData*> queue_send;
pthread_rwlock_t rwlock__queue_send;
bool wait_queue_send = false;

struct IOWorker workersIO[ COUNT_WORKERS ];

map<string, SessionUser*> SESSION_USERS;
pthread_rwlock_t rwlock_session_users;

vector<UploadFile*> UPLOAD_TASKS;
pthread_rwlock_t rwlock_download_tasks;

//***  ***  ***  ***  ***  ***  ***  ***  ***  ***  ***  ***  ***  ***  ***  ***** 
void msleep(unsigned int msec);
void _exec(ReadCommand *data, SessionUser *user);

ssize_t _send_data_from_socket_crypt_encode(
                    int soket
                  , struct sockaddr_in si_other
                  , socklen_t slen

                  , int *sended_length
                  , uint64_t timestamp
                  , const char* send_cmd
                  , uint8_t xor_key[16]
                  , vector<string> params = vector<string>()
                  , struct buf* add_output_byffer = NULL
            );

void deleteSession(string address_and_port);
uint64_t get_new_timestamp(bool need_block);

void _okSend(SendData *sd, uint8_t typedata, void *data);
void _errorSend(SendData *sd, uint8_t typedata, void *data);
//***  ***  ***  ***  ***  ***  ***  ***  ***  ***  ***  ***  ***  ***  ***  ***** 
///------------------------------------------------------------------------------------------
void __inc_totalUseMemoryQueueSend(SendData *data)
{
    totalUseMemoryQueueSend += sizestructSendData + ( data->add_output_byffer != NULL ? data->add_output_byffer->len : 0 );
}
///------------------------------------------------------------------------------------------
void __dec_totalUseMemoryQueueSend(SendData *data)
{
    totalUseMemoryQueueSend -= sizestructSendData + ( data->add_output_byffer != NULL ? data->add_output_byffer->len : 0 );
}
///------------------------------------------------------------------------------------------
uint8_t circular_left_shift(uint8_t input, int n)
{
    uint8_t size = 8;
    uint8_t result;
     
    n = n % size;
    result = input << n | input >> (size - n);
    return result;
}
///------------------------------------------------------------------------------------------
uint8_t circular_right_shift(uint8_t input, int n)
{
    uint8_t size = 8;
    uint8_t result;
 
    n = n % size;
    result = input >> n | input << (size - n);
    return result;
}
///------------------------------------------------------------------------------------------
uint8_t *get_key(float r1, float r2, int len)
{
    uint8_t ff[] = { 0x58, 0xd6, 0xa2, 0x45, 0x3b, 0xa0 };
    
    uint8_t *result = (uint8_t*) malloc( len * 8 );
    int offset = 0;
    float i= 0, step = 360.0f / (float) len;
    
    for(int f = 0; f < len; f++)
    {
        float x = r1 * cos( i * M_PI / 180.0 );
        float y = r2 * sin( i * M_PI / 180.0 );
        
        x = round(x * 1000);
        y = round(y * 1000);
        
        memcpy(result + offset, &x, 4); offset += 4;
        memcpy(result + offset, &y, 4); offset += 4;
        
        i += step;
    }
    
    for(int f = 0; f < len * 8; f++)
    {
        result[f] ^= ff[ f % 6 ];
        result[f] = circular_left_shift(result[f], (ff[ f % 6 ] + 1) % 8);
        result[f] = ~result[f];
    }
    
    return result;
}
///------------------------------------------------------------------------------------------
uint8_t* _af5789(uint8_t* data, uint32_t len, uint8_t *key, uint8_t key_len, int encrypt)
{
    uint8_t* s = (uint8_t*)calloc( len, 1 );
    uint32_t i = 0;
    uint8_t shift, shift2, tmp;

    memcpy(s, data, len);

    size_t f = 0;
    for(i = 0; i < len; i++)
    {
        shift  = i*3 % 8;
        shift2 = i*2 % 8;
        if(shift == 0)
        {
            shift = 3;
        }
        
        if( encrypt == 1 )
        {
            s[i] ^= key[f++ % key_len];
            s[i] = circular_left_shift(s[i], shift);
            s[i] = ~s[i];
            s[i] ^= iv[i % 16];
            s[i] = circular_left_shift(s[i], shift2);
            s[i] ^= iv2[i % 16];
        }
        else
        {
            s[i] ^= iv2[i % 16];
            s[i] = circular_right_shift(s[i], shift2);
            s[i] ^= iv[i % 16];
            s[i] = ~s[i];
            s[i] = circular_right_shift(s[i], shift);
            s[i] ^= key[f++ % key_len];
        }
    }
    
    return s;
}
///------------------------------------------------------------------------------------------
uint8_t *_encrypt(uint8_t *input, unsigned int len_input, float f1, float f2, int f3)
{
    uint8_t *key    = get_key(f1, f2, f3);
    uint8_t *result = _af5789(input, len_input, key, f3 * 8, 1);
    
    free(key);
    return result;
}

uint8_t *_decrypt(uint8_t *input, unsigned int len_input, float f1, float f2, int f3)
{
    uint8_t *key    = get_key(f1, f2, f3);
    uint8_t *result = _af5789(input, len_input, key, f3 * 8, 2);
    
    free(key);
    return result;
}
///------------------------------------------------------------------------------------------
unsigned char* simple_xor(unsigned char* data, uint32_t len, uint8_t *key, uint8_t key_len)
{
    unsigned char* s = (unsigned char*)calloc( len, 1 );
    uint32_t i = 0;

    memcpy(s, data, len);

    size_t f = 0;
    for(i = 0; i < len; i++)
    {
        s[i] ^= key[f++ % key_len];
    }
    return s;
}
///------------------------------------------------------------------------------------------
float ReverseFloat( const float inFloat )
{
   float retVal;
   char *floatToConvert = ( char* ) & inFloat;
   char *returnFloat = ( char* ) & retVal;

   // swap the bytes into a temporary buffer
   returnFloat[0] = floatToConvert[3];
   returnFloat[1] = floatToConvert[2];
   returnFloat[2] = floatToConvert[1];
   returnFloat[3] = floatToConvert[0];

   return retVal;
}
///------------------------------------------------------------------------------------------
ssize_t _send_data_from_socket_crypt_encode(
                    int soket
                  , struct sockaddr_in si_other
                  , socklen_t slen

                  , int *sended_length
                  , uint64_t timestamp
                  , const char* send_cmd
                  , uint8_t xor_key[16]
                  , vector<string> params
                  , struct buf* add_output_byffer
        )
{
    struct buf* output_byffer = (struct buf*)buf_new();
    struct buf* encode_byffer = (struct buf*)buf_new();
    uint16_t _len_us    = 0;
    
    unsigned char *res_encode = NULL, *out_aes = NULL, *res_encode_0 = NULL;
    uint32_t _len_add_byffer = 0;
    int len_cmd = strlen(send_cmd), params_counts = params.size();
    
    initrand();
    
    // here encrypt parameters
    float f1 = randfloat(10, 80);
    float f2 = randfloat(10, 80);
    short f3 = 4 + myrandom(4);
    
    printf("fff: %.4f %.4f %d\n", f1, f2, f3);

    _len_us  = (uint16_t) len_cmd;  /// запись команды + параметры
    _len_us  = htons( (uint16_t) _len_us );

    timestamp = htole64( timestamp );

//#if DEBUG_PRINT
    //if( strcmp(send_cmd, "OK_SEND") != 0 )
    /*{
        textcolor(BRIGHT, YELLOW, BLACK);
        printf("_send_data_from_socket_crypt_encode -> %s %ld\n", send_cmd, timestamp );
        textcolor(RESET, WHITE, BLACK);
        
    }*/
//#endif

    buf_append_u8(encode_byffer, '*');
    buf_append_u8(encode_byffer, '-');
    buf_append_data(encode_byffer, &timestamp, sizeof(uint64_t) );
    buf_append_u16( encode_byffer,  _len_us );
    buf_append_data(encode_byffer, (void*) send_cmd, len_cmd );

    /*if(send_data.length() != utf8_length( send_data ) )
    {
        printf("cmd: %s %d %d\n", send_data.c_str(), send_data.length(), utf8_length( send_data ) );
    }*/

    /// всего параметров
    buf_append_u8(encode_byffer,  (uint8_t) params_counts );

    for(unsigned int i = 0; i < params_counts; i++)
    {
        _len_us = (uint16_t) params[i].length();
        _len_us = htons( (uint16_t) _len_us );

        //printf("send parameter [%d] = %d %s\n", i, params[i].length(), params[i].c_str() );

        buf_append_u16(encode_byffer,  _len_us );
        buf_append_data(encode_byffer, (void*)params[i].c_str(), params[i].length() );
    }

    if( add_output_byffer != NULL )
    {
        _len_add_byffer = add_output_byffer->len;
    }

    buf_append_u32(encode_byffer,  htonl( (uint32_t) _len_add_byffer ) );

    if( add_output_byffer != NULL )
    {
        //printf("add_output_byffer->len: %d\n", add_output_byffer->len);

        buf_append_data(encode_byffer, add_output_byffer->ptr, add_output_byffer->len);
    }

    res_encode_0 = _encrypt(encode_byffer->ptr, encode_byffer->len, f1, f2, f3 );
    res_encode   = simple_xor(res_encode_0, encode_byffer->len, xor_key, 16 );

    //crc32 calc
    uint32_t crc_buffer = xcrc32( (unsigned char *)res_encode, encode_byffer->len, 0xffffffff );
    
    printf("crc32: %ld\n", crc_buffer);

    buf_append_u8( output_byffer, (unsigned char)'*' );             /// проверочный байт *
    buf_append_u32(output_byffer,  htonl( (uint32_t) encode_byffer->len ) );
    
    buf_append_data(output_byffer, &f1, 4);
    buf_append_data(output_byffer, &f2, 4);
    buf_append_data(output_byffer, &f3, 2);

    buf_append_data(output_byffer, res_encode, encode_byffer->len);
    
    buf_append_u8( output_byffer, (unsigned char)'=' );
    buf_append_data(output_byffer, &crc_buffer, sizeof(uint32_t));
    
    // возвращает число переданных байт в случае успешного завершения и -1 в противном случае.
    int result = sendto(  
              soket
            , output_byffer->ptr
            , output_byffer->len
            , 0
            , (struct sockaddr*) &si_other
            , slen
        );
    
    /*if( result == -1 )
    {
        printf("[%d] errno=%d\n", __LINE__, errno);
    }*/

    *sended_length = output_byffer->len;

    buf_free(output_byffer);
    buf_free(encode_byffer);
    
    free(res_encode);
    free(res_encode_0);

    return result;
}
///------------------------------------------------------------------------------------------
ReadCommand* _readCommand(UdpRcvSocket_READER* _item)
{
    ReadCommand *res           = NULL;
    int offset_1               = 0;
    struct buf* encode_byffer  = NULL;
    
    res = new ReadCommand();
    res->udp_info = _item;

    if( offset_1 + 4 > _item->buf_len )
    {
        return res;
    }

    long len_data = 0, len_cmd = 0;
    float f1, f2;
    short f3;

    memcpy(&len_data, _item->buf + offset_1, 4);
    offset_1 += 4;
    
    //printf("%d == %d\n", len_data + 4 + 4 + 4 + 2 + 4, _item->buf_len );
    
    if( len_data + 4 + 4 + 4 + 2 + 4  > _item->buf_len )
    {
        return res;
    }

    memcpy(&f1, _item->buf + offset_1, 4); offset_1 += 4;
    memcpy(&f2, _item->buf + offset_1, 4); offset_1 += 4;
    memcpy(&f3, _item->buf + offset_1, 2); offset_1 += 2;
    
    //f1 = ReverseFloat(f1);
    //f2 = ReverseFloat(f2);
    
    printf("fff: %.3f %.3f %d\n", f1, f2, f3);

    encode_byffer = (struct buf*) buf_new();
    buf_append_data(encode_byffer, _item->buf + offset_1, len_data);
    offset_1 += len_data;

    uint32_t _crc32 = 0;

    if( offset_1 + 4 > _item->buf_len )
    {
        return res;
    }

    memcpy(&_crc32, _item->buf + offset_1, 4); //offset_1 += 4;

    printf("crc32: %d\n", _crc32);

    //crc32 calc
    uint32_t crc_buffer = xcrc32((unsigned char *)encode_byffer->ptr, encode_byffer->len, 0xffffffff);

    //res1 = (int)crc_buffer;
    //crc_buffer = crc_buffer ^ 0xffffffff;

    printf("crc32_2: %d\n", crc_buffer);

    if( _crc32 != crc_buffer )
    {
        printf("error crc32_2\n");
        return res;
    }

    ///----------------------------------------------------------------------------------
    unsigned char *data_encoded_0 = simple_xor(encode_byffer->ptr, len_data, default_xor_key, 16);
    unsigned char *data_encoded   = _decrypt(data_encoded_0, len_data, f1, f2, f3);

    res->len_message = len_data;

    //unsigned char token = 0;
    int offset = 0;

    /*memcpy(&token, data_encoded + offset, 1); offset += 1;
    if(token != 42)
    {
        //printf("error token\n");
        buf_free( encode_byffer );
        free( data_encoded );
        return res;
    }*/
    //-----------------------------------------------------------

    char *read_str = NULL;
    unsigned char count_params = 0;
    uint64_t timestamp;
    char check_ch1, check_ch2;

    memcpy(&check_ch1, data_encoded + offset, 1); offset += 1;
    memcpy(&check_ch2, data_encoded + offset, 1); offset += 1;

    if( check_ch1 != '*' && check_ch2 != '-' )
    {
        printf("error read *-\n");

        //res.stop_socket = true;
        buf_free( encode_byffer );
        free(data_encoded);
        free( data_encoded_0 );
        return res;
    }

    memcpy(&timestamp, data_encoded + offset, sizeof(uint64_t));  offset += sizeof(uint64_t);
    memcpy(&len_cmd, data_encoded + offset, 4);                   offset += 4;
    
    printf("timestamp = %ld\n", timestamp);

    printf("len_cmd: %ld\n", len_cmd);

    read_str = (char*)calloc( len_cmd + 1, 1 );

    if(read_str == NULL)
    {
        buf_free( encode_byffer );
        free( data_encoded );
        free( data_encoded_0 );
        return res;
    }

    memcpy(read_str, data_encoded + offset, len_cmd); offset += len_cmd;

    res->cmd.append( read_str );
    res->timestamp = timestamp;

    //printf("read_str: %s\n", read_str);
    
    if(read_str != NULL)
    {
        free(read_str);
        read_str = NULL;
    }

    memcpy(&count_params, data_encoded + offset, 1); offset += 1;

    //error_flag = false;

    for(unsigned char k = 0; k < count_params; k++)
    {
        string _s_str = "";
        uint16_t len = 0;

        memcpy(&len, data_encoded + offset, 2); offset += 2;

        if(len < 0 || len > 65536)
        {
            //printf("error read parameters \n");
            buf_free( encode_byffer );
            free( data_encoded );
            free( data_encoded_0 );
            return res;
        }

        read_str = (char*)calloc( len + 1, sizeof(char) );

        if(read_str == NULL)
        {
            buf_free( encode_byffer );
            free( data_encoded );
            free( data_encoded_0 );
            return res;
        }

        memcpy(read_str, data_encoded + offset, len); offset += len;

        _s_str.append( read_str );

        if(read_str != NULL)
        {
            free(read_str);
            read_str = NULL;
        }

        //printf("_s_str: %s\n", _s_str.c_str());
        res->params.push_back( _s_str );
    }

    //if(error_flag == false)
    {
        int len_buffer = 0;

        memcpy(&len_buffer, data_encoded + offset, 4); offset += 4;

        //printf("len_buffer: %d\n", len_buffer);

        res->len_buffer = len_buffer;

        res->buffer = (unsigned char*) calloc( len_buffer, 1 );

        memcpy(res->buffer, data_encoded + offset, len_buffer); //offset += len_buffer;

        res->init_ok = true;
    }

    buf_free( encode_byffer );
    free( data_encoded_0 );
    free( data_encoded );
    return res;
}
///------------------------------------------------------------------------------------------
void *thread_add_SendData(void *arg)
{
    SendData *_tmp = (SendData *) arg;
    
    __LOCK_WRITE(&rwlock__queue_send, __LINE__, __FUNCTION__);

    __inc_totalUseMemoryQueueSend(_tmp);

    _tmp->timestamp = get_new_timestamp(false);
    queue_send.push_back( _tmp );

    __LOCK_FREE( &rwlock__queue_send, __LINE__, __FUNCTION__ );
    
    pthread_exit(0);
}
///------------------------------------------------------------------------------------------
void *thread_worker(void *arg)
{
    pthread_setname_np(pthread_self(), "thread_worker");

    int index = *((int *) arg);
    free( arg );

    UdpRcvSocket_READER* _item;
    SessionUser *p_user;
    
    while (true)
    {
        while( workersIO[index].queue_udp_command.size() > 0 )
        {
            _item = workersIO[index].queue_udp_command[0];
            
            ReadCommand *parse = _readCommand(_item);
            
            if(parse->init_ok)
            {
                __LOCK_READ(&rwlock_session_users, __LINE__, __FUNCTION__);

                map<string, SessionUser*>::iterator _it = SESSION_USERS.find( parse->udp_info->address_and_port );
                
                if(  _it == SESSION_USERS.end() )
                {
                    SessionUser *user = new SessionUser();
                    
                    p_user = user;
                    
                    __LOCK_FREE( &rwlock_session_users, __LINE__, __FUNCTION__ );
                    __LOCK_WRITE(&rwlock_session_users, __LINE__, __FUNCTION__);
                    
                    //printf("== %s\n", parse->udp_info->address_and_port.c_str());
                    
                    SESSION_USERS[ parse->udp_info->address_and_port ] = user;
                    
                    // ++ check mtu
                    ///------------------------------------------------------------------
                    /*SendData* _tmp = new SendData();
                    CheckMTU_STEP *_step = new CheckMTU_STEP();

                    _tmp->copyNetInfo(parse->udp_info);
                    
                    _tmp->cmd              = "CHECK_MTU";

                    _tmp->add_output_byffer   = (struct buf*)buf_new();
                    
                    _tmp->interval_send       = 250;
                    _tmp->callback_data       = (void *) ( _step );
                    _tmp->callback_typedata   = SEND_DATA_TYPE_CALBACK_DATA___CHECK_MTU;
                    _tmp->callback_okSend     = _okSend;
                    _tmp->callback_errorSend  = _errorSend;

                    uint8_t *test_buf = (uint8_t *) malloc( _step->test );

                    buf_append_data(_tmp->add_output_byffer, test_buf, _step->test);

                    free(test_buf);

                    __LOCK_WRITE(&rwlock__queue_send, __LINE__, __FUNCTION__);

                    __inc_totalUseMemoryQueueSend(_tmp);
                    
                    _tmp->timestamp = get_new_timestamp(false);
                    queue_send.push_back( _tmp );

                    __LOCK_FREE( &rwlock__queue_send, __LINE__, __FUNCTION__ );
                    ///-----------------------------------------------------------------*/
                }
                else
                {
                    p_user = _it->second;
                    
                    _it->second->lockWRITE(__LINE__, __FUNCTION__);
                    
                    _it->second->last_active_tms = time(NULL);
                    
                    _it->second->lockFREE(__LINE__, __FUNCTION__);
                }

                __LOCK_FREE( &rwlock_session_users, __LINE__, __FUNCTION__ );
                
                _exec(parse, p_user);
            }
            else
            {
                printf("error_read\n");
            }
            
            workersIO[index].lockWRITE(__LINE__, __FUNCTION__);

            delete _item;
            delete parse;
            workersIO[index].queue_udp_command.erase( workersIO[index].queue_udp_command.begin() );

            workersIO[index].lockFREE(__LINE__, __FUNCTION__);
            
            msleep(3);
        }
        
        msleep(5);
    }

    printf("stop thread_worker");

    pthread_exit(0);
}
///------------------------------------------------------------------------------------------
void *thread_upload(void *arg)
{
    pthread_setname_np(pthread_self(), "thread_upload");
    
    vector<SendData*> list_send;
    uint8_t *buf;
    
    while(true)
    {
        list_send.clear();
        
        if( totalUseMemoryQueueSend < SENDDATA_QUEUE_LIMIT_MEMORY )
        {
            __LOCK_READ( &rwlock_download_tasks, __LINE__, __FUNCTION__ );

            for(unsigned int i = 0, __len = UPLOAD_TASKS.size(); i < __len; i++)
            {
                UploadFile *_item = UPLOAD_TASKS[ i ];

                _item->lockWRITE(__LINE__, __FUNCTION__ );

                if( ! _item->need_remove && _item->counter_references < 50 )
                {
                    if(_item->status == STATUS_DOWNLOAD_FILE__NONE)
                    {
                        printf("Start upload. Step 1 - check mtu\n");
                        _item->status = STATUS_DOWNLOAD_FILE__CHECK_MTU;

                        // ++ check mtu
                        ///------------------------------------------------------------------
                        SendData* _tmp = new SendData();
                        _item->mtu = new CheckMTU_STEP();

                        for(int f = 0; f < _item->params.size(); f++)
                        {
                            _item->mtu->params.push_back( _item->params[f] );
                            _tmp->params.push_back( _item->params[f] );
                        }

                        _tmp->copyNetInfo(_item);

                        _tmp->cmd                 = "CHECK_MTU";

                        _tmp->add_output_byffer   = (struct buf*)buf_new();

                        _tmp->interval_send       = 250;
                        _tmp->callback_data       = (void *) ( _item );
                        _tmp->callback_typedata   = SEND_DATA_TYPE_CALBACK_DATA___DOWNLOAD_FILE;
                        _tmp->callback_okSend     = _okSend;
                        _tmp->callback_errorSend  = _errorSend;

                        uint8_t *test_buf = (uint8_t *) malloc( _item->mtu->test );

                        buf_append_data(_tmp->add_output_byffer, test_buf, _item->mtu->test);

                        free(test_buf);

                        _item->counter_references += 1;

                        list_send.push_back(_tmp);

                        /*__LOCK_WRITE(&rwlock__queue_send, __LINE__, __FUNCTION__);

                        __inc_totalUseMemoryQueueSend(_tmp);

                        _tmp->timestamp = get_new_timestamp(false);
                        queue_send.push_back( _tmp );

                        __LOCK_FREE( &rwlock__queue_send, __LINE__, __FUNCTION__ );*/
                        ///-----------------------------------------------------------------
                    }
                    else if(_item->status == STATUS_DOWNLOAD_FILE__RUN_UPLOAD)
                    {
                        //if( totalUseMemoryQueueSend < SENDDATA_QUEUE_LIMIT_MEMORY )
                        //{
                            //printf(">>>> Run upload. Step 2. Size mtu = %d\n", _item->mtu->lask_k);

                            for(uint8_t pp = 0; pp < 10; pp++)
                            {
                                uint32_t size_need = _item->mtu->lask_k - 10;
                                buf                = (uint8_t *) malloc(size_need);

                                uint32_t position_file = ftell(_item->file);
                                uint32_t count_read    = fread(buf, 1, size_need, _item->file);

                                //printf("read file. [%d] - %d\n", position_file, count_read);

                                if( count_read == 0 )
                                {
                                    free(buf);
                                    _item->status = STATUS_DOWNLOAD_FILE__END_UPLOAD_REMOVE_TASK;
                                    break;
                                }
                                else
                                {
                                    // ++ step upload
                                    ///------------------------------------------------------------------
                                    SendData* _tmp = new SendData();

                                    for(int f = 0; f < _item->params.size(); f++)
                                    {
                                        _tmp->params.push_back( _item->params[f] );
                                    }

                                    _tmp->copyNetInfo(_item);

                                    _tmp->type_send           = TYPE_SEND_NO_WAIT;
                                    _tmp->cmd                 = "UPLOADFILE_STEP";

                                    _tmp->add_output_byffer   = (struct buf*)buf_new();

                                    _tmp->callback_data       = (void *) ( _item );
                                    _tmp->callback_typedata   = SEND_DATA_TYPE_CALBACK_DATA___DOWNLOAD_FILE;
                                    //_tmp->callback_okSend     = _okSend;
                                    _tmp->callback_errorSend  = _errorSend;

                                    buf_append_u32(_tmp->add_output_byffer, position_file);
                                    buf_append_data(_tmp->add_output_byffer, buf, count_read);
                                    
                                    _item->counter_references += 1;

                                    list_send.push_back(_tmp);
                                    
                                    free(buf);
                                }
                            } // --for
                        //}
                    }
                    else if(
                               (_item->status == STATUS_DOWNLOAD_FILE__ERROR_NEED_REMOVE_TASK)
                            || (_item->status == STATUS_DOWNLOAD_FILE__END_UPLOAD_REMOVE_TASK)
                          )
                    {
                        _item->need_remove = true;
                    }

                }

                _item->lockFREE(__LINE__, __FUNCTION__ );
            }

            __LOCK_FREE( &rwlock_download_tasks, __LINE__, __FUNCTION__ );

            //++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

            wait_queue_send = true;

            msleep(10);

            __LOCK_WRITE(&rwlock__queue_send, __LINE__, __FUNCTION__);

            for(int k = 0, _size = list_send.size(); k < _size; k++)
            {
                SendData *_tmp = list_send[ k ];

                __inc_totalUseMemoryQueueSend(_tmp);

                _tmp->timestamp = get_new_timestamp(false);
                queue_send.push_back( _tmp );
            }

            __LOCK_FREE( &rwlock__queue_send, __LINE__, __FUNCTION__ );

            wait_queue_send = false;

            //++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
        }
        
        msleep(3);

        /// удаление
        __LOCK_WRITE( &rwlock_download_tasks, __LINE__, __FUNCTION__ );

        for(unsigned int i = 0, len = UPLOAD_TASKS.size(); i < len; i++)
        {
            UploadFile *_item = UPLOAD_TASKS[ i ];

            _item->lockWRITE(__LINE__, __FUNCTION__ );
            
            //printf("_item->counter_references: %d\n", _item->counter_references);

            if( _item->counter_references == 0 && _item->need_remove )
            {
                printf("delete df\n");
                
                delete _item;
                _item = NULL;
                UPLOAD_TASKS.erase( UPLOAD_TASKS.begin() + i );
                
                break;
            }

            _item->lockFREE(__LINE__, __FUNCTION__ );
        }

        __LOCK_FREE( &rwlock_download_tasks, __LINE__, __FUNCTION__ );
        
        msleep(3);
    }
    
    pthread_exit(0);          /* terminate the thread */
}
///------------------------------------------------------------------------------------------
void *thread_bk(void *arg)
{
    pthread_setname_np(pthread_self(), "thread_bk");
    //int count_send = 0;

    string delete_session_key = "";
    
    while(true)
    {
        __LOCK_READ(&rwlock_session_users, __LINE__, __FUNCTION__);
        
        printf("count sessions: %d %d %d\n", SESSION_USERS.size(), WAIT_SEND_SOCKET.size(), queue_send.size());
        
        for(map<string, SessionUser*>::iterator it = SESSION_USERS.begin(); it != SESSION_USERS.end(); it++ )
        {
            SessionUser *_t = it->second;
            
            _t->lockREAD(__LINE__, __FUNCTION__);
            
            //printf("[%s]\n", it->first.c_str());
            if( time(NULL) - _t->last_active_tms > IDLE_TIME_IN_SEC_AFTER_REMOVE_SESSION )
            {
                delete_session_key = it->first;
                _t->lockFREE(__LINE__, __FUNCTION__);
                break;
            }
            
            _t->lockFREE(__LINE__, __FUNCTION__);
        }
        
        __LOCK_FREE( &rwlock_session_users, __LINE__, __FUNCTION__ );
        
        
        if( delete_session_key.length() > 0 )
        {
            deleteSession(delete_session_key);
            
            delete_session_key = "";
        }
        
        sleep(2);
    }
        
    pthread_exit(0);          /* terminate the thread */
}
///------------------------------------------------------------------------------------------
void *thread_queue_output(void *arg)
{
    pthread_setname_np(pthread_self(), "thread_queue_output");
    
    ssize_t status_send;
    bool next_iteration = true;
    int count_remove    = 0;

    while(true)
    {
        if( wait_queue_send )
        {
            msleep(5);
        }
        
        __LOCK_READ( &rwlock__queue_send, __LINE__, __FUNCTION__ );
        
        for(unsigned int i = 0, __len = queue_send.size(); i < __len; i++)
        {
            if( wait_queue_send )
            {
                break;
            }
            
            SendData* _item = queue_send[ i ];
            
            _item->lockWRITE(__LINE__, __FUNCTION__);

            if(
                    _item->need_remove == false
                &&  get_time_in_ms() - _item->timestamp_last_send > _item->interval_send )
            {
                if(  _item->type_send == TYPE_SEND_DEFAULT )
                {
                    __LOCK_WRITE( &rwlock_wait_send_socket, __LINE__, __FUNCTION__ );

                    map<string, uint64_t>::iterator __it = WAIT_SEND_SOCKET.find( _item->address_and_port );

                    if(  __it != WAIT_SEND_SOCKET.end() && __it->second != _item->timestamp )
                    {
                        //printf("- skip item: [%ld] %s %s\n", _item->timestamp, _item->address_and_port.c_str(), _item->cmd.c_str());

                        __LOCK_FREE( &rwlock_wait_send_socket, __LINE__, __FUNCTION__ );
                        _item->lockFREE(__LINE__, __FUNCTION__);
                        continue;
                    }

                    __LOCK_FREE( &rwlock_wait_send_socket, __LINE__, __FUNCTION__ );
                }
                
                _item->timestamp_last_send = get_time_in_ms();

                //if( _item->OK_SEND == false )  /// __НЕ__было__подтврждения__OK_SEND
                {
                    int sended_length = 0;
                    
                    status_send = _send_data_from_socket_crypt_encode(
                                                  _item->soket
                                                , _item->si_other
                                                , _item->slen
                            
                                                , &sended_length
                                                , _item->timestamp
                                                , _item->cmd.c_str()
                                                , default_xor_key
                                                , _item->params
                                                , _item->add_output_byffer
                                               );

                    if( status_send == -1 )
                    {
                        _item->need_remove = true;
                    
                        if( _item->callback_errorSend != NULL )
                        {
                            _item->callback_errorSend(_item, _item->callback_typedata, _item->callback_data);
                        }
                    }
                    else
                    {
                        _item->count_send += 1;
                    }
                }

                if( /*_item->OK_SEND == true ||*/ _item->count_send >= LIMIT_COUNT_SEND )
                {
                    _item->need_remove = true;
                    
                    if( _item->callback_errorSend != NULL )
                    {
                        _item->callback_errorSend(_item, _item->callback_typedata, _item->callback_data);
                    }
                }
            }

            _item->lockFREE(__LINE__, __FUNCTION__ );
        }

        __LOCK_FREE( &rwlock__queue_send, __LINE__, __FUNCTION__ );

        msleep(2);

        /// удаление
        __LOCK_WRITE( &rwlock__queue_send, __LINE__, __FUNCTION__ );

        next_iteration = true;
        count_remove = 0;

        while(next_iteration)
        {
            next_iteration = false;

            for(unsigned int i = 0, len = queue_send.size(); i < len; i++)
            {
                SendData* _item = queue_send[ i ];

                _item->lockWRITE(__LINE__, __FUNCTION__ );

                if( _item->need_remove == true )
                {
                    if( _item->type_send == TYPE_SEND_DEFAULT )
                    {
                        __LOCK_WRITE( &rwlock_wait_send_socket, __LINE__, __FUNCTION__ );

                        map<string, uint64_t>::iterator __it = WAIT_SEND_SOCKET.find( _item->address_and_port );

                        if( __it != WAIT_SEND_SOCKET.end() )
                        {
                            printf("WAIT_SEND_SOCKET - delete2: [%ld] %s %s\n", _item->timestamp, _item->address_and_port.c_str(), _item->cmd.c_str());
                            WAIT_SEND_SOCKET.erase( __it );
                        }

                        __LOCK_FREE( &rwlock_wait_send_socket, __LINE__, __FUNCTION__ );
                    }
                    
                    __dec_totalUseMemoryQueueSend(_item);
                    
                    delete _item;

                    queue_send.erase( queue_send.begin() + i );

                    next_iteration = true;
                    break;
                }

                _item->lockFREE(__LINE__, __FUNCTION__ );
            }
            
            count_remove += 1;
            
            if(count_remove > 10)
            {
                break;
            }
        }

        __LOCK_FREE( &rwlock__queue_send, __LINE__, __FUNCTION__ );

        msleep(2);

        /// очистка
        __LOCK_WRITE( &rwlock__register_input, __LINE__, __FUNCTION__ ); /// запись

        if( register_input.size() > LIMIT_SIZE_INPUT_REGISTER )
        {
            delete register_input[0];
            register_input[ 0 ] = NULL;
            
            register_input.erase( register_input.begin() ); /// удалить с начала
        }

        __LOCK_FREE( &rwlock__register_input, __LINE__, __FUNCTION__ );
        
        msleep(1);
    
    } // --while

    pthread_exit(0);          /* terminate the thread */
}
///---------------------------------------------------------------------------------------------
uint64_t get_new_timestamp(bool need_block)
{
    if( need_block )
    {
	__LOCK_READ( &rwlock__queue_send, __LINE__, __FUNCTION__ );
    }

    uint64_t result = get_timestamp();

    bool flag = true;
    unsigned int i = 0, len = 0;

    while(flag)
    {
        flag = false;

        for(i = 0, len = queue_send.size(); i < len; i++)
        {
            SendData* _item = queue_send[ i ];

            _item->lockREAD(__LINE__, __FUNCTION__ ); /// чтение

            if( _item->timestamp == result )
            {
                result += 1;
                flag = true;
                _item->lockFREE(__LINE__, __FUNCTION__ );
                break;
            }

            _item->lockFREE(__LINE__, __FUNCTION__ );
        }
    }

    if( need_block )
    {
	__LOCK_FREE( &rwlock__queue_send, __LINE__, __FUNCTION__ );
    }

    return result;
}
///---------------------------------------------------------------------------------------------
void msleep(unsigned int msec)
{
    struct timespec timeout0;
    struct timespec timeout1;
    struct timespec* tmp;
    struct timespec* t0 = &timeout0;
    struct timespec* t1 = &timeout1;

    t0->tv_sec = msec / 1000;
    t0->tv_nsec = (msec % 1000) * (1000 * 1000);

    while(nanosleep(t0, t1) == -1)
    {
        if(errno == EINTR)
        {
            tmp = t0;
            t0 = t1;
            t1 = tmp;
        }
        else
            return;
    }
    return;
}
//------------------------------------------------------------------------------
void _okSend(SendData *sd, uint8_t typedata, void *data)
{
    printf("_okSend %d\n", typedata);
    
    if( typedata == SEND_DATA_TYPE_CALBACK_DATA___CHECK_MTU )
    {
        CheckMTU_STEP *step = (CheckMTU_STEP *) data;
        
        step->lask_k = step->test;
        step->counter_test++;
        
        if( step->count_iteration == 1 )
        {
            printf(".[%s] lask_k=%d\n", sd->address_and_port.c_str(), step->lask_k);
            return;
        }
        else
        {
            step->test += step->diff;
        }
        
        step->count_iteration++;
        //printf("+[%s] mtu=%d\n", sd->address_and_port.c_str(), step->test);
        
        ///------------------------------------------------------------------
        SendData* _tmp = new SendData();

        _tmp->copyNetInfo(sd);
        
        _tmp->cmd                 = "CHECK_MTU";

        _tmp->add_output_byffer   = (struct buf*)buf_new();

        _tmp->interval_send       = 250;
        _tmp->callback_data       = (void *) ( new CheckMTU_STEP( step ) );
        _tmp->callback_typedata   = SEND_DATA_TYPE_CALBACK_DATA___CHECK_MTU;
        _tmp->callback_okSend     = _okSend;
        _tmp->callback_errorSend  = _errorSend;

        uint8_t *test_buf = (uint8_t *) malloc( step->test );
                
        buf_append_data(_tmp->add_output_byffer, test_buf, step->test);

        free(test_buf);
        
        ///------------------------------------------------------------------
        pthread_attr_t pattr_thread_add_SendData;
        pthread_t child_thread_add_SendData;
        // set thread create attributes
        pthread_attr_init(&pattr_thread_add_SendData);
        pthread_attr_setdetachstate(&pattr_thread_add_SendData, PTHREAD_CREATE_DETACHED);

        pthread_create(&child_thread_add_SendData, &pattr_thread_add_SendData, thread_add_SendData, _tmp);
        ///-----------------------------------------------------------------
    }
    else if( typedata == SEND_DATA_TYPE_CALBACK_DATA___DOWNLOAD_FILE )
    {
        //__LOCK_READ( &rwlock_download_tasks, __LINE__, __FUNCTION__ );
        
            UploadFile *df = (UploadFile *) data;
            
            if( df == NULL )
            {
                printf("df == NULL %d\n", __LINE__);
                return;
            }

            df->lockWRITE(__LINE__, __FUNCTION__ );

            if( ! df->need_remove )
            {
                if( df->status == STATUS_DOWNLOAD_FILE__CHECK_MTU )
                {
                    CheckMTU_STEP *step = (CheckMTU_STEP *) df->mtu;

                    step->lask_k = step->test;
                    step->counter_test++;
                    
                    printf("ok send mtu lask_k = %d\n", step->lask_k);

                    if( step->count_iteration == 1 )
                    {
                        printf(".[%s] %d lask_k=%d\n", sd->address_and_port.c_str(), step->count_iteration, step->lask_k);
                        df->status = STATUS_DOWNLOAD_FILE__RUN_UPLOAD;
                        df->lockFREE(__LINE__, __FUNCTION__ );
                        return;
                    }
                    else
                    {
                        step->test += step->diff;
                    }

                    step->count_iteration++;
                    //printf("+[%s] mtu=%d\n", sd->address_and_port.c_str(), step->test);

                    ///------------------------------------------------------------------
                    SendData* _tmp = new SendData();

                    for(int f = 0; f < step->params.size(); f++)
                    {
                        _tmp->params.push_back( step->params[f] );
                    }

                    _tmp->copyNetInfo(sd);

                    _tmp->cmd                 = "CHECK_MTU";

                    _tmp->add_output_byffer   = (struct buf*)buf_new();

                    _tmp->interval_send       = 250;
                    _tmp->callback_data       = (void *) ( df );
                    _tmp->callback_typedata   = SEND_DATA_TYPE_CALBACK_DATA___DOWNLOAD_FILE;
                    _tmp->callback_okSend     = _okSend;
                    _tmp->callback_errorSend  = _errorSend;

                    uint8_t *test_buf = (uint8_t *) malloc( step->test );

                    buf_append_data(_tmp->add_output_byffer, test_buf, step->test);

                    free(test_buf);
                    
                    df->counter_references += 1;

                    ///------------------------------------------------------------------
                    pthread_attr_t pattr_thread_add_SendData;
                    pthread_t child_thread_add_SendData;
                    // set thread create attributes
                    pthread_attr_init(&pattr_thread_add_SendData);
                    pthread_attr_setdetachstate(&pattr_thread_add_SendData, PTHREAD_CREATE_DETACHED);

                    pthread_create(&child_thread_add_SendData, &pattr_thread_add_SendData, thread_add_SendData, _tmp);
                    ///-----------------------------------------------------------------
                }
            }

            df->lockFREE(__LINE__, __FUNCTION__ );
            
        //__LOCK_FREE( &rwlock_download_tasks, __LINE__, __FUNCTION__ );
    }
}
//------------------------------------------------------------------------------
void _errorSend(SendData *sd, uint8_t typedata, void *data)
{
    printf("_errorSend %d\n", typedata);
    
    if( typedata == SEND_DATA_TYPE_CALBACK_DATA___CHECK_MTU )
    {
        CheckMTU_STEP *step = (CheckMTU_STEP *) data;
        
        if( step->count_iteration > 15 )
        {
            printf("..[%s] lask_k=%d\n", sd->address_and_port.c_str(), step->lask_k);
            return;
        }
        
        uint32_t k_old = step->test;
        
        if( step->counter_test > 0 )
        {
            step->test -= ( step->test - step->lask_k ) / 2;
        }
        else
        {
            step->test = step->test / 2;
        }
        
        step->diff = ( k_old - step->test ) / 2;
        
        if( step->diff == 0 )
        {
            printf(".[%s] lask_k=%d\n", sd->address_and_port.c_str(), step->lask_k);
            return;
        }
        
        step->count_iteration++;
        printf("+[%s] test mtu=%d\n", sd->address_and_port.c_str(), step->test);
        
        ///------------------------------------------------------------------
        SendData* _tmp = new SendData();

        _tmp->copyNetInfo(sd);
        
        _tmp->cmd                 = "CHECK_MTU";

        _tmp->add_output_byffer   = (struct buf*)buf_new();

        _tmp->interval_send       = 250;
        _tmp->callback_data       = (void *) ( new CheckMTU_STEP( step ) );
        _tmp->callback_typedata   = SEND_DATA_TYPE_CALBACK_DATA___CHECK_MTU;
        _tmp->callback_okSend     = _okSend;
        _tmp->callback_errorSend  = _errorSend;

        //buf_extend(_tmp->add_output_byffer, step->test);
        uint8_t *test_buf = (uint8_t *) malloc( step->test );
                
        buf_append_data(_tmp->add_output_byffer, test_buf, step->test);

        free(test_buf);

        ///------------------------------------------------------------------
        pthread_attr_t pattr_thread_add_SendData;
        pthread_t child_thread_add_SendData;
        // set thread create attributes
        pthread_attr_init(&pattr_thread_add_SendData);
        pthread_attr_setdetachstate(&pattr_thread_add_SendData, PTHREAD_CREATE_DETACHED);

        pthread_create(&child_thread_add_SendData, &pattr_thread_add_SendData, thread_add_SendData, _tmp);
        ///-----------------------------------------------------------------
    }
    else if( typedata == SEND_DATA_TYPE_CALBACK_DATA___DOWNLOAD_FILE )
    {
        //__LOCK_READ( &rwlock_download_tasks, __LINE__, __FUNCTION__ );
        
            UploadFile *df = (UploadFile *) data;

            if( df == NULL )
            {
                printf("df == NULL %d\n", __LINE__);
                return;
            }

            df->lockWRITE(__LINE__, __FUNCTION__ );
        
            if( ! df->need_remove )
            {
                if( df->status == STATUS_DOWNLOAD_FILE__CHECK_MTU )
                {
                    CheckMTU_STEP *step = (CheckMTU_STEP *) df->mtu;

                    /*if( step->count_iteration > 20 )
                    {
                        printf("..[%s] lask_k=%d\n", sd->address_and_port.c_str(), step->lask_k);
                        df->status = STATUS_DOWNLOAD_FILE__RUN_UPLOAD;
                        df->lockFREE(__LINE__, __FUNCTION__ );
                        return;
                    }*/

                    uint32_t k_old = step->test;

                    if( step->counter_test > 0 )
                    {
                        step->test -= ( step->test - step->lask_k ) / 2;
                    }
                    else
                    {
                        step->test = step->test / 2;
                    }

                    step->diff = ( k_old - step->test ) / 2;

                    if( step->diff == 0 )
                    {
                        printf(".[%s] %d lask_k=%d\n", sd->address_and_port.c_str(), step->count_iteration, step->lask_k);

                        if(step->count_iteration == 0)
                        {
                            df->status = STATUS_DOWNLOAD_FILE__ERROR_NEED_REMOVE_TASK;
                        }
                        else
                        {
                            df->status = STATUS_DOWNLOAD_FILE__RUN_UPLOAD;
                        }

                        df->lockFREE(__LINE__, __FUNCTION__ );
                        return;
                    }

                    step->count_iteration++;
                    //printf("+[%s] mtu=%d\n", sd->address_and_port.c_str(), step->test);

                    ///------------------------------------------------------------------
                    SendData* _tmp = new SendData();

                    for(int f = 0; f < step->params.size(); f++)
                    {
                        _tmp->params.push_back( step->params[f] );
                    }

                    _tmp->copyNetInfo(sd);

                    _tmp->cmd                 = "CHECK_MTU";

                    _tmp->add_output_byffer   = (struct buf*)buf_new();

                    _tmp->interval_send       = 250;
                    _tmp->callback_data       = (void *) ( df );
                    _tmp->callback_typedata   = SEND_DATA_TYPE_CALBACK_DATA___DOWNLOAD_FILE;
                    _tmp->callback_okSend     = _okSend;
                    _tmp->callback_errorSend  = _errorSend;

                    uint8_t *test_buf = (uint8_t *) malloc( step->test );

                    buf_append_data(_tmp->add_output_byffer, test_buf, step->test);

                    free(test_buf);
                    
                    df->counter_references += 1;

                    ///------------------------------------------------------------------
                    pthread_attr_t pattr_thread_add_SendData;
                    pthread_t child_thread_add_SendData;
                    // set thread create attributes
                    pthread_attr_init(&pattr_thread_add_SendData);
                    pthread_attr_setdetachstate(&pattr_thread_add_SendData, PTHREAD_CREATE_DETACHED);

                    pthread_create(&child_thread_add_SendData, &pattr_thread_add_SendData, thread_add_SendData, _tmp);
                    ///-----------------------------------------------------------------
                }
                else if( df->status == STATUS_DOWNLOAD_FILE__RUN_UPLOAD )
                {
                    df->need_remove = true;
                }
            }

            df->lockFREE(__LINE__, __FUNCTION__ );
        
        //__LOCK_FREE( &rwlock_download_tasks, __LINE__, __FUNCTION__ );
    }
}
////////////////////////////////////////////////////////////////////////////////
/*
 * error - wrapper for perror
 */
void error(char *msg) 
{
    perror(msg);
    exit(1);
}
////////////////////////////////////////////////////////////////////////////////
int main(int argc, char** argv) 
{
    struct sockaddr_in si_me, si_other;

    int s, recv_len;
    socklen_t slen = sizeof(si_other);
    unsigned char buf[UDP_BUFLEN];
    
    ///-----------------------------------------------------------------
    char path_save[512] = "";
    char *p;
    //string s;
    char tm_pstr[512] = "";

    if(!(p = strrchr(argv[0], '/')))
    {
        getcwd(tm_pstr, sizeof(tm_pstr));
    }
    else
    {
        char path_save[512] = "";

        *p = '\0';
        getcwd(path_save, sizeof(path_save));
        chdir(argv[0]);
        getcwd(tm_pstr, sizeof(tm_pstr));
        chdir(path_save);
    }
    
    snprintf(abs_exe_path, 512, "%s/../../SERVER", tm_pstr);
    ///-----------------------------------------------------------------
    
#if defined(IP_DONTFRAG) || defined(IP_MTU_DISCOVER)
        int optval;
#endif
        
    time_t timer;
    char print_buffer[26];
    struct tm* tm_info;

    timer   = time(NULL);
    tm_info = localtime(&timer);

    strftime(print_buffer, 26, "%d.%m.%Y %H:%M:%S", tm_info);
    printf("VERSION: %s Start: %s\n", VERSION, print_buffer);

    //create a UDP socket
    if ( ( s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP) ) == -1)
    {
        printf("socket error thread_udp_1");
        pthread_exit(0);
    }

#if defined(IP_DONTFRAG)
        optval = 1;
        setsockopt(s, IPPROTO_IP, IP_DONTFRAG,
                   (const void *) &optval, sizeof(optval));
#elif defined(IP_MTU_DISCOVER)
        optval = IP_PMTUDISC_DO;
        setsockopt(s, IPPROTO_IP, IP_MTU_DISCOVER,
                   (const void *) &optval, sizeof(optval));
#endif
        
    /* setsockopt: Handy debugging trick that lets 
     * us rerun the server immediately after we kill it; 
     * otherwise we have to wait about 20 secs. 
     * Eliminates "ERROR on binding: Address already in use" error. 
     */
    optval = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval , sizeof(int));
    
    // zero out the structure
    memset((char *) &si_me, 0, sizeof(si_me));

    si_me.sin_family       = AF_INET;
    si_me.sin_port         = htons( UDP_PORT );
    si_me.sin_addr.s_addr  = htonl(INADDR_ANY);
    
    //bind socket to port
    if( bind(s, (struct sockaddr*)&si_me, sizeof(si_me) ) == -1)
    {
        printf("bind");
        pthread_exit(0);
    }
    /// ***********************************************************************
    pthread_rwlock_init(&rwlock__queue_send, NULL);
    pthread_rwlock_init(&rwlock_wait_send_socket, NULL);
    pthread_rwlock_init(&rwlock__register_input, NULL);
    pthread_rwlock_init(&rwlock_session_users, NULL);
    pthread_rwlock_init(&rwlock_download_tasks, NULL);
    
    ///------------------------------------------------------------------
    pthread_attr_t pattr_thread_udp_queue_1;
    pthread_t child_thread_udp_queue_1;
    // set thread create attributes
    pthread_attr_init(&pattr_thread_udp_queue_1);
    pthread_attr_setdetachstate(&pattr_thread_udp_queue_1, PTHREAD_CREATE_DETACHED);

    pthread_create(&child_thread_udp_queue_1, &pattr_thread_udp_queue_1, thread_queue_output, NULL);
    ///------------------------------------------------------------------
    
    ///------------------------------------------------------------------
    pthread_attr_t pattr_thread_bk;
    pthread_t child_thread_bk;
    // set thread create attributes
    pthread_attr_init(&pattr_thread_bk);
    pthread_attr_setdetachstate(&pattr_thread_bk, PTHREAD_CREATE_DETACHED);

    pthread_create(&child_thread_bk, &pattr_thread_bk, thread_bk, NULL);
    ///------------------------------------------------------------------
    
    ///------------------------------------------------------------------
    pthread_attr_t pattr_thread_download;
    pthread_t child_thread_download;
    // set thread create attributes
    pthread_attr_init(&pattr_thread_download);
    pthread_attr_setdetachstate(&pattr_thread_download, PTHREAD_CREATE_DETACHED);

    pthread_create(&child_thread_download, &pattr_thread_download, thread_upload, NULL);
    ///------------------------------------------------------------------

    for(int f = 0; f < COUNT_WORKERS; f++)
    {
        workersIO[f].index = f;

        pthread_attr_t pattr55;
        // set thread create attributes
        pthread_attr_init(&pattr55);
        pthread_attr_setdetachstate(&pattr55, PTHREAD_CREATE_DETACHED);

        int *arg = (int *) malloc(sizeof(*arg));

        if ( arg == NULL )
        {
            fprintf(stderr, "Couldn't allocate memory for thread arg.\n");
            exit(EXIT_FAILURE);
        }

        *arg = workersIO[f].index;

        pthread_create(&workersIO[f].th, &pattr55, thread_worker, arg);
    }
    
    int index_worker = 0;
    struct hostent *hostp;  /* client host info */
    char *hostaddrp;        /* dotted decimal host addr string */
    
    while(1)
    {
        //memset((unsigned char *) buf, 0, UDP_BUFLEN);

        if ((recv_len = recvfrom(s, buf, UDP_BUFLEN, 0, (struct sockaddr *) &si_other, &slen)) == -1)
        {
            printf("recvfrom error 1\n");
            continue;
        }
        
        if( recv_len < 4 )
        {
            continue;
        }

        /* 
         * gethostbyaddr: determine who sent the datagram
         */
        /*hostp = gethostbyaddr((const char *)&si_other.sin_addr.s_addr
                               , sizeof(si_other.sin_addr.s_addr), AF_INET);
        if (hostp == NULL)
        {
            error("ERROR on gethostbyaddr");
        }
       
        hostaddrp = inet_ntoa( si_other.sin_addr );
        if (hostaddrp == NULL)
        {
            error("ERROR on inet_ntoa\n");
        }
        
        printf("server received datagram from %s (%s)\n", hostp->h_name, hostaddrp);*/
        
        //char str_addres_and_port[128] = "";
        //string key_map_readers = "";

        //snprintf( str_addres_and_port, 128, "%s:%d", inet_ntoa(si_other.sin_addr), ntohs(si_other.sin_port) );

        //key_map_readers.append(str_addres_and_port);
        //printf("received packet from %s:%d\n", inet_ntoa(si_other.sin_addr), ntohs(si_other.sin_port));

        UdpRcvSocket_READER *_rcv_info = new UdpRcvSocket_READER();

        memcpy(&_rcv_info->si_other, &si_other, sizeof(struct sockaddr_in));
        _rcv_info->soket       =  s;
        _rcv_info->slen        =  slen;
        _rcv_info->buf_len     =  recv_len - 2;
        _rcv_info->buf         =  (uint8_t *) malloc( _rcv_info->buf_len );
        
        uint16_t random_value = 0;
        memcpy(&random_value, buf, 2 );
        
        memcpy(_rcv_info->buf, buf + 2, _rcv_info->buf_len );
        
        _rcv_info->address_and_port = string_format(
                        "%d:%s:%d"
                      , random_value
                      , inet_ntoa(si_other.sin_addr)
                      , ntohs(si_other.sin_port)
                );
        
        //printf("%d:%s:%d:%s\n", _rcv_info->soket, inet_ntoa(_rcv_info->si_other.sin_addr) , ntohs(_rcv_info->si_other.sin_port), _rcv_info->address_and_port.c_str());
        
        //textcolor(BRIGHT, GREEN, BLACK);
        //printf("UdpRcvSocket_READER: [read len: %d] %s\n", _rcv_info->buf_len, _rcv_info->address_and_port.c_str());
        //textcolor(RESET, WHITE, BLACK);
        
        workersIO[index_worker].lockWRITE(__LINE__, __FUNCTION__);

        workersIO[index_worker].queue_udp_command.push_back(_rcv_info);
        
        workersIO[index_worker].lockFREE(__LINE__, __FUNCTION__);
        
        index_worker += 1;

        if( index_worker >= COUNT_WORKERS )
        {
            index_worker = 0;
        }
    }

    return 0;
}
///----------------------------------------------------------------------------------------------------------
bool __exec_h(ReadCommand *data)
{
    string cmd = data->cmd;

    if(strcasecmp(cmd.c_str(), "OK_SEND") == 0) /// принят запрос подтверждения
    {
        __LOCK_READ( &rwlock__queue_send, __LINE__, __FUNCTION__ );

        for(unsigned int i = 0, len = queue_send.size(); i < len; i++)
        {
            SendData* _item = queue_send[ i ];
            
            _item->lockWRITE(__LINE__, __FUNCTION__); /// запись

            if(
                      _item->need_remove == false
                   && _item->timestamp   == data->timestamp
                   && _item->address_and_port.compare(data->udp_info->address_and_port) == 0
              )
            {
                //_item->OK_SEND = true;
                _item->need_remove = true;
                
                if( _item->callback_okSend != NULL )
                {
                    _item->callback_okSend(_item, _item->callback_typedata, _item->callback_data);
                }
            }

            _item->lockFREE(__LINE__, __FUNCTION__);
        }

        __LOCK_FREE( &rwlock__queue_send, __LINE__, __FUNCTION__ );

        return true; // is OK_SEND
    }

    ///-------------------------------------------------------------------------------------------
    int sended_length = 0;
    /// отправить подтверждение
    _send_data_from_socket_crypt_encode(
              data->udp_info->soket
            , data->udp_info->si_other
            , data->udp_info->slen
            
            , &sended_length
            , data->timestamp
            , "OK_SEND"
            , default_xor_key);

    ///-------------------------------------------------------------------------------------------
    __LOCK_READ( &rwlock__register_input, __LINE__, __FUNCTION__ ); /// чтение

    bool founded_input = false;

    for(unsigned int i = 0, len = register_input.size(); i < len; i++ )
    {
        InputCmdReg *_tmp = register_input[i];

        if(
                _tmp->timestamp == data->timestamp
             && _tmp->address_and_port.compare(data->udp_info->address_and_port) == 0
          )
        {
            founded_input = true;
            break;
        }
    }

    __LOCK_FREE( &rwlock__register_input, __LINE__, __FUNCTION__ );

    /// ---- дублирование ----
    if( founded_input )
    {
        printf("diplicate %s %lu\n", data->udp_info->address_and_port.c_str(), data->timestamp);
        return true;
    }
    else
    {
        InputCmdReg *_tmp       = new InputCmdReg();
        _tmp->address_and_port  = data->udp_info->address_and_port;
        _tmp->timestamp         = data->timestamp;

        __LOCK_WRITE( &rwlock__register_input, __LINE__, __FUNCTION__ );

        register_input.push_back( _tmp );

        __LOCK_FREE( &rwlock__register_input, __LINE__, __FUNCTION__ );
    }

    return false;
}
//******************************************************************************
void deleteSession(string address_and_port)
{
    __LOCK_WRITE(&rwlock_session_users, __LINE__, __FUNCTION__);

    map<string, SessionUser*>::iterator _it = SESSION_USERS.find( address_and_port );

    if( _it != SESSION_USERS.end() )
    {
        delete _it->second;
        SESSION_USERS.erase(_it);
    }

    __LOCK_FREE(&rwlock_session_users, __LINE__, __FUNCTION__);
    
    
    __LOCK_READ( &rwlock_download_tasks, __LINE__, __FUNCTION__ );
        
    for(unsigned int i = 0, __len = UPLOAD_TASKS.size(); i < __len; i++)
    {
        UploadFile *_item = UPLOAD_TASKS[ i ];
        
        _item->lockWRITE(__LINE__, __FUNCTION__ );
        
        if( _item->address_and_port.compare( address_and_port ) == 0 )
        {
            _item->need_remove = true;
            _item->lockFREE(__LINE__, __FUNCTION__ );
            break;
        }
        
        _item->lockFREE(__LINE__, __FUNCTION__ );
        
        msleep(1);
    }
    
    __LOCK_FREE( &rwlock_download_tasks, __LINE__, __FUNCTION__ );
    
    
    __LOCK_READ( &rwlock__queue_send, __LINE__, __FUNCTION__ );

    for(unsigned int i = 0, __len = queue_send.size(); i < __len; i++)
    {
        SendData* _item = queue_send[ i ];

        _item->lockWRITE(__LINE__, __FUNCTION__ );

        if( _item->address_and_port.compare( address_and_port ) == 0 )
        {
            _item->need_remove = true;
        }
        
        _item->lockFREE(__LINE__, __FUNCTION__ );
        
        msleep(1);
    }

    __LOCK_FREE( &rwlock__queue_send, __LINE__, __FUNCTION__ );
    
    //--------------------------------------------------------------------------
}
//******************************************************************************
void _exec(ReadCommand *data, SessionUser *user)
{
    if(data == NULL)
    {
        printf("NULL data\n");
        return;
    }
    
    if( data->cmd.compare("OK_SEND") != 0 )
    {
        textcolor(BRIGHT, RED, BLACK);
        printf( "_exec: %s\n", data->cmd.c_str() );//, data->timestamp, data->udp_info->address_and_port.c_str() );
        textcolor(RESET, WHITE, BLACK);
    }
    
    if( __exec_h( data ) )
    {
        return;
    }
    
    printf("cmd: %s\n", data->cmd.c_str());
    
    /*if(data->params.size() > 0)
    {
        printf("count parameters: %d\n", data->params.size());
        for(uint32_t i = 0; i < data->params.size(); i++)
        {
            printf("[%d] %s\n", i + 1, data->params[i].c_str());
        }
    }*/
    
    char *cmd = (char*) data->cmd.c_str();
    
    if( strcmp(cmd, "SEND_TEXT_MESSAGE") == 0 )
    {
        for(int k = 0; k < 100; k++)
        {
            //{
                ///------------------------------------------------------------------
                SendData* _tmp = new SendData();

                _tmp->copyNetInfo(data->udp_info);
                /*memcpy(&_tmp->si_other, &data->udp_info->si_other, sizeof(struct sockaddr_in));
                _tmp->soket            = data->udp_info->soket;
                _tmp->slen             = data->udp_info->slen;
                _tmp->address_and_port = data->udp_info->address_and_port;*/

                _tmp->cmd       = string_format("FIRST_CMD_%d", k);

                _tmp->add_output_byffer = (struct buf*)buf_new();

                buf_append_u32(_tmp->add_output_byffer, data->udp_info->soket);


                printf("%d:%s:%d:%s\n", data->udp_info->soket, inet_ntoa(data->udp_info->si_other.sin_addr) , ntohs(data->udp_info->si_other.sin_port), data->udp_info->address_and_port.c_str());
                printf("%d:%s:%d:%s\n", _tmp->soket, inet_ntoa(_tmp->si_other.sin_addr) , ntohs(_tmp->si_other.sin_port), _tmp->address_and_port.c_str());

                __LOCK_WRITE(&rwlock__queue_send, __LINE__, __FUNCTION__);
                
                __inc_totalUseMemoryQueueSend(_tmp);

                _tmp->timestamp = get_new_timestamp(false);
                queue_send.push_back( _tmp );

                __LOCK_FREE( &rwlock__queue_send, __LINE__, __FUNCTION__ );
                ///------------------------------------------------------------------
            //}
        }
    }
    else if( strcmp(cmd, "START_SEND_FILE") == 0 && data->params.size() == 1 )
    {
        string path = string_format("/home/sk/Downloads/%s.data", data->udp_info->address_and_port.c_str());
        
        printf("%s\n", path.c_str());
        
        FILE *fp;
        
        if ( (fp = fopen(path.c_str(), "wb")) == NULL ) 
        {
            printf("Cannot open file.\n");
        }
        else
        {
            ftruncate(fileno(fp), atoi( data->params[0].c_str() ) );
            fclose(fp);
        }
    }
    else if( strcmp(cmd, "SEND_FILE") == 0 && data->params.size() == 0 )
    {
        long pos = 0;
        memcpy(&pos, data->buffer, 4);
        
        string path = string_format("/home/sk/Downloads/%s.data", data->udp_info->address_and_port.c_str());
        
        FILE *fp;
        
        if ( (fp = fopen(path.c_str(), "r+b")) == NULL )
        {
            printf("Cannot open file.\n");
        }
        else
        {
            fseek(fp, pos, SEEK_SET);
            fwrite(data->buffer + 4, 1, data->len_buffer - 4, fp);
            fclose(fp);
        }
    }
    else if( strcmp(cmd, "STOP_APP") == 0 && data->params.size() == 0 )
    {
        deleteSession( data->udp_info->address_and_port );
    }
    else if( strcmp(cmd, "ECHO") == 0 )
    {
        
    }
    else if( strcmp(cmd, "DOWNLOAD_FILE") == 0 && data->params.size() == 2 )
    {
        string path     = string_format("%s/tmp/%s", abs_exe_path, data->params[0].c_str());
        bool file_exist = file_exists(path);
        
        printf("path: [%s] %s [%s]\n", file_exist ? "exist" : "no exist!" , path.c_str(), data->params[1].c_str() );
        
        if( file_exist )
        {
            // отправить ответ --> OK
            ///-----------------------------------------------------------------
            SendData* _tmp = new SendData();

            _tmp->copyNetInfo(data->udp_info);
            /*memcpy(&_tmp->si_other, &data->udp_info->si_other, sizeof(struct sockaddr_in));
            _tmp->soket            = data->udp_info->soket;
            _tmp->slen             = data->udp_info->slen;
            _tmp->address_and_port = data->udp_info->address_and_port;*/

            _tmp->cmd       = "DOWNLOAD_FILE";
            _tmp->params.push_back("OK");
            _tmp->params.push_back(data->params[0]);
            _tmp->params.push_back( string_format("%d", get_file_size(path.c_str())) );

            __LOCK_WRITE(&rwlock__queue_send, __LINE__, __FUNCTION__);

            __inc_totalUseMemoryQueueSend(_tmp);

            _tmp->timestamp = get_new_timestamp(false);
            queue_send.push_back( _tmp );

            __LOCK_FREE( &rwlock__queue_send, __LINE__, __FUNCTION__ );
            ///-----------------------------------------------------------------
            
            // добавляем задачу
            
            UploadFile *df = new UploadFile();
            
            df->copyNetInfo(data->udp_info);
            df->file = fopen(path.c_str(), "rb");
            
            df->params.push_back( path );
            df->params.push_back( data->params[1] );
            
            __LOCK_WRITE( &rwlock_download_tasks, __LINE__, __FUNCTION__ );
         
            UPLOAD_TASKS.push_back(df);
         
            __LOCK_FREE( &rwlock_download_tasks, __LINE__, __FUNCTION__ );
        }
        else
        {
            ///-----------------------------------------------------------------
            SendData* _tmp = new SendData();

            _tmp->copyNetInfo(data->udp_info);
            /*memcpy(&_tmp->si_other, &data->udp_info->si_other, sizeof(struct sockaddr_in));
            _tmp->soket            = data->udp_info->soket;
            _tmp->slen             = data->udp_info->slen;
            _tmp->address_and_port = data->udp_info->address_and_port;*/

            _tmp->cmd       = "DOWNLOAD_FILE";
            _tmp->params.push_back("ERROR");

            __LOCK_WRITE(&rwlock__queue_send, __LINE__, __FUNCTION__);

            __inc_totalUseMemoryQueueSend(_tmp);

            _tmp->timestamp = get_new_timestamp(false);
            queue_send.push_back( _tmp );

            __LOCK_FREE( &rwlock__queue_send, __LINE__, __FUNCTION__ );
            ///-----------------------------------------------------------------
        }
    }
    
}
